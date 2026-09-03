/* identity.c —— 内嵌设备身份证书/私钥的 DER 字节与解析入口。
 * 字节来自 id_cert.inc / id_key.inc（随固件内嵌、全安装实例共用），
 * 有效期 2026-09 ~ 2054-01，证书与私钥已验证为匹配的自签 RSA-2048。
 * 除生成身份外不要改动两个 .inc 的字节。 */
#include "identity.h"

#include <string.h>
#include <mbedtls/sha256.h>
#include "id_cert.inc"
#include "id_key.inc"

int identity_cert_parse(mbedtls_x509_crt *crt)
{
    return mbedtls_x509_crt_parse_der(crt, id_cert, id_cert_len);
}

int identity_key_parse(mbedtls_pk_context *pk)
{
    /* 未加密 PKCS#8：无需口令与 RNG */
    return mbedtls_pk_parse_key(pk, id_key, id_key_len, NULL, 0, NULL, NULL);
}

int identity_fingerprint(char out[65])
{
    mbedtls_x509_crt crt;
    unsigned char dig[32];
    int ret, i;

    mbedtls_x509_crt_init(&crt);
    ret = mbedtls_x509_crt_parse_der(&crt, id_cert, id_cert_len);
    if (ret != 0)
        goto out;
    ret = mbedtls_sha256(crt.raw.p, crt.raw.len, dig, 0);
    if (ret != 0)
        goto out;
    for (i = 0; i < 32; i++) {
        out[i * 2]     = "0123456789ABCDEF"[dig[i] >> 4];
        out[i * 2 + 1] = "0123456789ABCDEF"[dig[i] & 0xF];
    }
    out[64] = 0;
out:
    mbedtls_x509_crt_free(&crt);
    return ret;
}
