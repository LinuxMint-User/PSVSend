/* identity.c —— per-device 身份证书/私钥（首启后台生成，落盘复用）。
 *
 * 背景：2026 版 LocalSend 的 HTTPS 服务器强制 mTLS，客户端必须出示证书。
 * 旧方案把一张证书内嵌进固件，全安装实例共用——private key 公开在仓库里，
 * 既不能证明"是哪台设备"，也会让两台 PSV 互判为"自己"。
 *
 * 现在的优先级：
 *   1) ux0:data/psvsend/identity.crt / identity.key（首启生成，RSA-2048 自签）；
 *   2) 固件内嵌 id_cert.inc / id_key.inc（仅作生成失败时的兼容兜底）。
 *
 * 首启生成在后台线程进行（实测 2~9 秒），期间 identity_pending() 为真，
 * api 层暂缓启动 http / 发现；生成完成后各调用点（每次 TLS 握手、每轮
 * announce）从本模块现取，自动改用新身份——不需要重启任何模块。 */
#include "identity.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <mbedtls/sha256.h>
#include <mbedtls/rsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include "core/dlog.h"
#include "id_cert.inc"
#include "id_key.inc"

#define ID_DIR       "ux0:data/psvsend"
#define ID_CRT_PATH  ID_DIR "/identity.crt"
#define ID_KEY_PATH  ID_DIR "/identity.key"
#define ID_MAX_CERT  2048        /* 实测自签 RSA-2048 证书约 815B，留足余量 */
#define ID_MAX_KEY   4096        /* PKCS#8 RSA-2048 私钥约 1.2KB */
#define ID_STACK     0x20000
#define ID_WAIT_MS   15000       /* 网络侧等身份就绪的上限（首启实测 2~9s） */

enum { ID_PENDING = 0, ID_READY = 1, ID_FALLBACK = 2 };

static unsigned char s_cert[ID_MAX_CERT];
static size_t        s_cert_len;
static unsigned char s_key[ID_MAX_KEY];
static size_t        s_key_len;
static char          s_fp[65];
static volatile int  s_state = ID_PENDING;
static SceUID        s_mtx = -1;
static int           s_inited = 0;

static void id_lock(void)
{
    if (s_mtx >= 0) sceKernelLockMutex(s_mtx, 1, NULL);
}

static void id_unlock(void)
{
    if (s_mtx >= 0) sceKernelUnlockMutex(s_mtx, 1);
}

static void hex_fp(const unsigned char *der, size_t len, char out[65])
{
    unsigned char dig[32];
    int i;
    if (mbedtls_sha256(der, len, dig, 0) != 0) {
        out[0] = 0;
        return;
    }
    for (i = 0; i < 32; i++) {
        out[i * 2]     = "0123456789ABCDEF"[dig[i] >> 4];
        out[i * 2 + 1] = "0123456789ABCDEF"[dig[i] & 0xF];
    }
    out[64] = 0;
}

/* ---------- 文件读写（tmp + rename，防断电留半张） ---------- */

static int read_all(const char *path, unsigned char *buf, size_t cap, size_t *out)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int n;
    if (fd < 0) return -1;
    n = sceIoRead(fd, buf, (unsigned)cap);
    sceIoClose(fd);
    if (n <= 0) return -1;
    *out = (size_t)n;
    return 0;
}

static int write_all(const char *path, const unsigned char *data, size_t len)
{
    char tmp[96];
    SceUID fd;
    int n;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0600);
    if (fd < 0) return -1;
    n = sceIoWrite(fd, data, (unsigned)len);
    sceIoClose(fd);
    if (n != (int)len) {
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(path);               /* 部分 FS 的 rename 不覆盖已存在文件 */
    return sceIoRename(tmp, path);
}

/* ---------- 证书/私钥配对校验 ---------- */

