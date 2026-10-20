/* identity.h —— per-device 设备身份证书/私钥。
 *
 * 2026 版 LocalSend 的 HTTPS 服务器强制 mTLS：客户端不出示证书会收到
 * fatal alert certificate_required。因此出站 HTTPS 必须带本机自签证书。
 *
 * 身份优先从 ux0:data/psvsend/identity.{crt,key} 读取（首启后台生成
 * RSA-2048 自签证书）；生成失败才回退固件内嵌证书（兼容兜底，
 * 那份私钥是公开的，不能当身份用）。
 *
 * 生成期间 identity_pending() 为真；网络侧调用 identity_cert_parse /
 * identity_key_parse 会先等它就绪（有上限）。生成完成后无需重启任何模块：
 * 每次 TLS 握手、每轮 announce 都重新取。 */
#ifndef PSVSEND_IDENTITY_H
#define PSVSEND_IDENTITY_H

#include <stdbool.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>

/* 启动时调用一次：读盘校验，失败则起后台线程生成。 */
void identity_init(void);

/* 首启证书是否仍在生成。为真时 api 层暂缓启动 http/发现（避免用旧指纹公告）。 */
bool identity_pending(void);

/* 解析当前身份证书/私钥（DER）。返回 0=成功；非 0=mbedTLS 错误码。 */
int identity_cert_parse(mbedtls_x509_crt *crt);
int identity_key_parse(mbedtls_pk_context *pk);

/* 当前身份证书指纹（DER SHA-256，大写 hex，含 '\0'）。
 * 返回 0=成功；-1=首启仍在生成（调用方可回退 config 指纹或跳过公告）。 */
int identity_fingerprint(char out[65]);

#endif
