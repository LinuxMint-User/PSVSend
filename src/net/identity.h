/* identity.h —— psvsend 设备身份证书/私钥。
 * 2026 版 LocalSend 的 HTTPS 服务器强制 mTLS：客户端不出示证书直接
 * fatal alert certificate_required。故出站 HTTPS（发文件）必须带本机
 * 自签证书（RSA-2048，CN=PS Vita / O=psvsend，DER 内嵌于 id_cert.inc /
 * id_key.inc）。接收端只要求"证书本身有效"，不做指纹比对。 */
#ifndef PSVSEND_IDENTITY_H
#define PSVSEND_IDENTITY_H

#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>

/* 解析内嵌身份证书/私钥（DER）。返回 0=成功；非 0=mbedTLS 错误码。 */
int identity_cert_parse(mbedtls_x509_crt *crt);
int identity_key_parse(mbedtls_pk_context *pk);

/* 身份证书指纹（DER SHA-256，大写 hex，含 '\0'）。返回 0=成功。 */
int identity_fingerprint(char out[65]);

#endif
