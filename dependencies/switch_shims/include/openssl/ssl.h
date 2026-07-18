#pragma once
#include <openssl/_ossl_compat.h>

// OpenSSL SSL_CTX subset mapped to libcurl's mbedTLS configuration.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef int (*SSL_verify_cb)(int preverify_ok, X509_STORE_CTX* ctx);

#define SSL_MODE_AUTO_RETRY 0x00000004L
#define SSL_VERIFY_NONE 0x00
#define SSL_VERIFY_PEER 0x01

long SSL_CTX_set_mode(SSL_CTX* ctx, long mode);
void SSL_CTX_set_verify(SSL_CTX* ctx, int mode, SSL_verify_cb cb);
void SSL_CTX_set_verify_depth(SSL_CTX* ctx, int depth);
int SSL_CTX_set_cipher_list(SSL_CTX* ctx, const char* str);
X509_STORE* SSL_CTX_get_cert_store(const SSL_CTX* ctx);
int SSL_CTX_use_certificate(SSL_CTX* ctx, X509* x);
int SSL_CTX_use_RSAPrivateKey(SSL_CTX* ctx, RSA* rsa);
int SSL_CTX_check_private_key(const SSL_CTX* ctx);
int ECDH_compute_key(void* out, size_t outlen, const EC_POINT* pub_key, EC_KEY* ecdh, void* kdf);

#ifdef __cplusplus
}
#endif