static int identity_valid(const unsigned char *cert, size_t clen,
                          const unsigned char *key, size_t klen)
{
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    unsigned char pub[600];
    int n, ok = -1;

    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);
    if (mbedtls_x509_crt_parse_der(&crt, cert, clen) != 0)
        goto out;
    if (mbedtls_pk_parse_key(&pk, key, klen, NULL, 0, NULL, NULL) != 0)
        goto out;
    /* 公钥必须与证书里的 SubjectPublicKeyInfo 完全一致 */
    n = mbedtls_pk_write_pubkey_der(&pk, pub, sizeof pub);
    if (n <= 0 || (size_t)n != crt.pk_raw.len)
        goto out;
    if (memcmp(pub + sizeof pub - n, crt.pk_raw.p, (size_t)n) != 0)
        goto out;
    ok = 0;
out:
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    return ok;
}

/* ---------- 发布到共享缓存 ---------- */

static void identity_publish(const unsigned char *cert, size_t clen,
                             const unsigned char *key, size_t klen, int state)
{
    char fp[65];

    hex_fp(cert, clen, fp);
    id_lock();
    memcpy(s_cert, cert, clen);
    s_cert_len = clen;
    memcpy(s_key, key, klen);
    s_key_len = klen;
    memcpy(s_fp, fp, sizeof fp);
    s_state = state;
    id_unlock();
}

static void identity_use_embedded(void)
{
    identity_publish(id_cert, id_cert_len, id_key, id_key_len, ID_FALLBACK);
}

/* ---------- 生成自签证书 ---------- */

static uint64_t g_seed;

static int id_entropy_poll(void *arg, unsigned char *out, size_t len,
                           size_t *olen)
{
    uint64_t x;
    size_t i, k;
    (void)arg;

    /* Vita 无用户态硬件 RNG：优先用 libc getentropy（实测可用），
     * 失败再用时间/地址混洗兜底。 */
    if (getentropy(out, len) == 0) {
        *olen = len;
        return 0;
    }
    x = (uint64_t)sceKernelGetSystemTimeWide()
      ^ ((uint64_t)(uintptr_t)&g_seed << 17)
      ^ ((uint64_t)(g_seed++) * 0x9E3779B97F4A7C15ull);
    for (i = 0; i < len; i += 8) {
        uint64_t z = x + 0x9E3779B97F4A7C15ull;
        x = z ^ (x << 13) ^ (x >> 7);
        k = (len - i < 8) ? (len - i) : 8;
        memcpy(out + i, &z, k);
    }
    *olen = len;
    return 0;
}

/* 生成一对 RSA-2048 自签证书；成功返回 0，证书/私钥 DER 写入 cert/key。 */
static int identity_generate(unsigned char *cert, size_t *clen,
                             unsigned char *key, size_t *klen)
{
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    unsigned char serial[16];
    int ret = -1, n;

    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);

    if (mbedtls_entropy_add_source(&ent, id_entropy_poll, NULL, 48,
                                   MBEDTLS_ENTROPY_SOURCE_STRONG) != 0)
        goto out;
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char *)"psvsend-identity", 16) != 0)
        goto out;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
        goto out;
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), mbedtls_ctr_drbg_random,
                            &drbg, 2048, 65537) != 0)
        goto out;

    /* 私钥：未加密 PKCS#8。mbedTLS 把结果写在缓冲末尾。 */
    n = mbedtls_pk_write_key_der(&pk, key, ID_MAX_KEY);
    if (n <= 0)
        goto out;
    memmove(key, key + ID_MAX_KEY - n, (size_t)n);
    *klen = (size_t)n;

    /* 证书字段对齐旧内嵌证书：CN=PS Vita / O=psvsend、SHA-256、CA:TRUE、
     * SKI/AKI；有效期给到 2055，避免真机时钟不准导致 notBefore 未到。 */
    if (getentropy(serial, sizeof serial) != 0)
        memset(serial, 0x5A, sizeof serial);
    serial[0] &= 0x7F;               /* 正整数 */

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=PS Vita,O=psvsend") != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, "CN=PS Vita,O=psvsend") != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial) != 0 ||
        mbedtls_x509write_crt_set_validity(&crt, "20200101000000",
                                           "20550101000000") != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0) != 0 ||
        mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0 ||
        mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0)
        goto out;

    n = mbedtls_x509write_crt_der(&crt, cert, ID_MAX_CERT,
                                  mbedtls_ctr_drbg_random, &drbg);
    if (n <= 0)
        goto out;
    memmove(cert, cert + ID_MAX_CERT - n, (size_t)n);
    *clen = (size_t)n;
    ret = 0;
