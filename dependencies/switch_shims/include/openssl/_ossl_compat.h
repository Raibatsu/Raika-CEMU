#pragma once

// OpenSSL 1.1 compatibility subset backed by mbedTLS.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENSSL_VERSION_NUMBER 0x1010100fL

// Digests
#define SHA_DIGEST_LENGTH 20
#define SHA1_DIGEST_LENGTH 20
#define SHA224_DIGEST_LENGTH 28
#define SHA256_DIGEST_LENGTH 32
#define SHA384_DIGEST_LENGTH 48
#define SHA512_DIGEST_LENGTH 64
#define MD5_DIGEST_LENGTH 16
#define EVP_MAX_MD_SIZE 64

unsigned char* SHA1(const unsigned char* d, size_t n, unsigned char* md);
unsigned char* SHA256(const unsigned char* d, size_t n, unsigned char* md);
unsigned char* SHA512(const unsigned char* d, size_t n, unsigned char* md);
unsigned char* MD5(const unsigned char* d, size_t n, unsigned char* md);

typedef struct evp_md_st EVP_MD;
typedef struct engine_st ENGINE;
typedef struct evp_md_ctx_st EVP_MD_CTX;

const EVP_MD* EVP_sha1(void);
const EVP_MD* EVP_sha256(void);
const EVP_MD* EVP_sha512(void);
const EVP_MD* EVP_md5(void);

EVP_MD_CTX* EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX* ctx);
int EVP_DigestInit(EVP_MD_CTX* ctx, const EVP_MD* type);
int EVP_DigestInit_ex(EVP_MD_CTX* ctx, const EVP_MD* type, ENGINE* impl);
int EVP_DigestUpdate(EVP_MD_CTX* ctx, const void* d, size_t cnt);
int EVP_DigestFinal_ex(EVP_MD_CTX* ctx, unsigned char* md, unsigned int* s);
int EVP_Digest(const void* data, size_t count, unsigned char* md, unsigned int* size, const EVP_MD* type, ENGINE* impl);

unsigned char* HMAC(const EVP_MD* evp_md, const void* key, int key_len,
                    const unsigned char* d, size_t n, unsigned char* md, unsigned int* md_len);

// BIGNUM
typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;

BIGNUM* BN_new(void);
void BN_free(BIGNUM* a);
void BN_clear_free(BIGNUM* a);
BIGNUM* BN_bin2bn(const unsigned char* s, int len, BIGNUM* ret);
int BN_bn2bin(const BIGNUM* a, unsigned char* to);
int BN_bn2binpad(const BIGNUM* a, unsigned char* to, int tolen);
int BN_num_bytes(const BIGNUM* a);
int BN_num_bits(const BIGNUM* a);
BIGNUM* BN_copy(BIGNUM* to, const BIGNUM* from);

BN_CTX* BN_CTX_new(void);
void BN_CTX_free(BN_CTX* c);
void BN_CTX_start(BN_CTX* ctx);
BIGNUM* BN_CTX_get(BN_CTX* ctx);
void BN_CTX_end(BN_CTX* ctx);

// EC/ECDSA
#define NID_sect233r1 683
#define NID_X9_62_prime256v1 415

typedef struct ec_group_st EC_GROUP;
typedef struct ec_key_st EC_KEY;
typedef struct ec_point_st EC_POINT;
typedef struct ECDSA_SIG_st ECDSA_SIG;

EC_GROUP* EC_GROUP_new_by_curve_name(int nid);
void EC_GROUP_free(EC_GROUP* group);

EC_KEY* EC_KEY_new_by_curve_name(int nid);
void EC_KEY_free(EC_KEY* key);
int EC_KEY_generate_key(EC_KEY* key);
const BIGNUM* EC_KEY_get0_private_key(const EC_KEY* key);
int EC_KEY_set_private_key(EC_KEY* key, const BIGNUM* prv);
int EC_KEY_set_public_key_affine_coordinates(EC_KEY* key, BIGNUM* x, BIGNUM* y);

EC_POINT* EC_POINT_new(const EC_GROUP* group);
void EC_POINT_free(EC_POINT* point);
int EC_POINT_mul(const EC_GROUP* group, EC_POINT* r, const BIGNUM* n, const EC_POINT* q, const BIGNUM* m, BN_CTX* ctx);
int EC_POINT_get_affine_coordinates(const EC_GROUP* group, const EC_POINT* p, BIGNUM* x, BIGNUM* y, BN_CTX* ctx);
int EC_POINT_set_affine_coordinates(const EC_GROUP* group, EC_POINT* p, const BIGNUM* x, const BIGNUM* y, BN_CTX* ctx);
#define EC_POINT_get_affine_coordinates_GF2m EC_POINT_get_affine_coordinates
#define EC_POINT_set_affine_coordinates_GF2m EC_POINT_set_affine_coordinates
#define EC_POINT_get_affine_coordinates_GFp  EC_POINT_get_affine_coordinates
#define EC_POINT_set_affine_coordinates_GFp  EC_POINT_set_affine_coordinates

ECDSA_SIG* ECDSA_SIG_new(void);
void ECDSA_SIG_free(ECDSA_SIG* sig);
void ECDSA_SIG_get0(const ECDSA_SIG* sig, const BIGNUM** pr, const BIGNUM** ps);
int ECDSA_SIG_set0(ECDSA_SIG* sig, BIGNUM* r, BIGNUM* s);
ECDSA_SIG* ECDSA_do_sign(const unsigned char* dgst, int dgst_len, EC_KEY* eckey);
ECDSA_SIG* ECDSA_do_sign_ex(const unsigned char* dgst, int dgstlen, const BIGNUM* kinv, const BIGNUM* rp, EC_KEY* eckey);
int ECDSA_do_verify(const unsigned char* dgst, int dgst_len, const ECDSA_SIG* sig, EC_KEY* eckey);

// RSA/PKEY/X509
#define EVP_PKEY_RSA 6

typedef struct rsa_st RSA;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct x509_st X509;
typedef struct x509_store_st X509_STORE;
typedef struct x509_store_ctx_st X509_STORE_CTX;

EVP_PKEY* EVP_PKEY_new(void);
void EVP_PKEY_free(EVP_PKEY* pkey);
int EVP_PKEY_assign_RSA(EVP_PKEY* pkey, RSA* key);

RSA* RSA_new(void);
void RSA_free(RSA* rsa);
RSA* d2i_RSAPrivateKey(RSA** a, const unsigned char** pp, long length);
int i2d_RSAPrivateKey(const RSA* a, unsigned char** pp);
int i2d_PrivateKey(EVP_PKEY* a, unsigned char** pp);

X509* d2i_X509(X509** px, const unsigned char** in, long len);
void X509_free(X509* a);
X509_STORE* X509_STORE_new(void);
void X509_STORE_free(X509_STORE* v);
int X509_STORE_add_cert(X509_STORE* ctx, X509* x);
int X509_STORE_CTX_get_error(X509_STORE_CTX* ctx);
void X509_STORE_CTX_set_error(X509_STORE_CTX* ctx, int s);
#define X509_V_OK 0

#ifdef __cplusplus
}
#endif