out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ret;
}

static int identity_gen_thread(SceSize args, void *argp)
{
    static unsigned char cert[ID_MAX_CERT];
    static unsigned char key[ID_MAX_KEY];
    size_t clen = 0, klen = 0;
    uint64_t t0 = (uint64_t)sceKernelGetSystemTimeWide();
    uint64_t ms;

    (void)args; (void)argp;

    if (identity_generate(cert, &clen, key, &klen) == 0) {
        ms = ((uint64_t)sceKernelGetSystemTimeWide() - t0) / 1000;
        identity_publish(cert, clen, key, klen, ID_READY);
        dlog("identity: generated rsa2048 in %llu ms (cert %d B, key %d B)",
             (unsigned long long)ms, (int)clen, (int)klen);
        if (write_all(ID_KEY_PATH, key, klen) == 0 &&
            write_all(ID_CRT_PATH, cert, clen) == 0)
            dlog("identity: saved to %s", ID_DIR);
        else
            dlog("identity: SAVE FAILED (using in-memory for this session)");
    } else {
        dlog("identity: generation FAILED -> embedded fallback");
        identity_use_embedded();
    }
    return sceKernelExitDeleteThread(0);
}

/* ---------- 对外接口 ---------- */

void identity_init(void)
{
    unsigned char cert[ID_MAX_CERT], key[ID_MAX_KEY];
    size_t clen = 0, klen = 0;
    SceUID t;

    if (s_inited) return;
    s_inited = 1;

    s_mtx = sceKernelCreateMutex("psvsend_id", 0, 0, NULL);
    if (s_mtx < 0)
        dlog("identity: mutex create FAILED 0x%08X", (unsigned)s_mtx);

    if (read_all(ID_CRT_PATH, cert, sizeof cert, &clen) == 0 &&
        read_all(ID_KEY_PATH, key, sizeof key, &klen) == 0 &&
        identity_valid(cert, clen, key, klen) == 0) {
        identity_publish(cert, clen, key, klen, ID_READY);
        dlog("identity: loaded fp=%s", s_fp);
        return;
    }

    /* 没有 / 不完整 / 不配对：清掉残留后重新生成 */
    dlog("identity: no valid identity on disk -> generating (first run)");
    sceIoRemove(ID_CRT_PATH);
    sceIoRemove(ID_KEY_PATH);

    t = sceKernelCreateThread("psvsend_identity", identity_gen_thread,
                              0x40, ID_STACK, 0, 0, NULL);
    if (t < 0) {
        dlog("identity: create thread FAILED 0x%08X -> embedded fallback",
             (unsigned)t);
        identity_use_embedded();
        return;
    }
    if (sceKernelStartThread(t, 0, NULL) < 0) {
        dlog("identity: start thread FAILED -> embedded fallback");
        sceKernelDeleteThread(t);
        identity_use_embedded();
    }
}

bool identity_pending(void)
{
    return s_state == ID_PENDING;
}

int identity_fingerprint(char out[65])
{
    if (s_state == ID_PENDING)
        return -1;
    id_lock();
    memcpy(out, s_fp, sizeof s_fp);
    id_unlock();
    return 0;
}

/* 网络侧（每次 TLS 握手）取身份：首启生成期间最多等 ID_WAIT_MS。
 * 超时后按内嵌证书兜底，避免把连接线程永久挂住。 */
static bool identity_wait_ready(void)
{
    int waited = 0;
    while (s_state == ID_PENDING && waited < ID_WAIT_MS) {
        sceKernelDelayThread(50000);
        waited += 50;
    }
    return s_state != ID_PENDING;
}

int identity_cert_parse(mbedtls_x509_crt *crt)
{
    int r;
    if (!identity_wait_ready())
        return -1;
    id_lock();
    r = mbedtls_x509_crt_parse_der(crt, s_cert, s_cert_len);
    id_unlock();
    return r;
}

int identity_key_parse(mbedtls_pk_context *pk)
{
    int r;
    if (!identity_wait_ready())
        return -1;
    id_lock();
    r = mbedtls_pk_parse_key(pk, s_key, s_key_len, NULL, 0, NULL, NULL);
    id_unlock();
    return r;
}
