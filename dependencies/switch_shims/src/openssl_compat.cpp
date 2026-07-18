// OpenSSL-compatible shim for the Nintendo Switch, backed by mbedTLS.

#include <openssl/_ossl_compat.h>
#include <openssl/ssl.h>
#include "sect233r1.h"

#include <mbedtls/md.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/md5.h>
#include <mbedtls/bignum.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>
#include <switch/kernel/random.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

// Message digests

struct evp_md_st
{
	mbedtls_md_type_t type;
	int size;
};

static const EVP_MD s_md_sha1{MBEDTLS_MD_SHA1, 20};
static const EVP_MD s_md_sha256{MBEDTLS_MD_SHA256, 32};
static const EVP_MD s_md_sha512{MBEDTLS_MD_SHA512, 64};
static const EVP_MD s_md_md5{MBEDTLS_MD_MD5, 16};

const EVP_MD* EVP_sha1(void)
{
	return &s_md_sha1;
}
const EVP_MD* EVP_sha256(void)
{
	return &s_md_sha256;
}
const EVP_MD* EVP_sha512(void)
{
	return &s_md_sha512;
}
const EVP_MD* EVP_md5(void)
{
	return &s_md_md5;
}

unsigned char* SHA1(const unsigned char* d, size_t n, unsigned char* md)
{
	if ((!d && n != 0) || !md)
		return nullptr;
	return mbedtls_sha1_ret(d, n, md) == 0 ? md : nullptr;
}
unsigned char* SHA256(const unsigned char* d, size_t n, unsigned char* md)
{
	if ((!d && n != 0) || !md)
		return nullptr;
	return mbedtls_sha256_ret(d, n, md, 0) == 0 ? md : nullptr;
}
unsigned char* SHA512(const unsigned char* d, size_t n, unsigned char* md)
{
	if ((!d && n != 0) || !md)
		return nullptr;
	return mbedtls_sha512_ret(d, n, md, 0) == 0 ? md : nullptr;
}
unsigned char* MD5(const unsigned char* d, size_t n, unsigned char* md)
{
	if ((!d && n != 0) || !md)
		return nullptr;
	return mbedtls_md5_ret(d, n, md) == 0 ? md : nullptr;
}

struct evp_md_ctx_st
{
	mbedtls_md_context_t ctx;
	const EVP_MD* md;
	bool active;
};

EVP_MD_CTX* EVP_MD_CTX_new(void)
{
	auto* c = new (std::nothrow) evp_md_ctx_st();
	if (!c)
		return nullptr;
	mbedtls_md_init(&c->ctx);
	c->md = nullptr;
	c->active = false;
	return c;
}
void EVP_MD_CTX_free(EVP_MD_CTX* ctx)
{
	if (!ctx)
		return;
	mbedtls_md_free(&ctx->ctx);
	delete ctx;
}
int EVP_DigestInit_ex(EVP_MD_CTX* ctx, const EVP_MD* type, ENGINE*)
{
	if (!ctx || !type)
		return 0;
	mbedtls_md_free(&ctx->ctx);
	mbedtls_md_init(&ctx->ctx);
	ctx->md = nullptr;
	ctx->active = false;

	const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type->type);
	if (!info || mbedtls_md_setup(&ctx->ctx, info, 0) != 0)
		return 0;
	if (mbedtls_md_starts(&ctx->ctx) != 0)
	{
		mbedtls_md_free(&ctx->ctx);
		mbedtls_md_init(&ctx->ctx);
		return 0;
	}
	ctx->md = type;
	ctx->active = true;
	return 1;
}
int EVP_DigestInit(EVP_MD_CTX* ctx, const EVP_MD* type)
{
	return EVP_DigestInit_ex(ctx, type, nullptr);
}
int EVP_DigestUpdate(EVP_MD_CTX* ctx, const void* d, size_t cnt)
{
	if (!ctx || !ctx->active || (!d && cnt != 0))
		return 0;
	if (cnt == 0)
		return 1;
	return mbedtls_md_update(&ctx->ctx, (const unsigned char*)d, cnt) == 0 ? 1 : 0;
}
int EVP_DigestFinal_ex(EVP_MD_CTX* ctx, unsigned char* md, unsigned int* s)
{
	if (!ctx || !ctx->active || !md)
		return 0;
	if (mbedtls_md_finish(&ctx->ctx, md) != 0)
		return 0;
	if (s)
		*s = ctx->md ? (unsigned int)ctx->md->size : 0;
	ctx->active = false;
	return 1;
}
int EVP_Digest(const void* data, size_t count, unsigned char* md, unsigned int* size, const EVP_MD* type, ENGINE*)
{
	if (!type || !md || (!data && count != 0))
		return 0;
	const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type->type);
	if (!info)
		return 0;
	if (mbedtls_md(info, (const unsigned char*)data, count, md) != 0)
		return 0;
	if (size)
		*size = (unsigned int)type->size;
	return 1;
}

unsigned char* HMAC(const EVP_MD* evp_md, const void* key, int key_len,
					const unsigned char* d, size_t n, unsigned char* md, unsigned int* md_len)
{
	if (!evp_md || key_len < 0 || (!key && key_len != 0) || (!d && n != 0) || !md)
		return nullptr;
	const mbedtls_md_info_t* info = mbedtls_md_info_from_type(evp_md->type);
	if (!info || mbedtls_md_hmac(info, (const unsigned char*)key, (size_t)key_len, d, n, md) != 0)
		return nullptr;
	if (md_len)
		*md_len = (unsigned int)evp_md->size;
	return md;
}

// BIGNUM

struct bignum_st
{
	mbedtls_mpi mpi;
};
struct bignum_ctx
{
	std::vector<BIGNUM*> pool;
	std::vector<size_t> marks;
	size_t used{};
};

BIGNUM* BN_new(void)
{
	auto* b = new (std::nothrow) bignum_st();
	if (!b)
		return nullptr;
	mbedtls_mpi_init(&b->mpi);
	return b;
}
void BN_free(BIGNUM* a)
{
	if (!a)
		return;
	mbedtls_mpi_free(&a->mpi);
	delete a;
}
void BN_clear_free(BIGNUM* a)
{
	BN_free(a);
}

BIGNUM* BN_bin2bn(const unsigned char* s, int len, BIGNUM* ret)
{
	if (len < 0 || (!s && len != 0))
		return nullptr;

	const bool allocated = !ret;
	BIGNUM* r = ret ? ret : BN_new();
	if (!r)
		return nullptr;

	const int result = len == 0 ? mbedtls_mpi_lset(&r->mpi, 0) : mbedtls_mpi_read_binary(&r->mpi, s, (size_t)len);
	if (result != 0)
	{
		if (allocated)
			BN_free(r);
		return nullptr;
	}
	return r;
}
int BN_num_bytes(const BIGNUM* a)
{
	if (!a)
		return 0;
	const size_t size = mbedtls_mpi_size(&a->mpi);
	return size <= (size_t)std::numeric_limits<int>::max() ? (int)size : 0;
}
int BN_num_bits(const BIGNUM* a)
{
	if (!a)
		return 0;
	const size_t bits = mbedtls_mpi_bitlen(&a->mpi);
	return bits <= (size_t)std::numeric_limits<int>::max() ? (int)bits : 0;
}
int BN_bn2bin(const BIGNUM* a, unsigned char* to)
{
	if (!a)
		return 0;
	size_t sz = mbedtls_mpi_size(&a->mpi);
	if ((sz != 0 && !to) || sz > (size_t)std::numeric_limits<int>::max())
		return 0;
	if (sz != 0 && mbedtls_mpi_write_binary(&a->mpi, to, sz) != 0)
		return 0;
	return (int)sz;
}
int BN_bn2binpad(const BIGNUM* a, unsigned char* to, int tolen)
{
	if (!a || tolen < 0 || (tolen != 0 && !to) || mbedtls_mpi_size(&a->mpi) > (size_t)tolen)
		return -1;
	if (tolen == 0)
		return 0;
	if (mbedtls_mpi_write_binary(&a->mpi, to, (size_t)tolen) != 0)
		return -1;
	return tolen;
}
BIGNUM* BN_copy(BIGNUM* to, const BIGNUM* from)
{
	if (!to || !from)
		return nullptr;
	return mbedtls_mpi_copy(&to->mpi, &from->mpi) == 0 ? to : nullptr;
}

BN_CTX* BN_CTX_new(void)
{
	return new (std::nothrow) bignum_ctx();
}
void BN_CTX_free(BN_CTX* c)
{
	if (!c)
		return;
	for (BIGNUM* b : c->pool)
		BN_free(b);
	delete c;
}
void BN_CTX_start(BN_CTX* ctx)
{
	if (ctx)
	{
		try
		{
			ctx->marks.push_back(ctx->used);
		} catch (const std::bad_alloc&)
		{
		}
	}
}
BIGNUM* BN_CTX_get(BN_CTX* ctx)
{
	if (!ctx)
		return nullptr;

	if (ctx->used == ctx->pool.size())
	{
		BIGNUM* b = BN_new();
		if (!b)
			return nullptr;
		try
		{
			ctx->pool.push_back(b);
		} catch (const std::bad_alloc&)
		{
			BN_free(b);
			return nullptr;
		}
	}

	BIGNUM* b = ctx->pool[ctx->used++];
	if (mbedtls_mpi_lset(&b->mpi, 0) != 0)
	{
		--ctx->used;
		return nullptr;
	}
	return b;
}
void BN_CTX_end(BN_CTX* ctx)
{
	if (!ctx || ctx->marks.empty())
		return;
	ctx->used = ctx->marks.back();
	ctx->marks.pop_back();
}

struct ec_group_st
{
	int nid;
};
struct ec_point_st
{
	BIGNUM* x;
	BIGNUM* y;
	bool infinity;
};
struct ec_key_st
{
	int nid;
	BIGNUM* priv;
	BIGNUM* pub_x;
	BIGNUM* pub_y;
	bool has_public;
};
struct ECDSA_SIG_st
{
	BIGNUM* r;
	BIGNUM* s;
};

namespace
{
	using SectElement = switch_crypto::sect233r1::Element;
	using SectPoint = switch_crypto::sect233r1::Point;

	bool bn_to_sect_element(const BIGNUM* value, SectElement& element)
	{
		element.fill(0);
		return value && mbedtls_mpi_cmp_int(&value->mpi, 0) >= 0 &&
			   mbedtls_mpi_size(&value->mpi) <= element.size() &&
			   mbedtls_mpi_write_binary(&value->mpi, element.data(), element.size()) == 0;
	}

	bool sect_element_to_bn(const SectElement& element, BIGNUM* value)
	{
		return value && mbedtls_mpi_read_binary(&value->mpi, element.data(), element.size()) == 0;
	}

	bool ec_point_to_sect(const EC_POINT* point, SectPoint& result)
	{
		if (!point)
			return false;
		result = {};
		if (point->infinity)
			return true;
		result.infinity = false;
		return bn_to_sect_element(point->x, result.x) && bn_to_sect_element(point->y, result.y);
	}

	bool ec_key_public_to_sect(const EC_KEY* key, SectPoint& result)
	{
		if (!key || !key->has_public)
			return false;
		result.infinity = false;
		return bn_to_sect_element(key->pub_x, result.x) && bn_to_sect_element(key->pub_y, result.y);
	}

	bool sect_to_ec_point(const SectPoint& point, EC_POINT* result)
	{
		if (!result)
			return false;
		result->infinity = point.infinity;
		return point.infinity || (sect_element_to_bn(point.x, result->x) && sect_element_to_bn(point.y, result->y));
	}

	bool switch_random_bytes(void* output, size_t length)
	{
		randomGet(output, length);
		return true;
	}

	bool switch_hmac_sha256(const uint8_t* key, size_t keyLength, const uint8_t* data,
							size_t dataLength, uint8_t* output)
	{
		const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
		return info && mbedtls_md_hmac(info, key, keyLength, data, dataLength, output) == 0;
	}
} // namespace

EC_GROUP* EC_GROUP_new_by_curve_name(int nid)
{
	if (nid != NID_sect233r1)
		return nullptr;
	auto* g = new (std::nothrow) ec_group_st();
	if (!g)
		return nullptr;
	g->nid = nid;
	return g;
}
void EC_GROUP_free(EC_GROUP* group)
{
	delete group;
}

EC_KEY* EC_KEY_new_by_curve_name(int nid)
{
	if (nid != NID_sect233r1)
		return nullptr;
	auto* k = new (std::nothrow) ec_key_st();
	if (!k)
		return nullptr;
	k->nid = nid;
	k->priv = BN_new();
	k->pub_x = BN_new();
	k->pub_y = BN_new();
	k->has_public = false;
	if (!k->priv || !k->pub_x || !k->pub_y)
	{
		EC_KEY_free(k);
		return nullptr;
	}
	return k;
}
void EC_KEY_free(EC_KEY* key)
{
	if (!key)
		return;
	BN_free(key->priv);
	BN_free(key->pub_x);
	BN_free(key->pub_y);
	delete key;
}
int EC_KEY_generate_key(EC_KEY* key)
{
	if (!key || key->nid != NID_sect233r1)
		return 0;
	SectElement privateKey{};
	SectPoint publicKey{};
	if (!switch_crypto::sect233r1::GenerateKey(switch_random_bytes, privateKey, publicKey) ||
		!sect_element_to_bn(privateKey, key->priv) ||
		!sect_element_to_bn(publicKey.x, key->pub_x) ||
		!sect_element_to_bn(publicKey.y, key->pub_y))
		return 0;
	key->has_public = true;
	return 1;
}
const BIGNUM* EC_KEY_get0_private_key(const EC_KEY* key)
{
	return key ? key->priv : nullptr;
}
int EC_KEY_set_private_key(EC_KEY* key, const BIGNUM* prv)
{
	SectElement privateKey{};
	if (!key || key->nid != NID_sect233r1 || !bn_to_sect_element(prv, privateKey) ||
		!switch_crypto::sect233r1::IsValidPrivateKey(privateKey))
		return 0;
	return BN_copy(key->priv, prv) ? 1 : 0;
}
int EC_KEY_set_public_key_affine_coordinates(EC_KEY* key, BIGNUM* x, BIGNUM* y)
{
	SectPoint publicKey{};
	publicKey.infinity = false;
	if (!key || key->nid != NID_sect233r1 || !bn_to_sect_element(x, publicKey.x) ||
		!bn_to_sect_element(y, publicKey.y) || !switch_crypto::sect233r1::IsValidPublicKey(publicKey))
		return 0;
	if (!BN_copy(key->pub_x, x) || !BN_copy(key->pub_y, y))
		return 0;
	key->has_public = true;
	return 1;
}

EC_POINT* EC_POINT_new(const EC_GROUP* group)
{
	if (!group || group->nid != NID_sect233r1)
		return nullptr;
	auto* p = new (std::nothrow) ec_point_st();
	if (!p)
		return nullptr;
	p->x = BN_new();
	p->y = BN_new();
	p->infinity = true;
	if (!p->x || !p->y)
	{
		EC_POINT_free(p);
		return nullptr;
	}
	return p;
}
void EC_POINT_free(EC_POINT* point)
{
	if (!point)
		return;
	BN_free(point->x);
	BN_free(point->y);
	delete point;
}
int EC_POINT_mul(const EC_GROUP* group, EC_POINT* r, const BIGNUM* n, const EC_POINT* q, const BIGNUM* m, BN_CTX*)
{
	if (!group || group->nid != NID_sect233r1 || !r || (!n && (!q || !m)))
		return 0;
	SectPoint result{};
	if (n)
	{
		SectElement scalar{};
		if (!bn_to_sect_element(n, scalar) || !switch_crypto::sect233r1::MultiplyGenerator(scalar, result))
			return 0;
	}
	if (q && m)
	{
		SectElement scalar{};
		SectPoint point{};
		SectPoint product{};
		if (!bn_to_sect_element(m, scalar) || !ec_point_to_sect(q, point) ||
			!switch_crypto::sect233r1::Multiply(point, scalar, product))
			return 0;
		if (!n)
			result = product;
		else if (!switch_crypto::sect233r1::Add(result, product, result))
			return 0;
	}
	return sect_to_ec_point(result, r) ? 1 : 0;
}
int EC_POINT_get_affine_coordinates(const EC_GROUP* group, const EC_POINT* p, BIGNUM* x, BIGNUM* y, BN_CTX*)
{
	if (!group || group->nid != NID_sect233r1 || !p || p->infinity || !x || !y)
		return 0;
	return BN_copy(x, p->x) && BN_copy(y, p->y) ? 1 : 0;
}
int EC_POINT_set_affine_coordinates(const EC_GROUP* group, EC_POINT* p, const BIGNUM* x, const BIGNUM* y, BN_CTX*)
{
	SectPoint point{};
	point.infinity = false;
	if (!group || group->nid != NID_sect233r1 || !p || !bn_to_sect_element(x, point.x) ||
		!bn_to_sect_element(y, point.y) || !switch_crypto::sect233r1::IsValidPublicKey(point))
		return 0;
	if (!BN_copy(p->x, x) || !BN_copy(p->y, y))
		return 0;
	p->infinity = false;
	return 1;
}

ECDSA_SIG* ECDSA_SIG_new(void)
{
	auto* s = new (std::nothrow) ECDSA_SIG_st();
	if (!s)
		return nullptr;
	s->r = BN_new();
	s->s = BN_new();
	if (!s->r || !s->s)
	{
		ECDSA_SIG_free(s);
		return nullptr;
	}
	return s;
}
void ECDSA_SIG_free(ECDSA_SIG* sig)
{
	if (!sig)
		return;
	BN_free(sig->r);
	BN_free(sig->s);
	delete sig;
}
void ECDSA_SIG_get0(const ECDSA_SIG* sig, const BIGNUM** pr, const BIGNUM** ps)
{
	if (pr)
		*pr = sig ? sig->r : nullptr;
	if (ps)
		*ps = sig ? sig->s : nullptr;
}
int ECDSA_SIG_set0(ECDSA_SIG* sig, BIGNUM* r, BIGNUM* s)
{
	if (!sig || !r || !s)
		return 0;
	if (sig->r != r)
		BN_free(sig->r);
	if (sig->s != s)
		BN_free(sig->s);
	sig->r = r;
	sig->s = s;
	return 1;
}
ECDSA_SIG* ECDSA_do_sign(const unsigned char* digest, int digest_length, EC_KEY* key)
{
	if (digest_length < 0 || !key || key->nid != NID_sect233r1)
		return nullptr;
	SectElement privateKey{};
	SectElement r{};
	SectElement s{};
	if (!bn_to_sect_element(key->priv, privateKey) ||
		!switch_crypto::sect233r1::Sign(privateKey, digest, static_cast<size_t>(digest_length), switch_hmac_sha256, r, s))
		return nullptr;
	ECDSA_SIG* signature = ECDSA_SIG_new();
	if (!signature || !sect_element_to_bn(r, signature->r) || !sect_element_to_bn(s, signature->s))
	{
		ECDSA_SIG_free(signature);
		return nullptr;
	}
	return signature;
}
ECDSA_SIG* ECDSA_do_sign_ex(const unsigned char* digest, int digest_length, const BIGNUM* inverse, const BIGNUM* r,
							EC_KEY* key)
{
	if (inverse || r)
		return nullptr;
	return ECDSA_do_sign(digest, digest_length, key);
}
int ECDSA_do_verify(const unsigned char* digest, int digest_length, const ECDSA_SIG* signature, EC_KEY* key)
{
	if (digest_length < 0 || !signature || !key || key->nid != NID_sect233r1)
		return 0;
	SectPoint publicKey{};
	SectElement r{};
	SectElement s{};
	if (!ec_key_public_to_sect(key, publicKey) || !bn_to_sect_element(signature->r, r) ||
		!bn_to_sect_element(signature->s, s))
		return 0;
	return switch_crypto::sect233r1::Verify(publicKey, digest, static_cast<size_t>(digest_length), r, s) ? 1 : 0;
}

// RSA/PKEY/X509

namespace
{
	bool get_der_object_length(const unsigned char* data, size_t available, size_t& object_length)
	{
		if (!data || available < 2 || data[0] != 0x30)
			return false;

		size_t header_length = 2;
		size_t content_length = data[1];
		if ((data[1] & 0x80) != 0)
		{
			const size_t length_octets = data[1] & 0x7f;
			if (length_octets == 0 || length_octets > sizeof(size_t) || available < 2 + length_octets || data[2] == 0)
				return false;

			header_length += length_octets;
			content_length = 0;
			for (size_t i = 0; i < length_octets; ++i)
			{
				if (content_length > (std::numeric_limits<size_t>::max() >> 8))
					return false;
				content_length = (content_length << 8) | data[2 + i];
			}
			if (content_length < 0x80)
				return false;
		}

		if (content_length > available - header_length)
			return false;
		object_length = header_length + content_length;
		return true;
	}
} // namespace

struct rsa_st
{
	mbedtls_pk_context key;
};
struct evp_pkey_st
{
	RSA* rsa{};
};
struct x509_st
{
	mbedtls_x509_crt certificate;
};
struct x509_store_st
{
	mbedtls_x509_crt owned_chain{};
	mbedtls_x509_crt* chain{};
	bool owns_chain{};
	std::mutex mutex;
};
struct x509_store_ctx_st
{
	int error{};
	uint32_t* flags{};
};

RSA* RSA_new(void)
{
	auto* rsa = new (std::nothrow) rsa_st();
	if (!rsa)
		return nullptr;
	mbedtls_pk_init(&rsa->key);
	return rsa;
}
void RSA_free(RSA* rsa)
{
	if (!rsa)
		return;
	mbedtls_pk_free(&rsa->key);
	delete rsa;
}

EVP_PKEY* EVP_PKEY_new(void)
{
	return new (std::nothrow) evp_pkey_st();
}
void EVP_PKEY_free(EVP_PKEY* pkey)
{
	if (!pkey)
		return;
	RSA_free(pkey->rsa);
	delete pkey;
}
int EVP_PKEY_assign_RSA(EVP_PKEY* pkey, RSA* key)
{
	if (!pkey || !key)
		return 0;
	if (pkey->rsa != key)
		RSA_free(pkey->rsa);
	pkey->rsa = key;
	return 1;
}
RSA* d2i_RSAPrivateKey(RSA** a, const unsigned char** pp, long length)
{
	if (!pp || !*pp || length <= 0)
		return nullptr;

	size_t object_length = 0;
	if (!get_der_object_length(*pp, (size_t)length, object_length))
		return nullptr;

	RSA* r = RSA_new();
	if (!r)
		return nullptr;
	if (mbedtls_pk_parse_key(&r->key, *pp, object_length, nullptr, 0) != 0 || !mbedtls_pk_can_do(&r->key, MBEDTLS_PK_RSA))
	{
		RSA_free(r);
		return nullptr;
	}

	if (a)
	{
		RSA* previous = *a;
		*a = r;
		if (previous && previous != r)
			RSA_free(previous);
	}
	*pp += object_length;
	return r;
}
int i2d_RSAPrivateKey(const RSA* rsa, unsigned char** pp)
{
	if (!rsa || !mbedtls_pk_can_do(&rsa->key, MBEDTLS_PK_RSA))
		return 0;

	const size_t key_size = mbedtls_pk_get_len(&rsa->key);
	if (key_size > (std::numeric_limits<size_t>::max() - 1024) / 8)
		return 0;
	const size_t buffer_size = std::max<size_t>(4096, key_size * 8 + 1024);
	std::vector<unsigned char> buffer;
	try
	{
		buffer.resize(buffer_size);
	} catch (const std::bad_alloc&)
	{
		return 0;
	}
	const int encoded_length = mbedtls_pk_write_key_der(
		const_cast<mbedtls_pk_context*>(&rsa->key), buffer.data(), buffer.size());
	if (encoded_length <= 0)
		return 0;

	if (pp)
	{
		const unsigned char* encoded = buffer.data() + buffer.size() - (size_t)encoded_length;
		if (*pp)
		{
			std::memcpy(*pp, encoded, (size_t)encoded_length);
			*pp += encoded_length;
		}
		else
		{
			auto* allocated = static_cast<unsigned char*>(std::malloc((size_t)encoded_length));
			if (!allocated)
				return 0;
			std::memcpy(allocated, encoded, (size_t)encoded_length);
			*pp = allocated;
		}
	}
	return encoded_length;
}

int i2d_PrivateKey(EVP_PKEY* pkey, unsigned char** pp)
{
	return pkey ? i2d_RSAPrivateKey(pkey->rsa, pp) : 0;
}

X509* d2i_X509(X509** px, const unsigned char** in, long len)
{
	if (!in || !*in || len <= 0)
		return nullptr;

	size_t object_length = 0;
	if (!get_der_object_length(*in, (size_t)len, object_length))
		return nullptr;

	auto* x = new (std::nothrow) x509_st();
	if (!x)
		return nullptr;
	mbedtls_x509_crt_init(&x->certificate);
	if (mbedtls_x509_crt_parse_der(&x->certificate, *in, object_length) != 0)
	{
		X509_free(x);
		return nullptr;
	}

	if (px)
	{
		X509* previous = *px;
		*px = x;
		if (previous && previous != x)
			X509_free(previous);
	}
	*in += object_length;
	return x;
}
void X509_free(X509* certificate)
{
	if (!certificate)
		return;
	mbedtls_x509_crt_free(&certificate->certificate);
	delete certificate;
}
X509_STORE* X509_STORE_new(void)
{
	auto* store = new (std::nothrow) x509_store_st();
	if (!store)
		return nullptr;
	mbedtls_x509_crt_init(&store->owned_chain);
	store->chain = &store->owned_chain;
	store->owns_chain = true;
	return store;
}
void X509_STORE_free(X509_STORE* store)
{
	if (!store || !store->owns_chain)
		return;
	mbedtls_x509_crt_free(&store->owned_chain);
	delete store;
}
int X509_STORE_add_cert(X509_STORE* ctx, X509* x)
{
	if (!ctx || !x || !x->certificate.raw.p || x->certificate.raw.len == 0)
		return 0;
	std::scoped_lock lock(ctx->mutex);
	if (!ctx->chain)
		return 0;
	return mbedtls_x509_crt_parse_der(ctx->chain, x->certificate.raw.p, x->certificate.raw.len) == 0 ? 1 : 0;
}
int X509_STORE_CTX_get_error(X509_STORE_CTX* ctx)
{
	return ctx ? ctx->error : X509_V_OK;
}
void X509_STORE_CTX_set_error(X509_STORE_CTX* ctx, int error)
{
	if (!ctx)
		return;
	ctx->error = error;
	if (!ctx->flags)
		return;
	if (error == X509_V_OK)
		*ctx->flags = 0;
	else if (*ctx->flags == 0)
		*ctx->flags = MBEDTLS_X509_BADCERT_OTHER;
}

// SSL/TLS

namespace
{
	struct VerifyCallbackSlot
	{
		std::atomic<SSL_verify_cb> callback{};
	};

	std::array<VerifyCallbackSlot, 8> s_verify_callbacks;

	VerifyCallbackSlot* get_verify_callback_slot(SSL_verify_cb callback)
	{
		for (auto& slot : s_verify_callbacks)
		{
			if (slot.callback.load(std::memory_order_acquire) == callback)
				return &slot;
		}
		for (auto& slot : s_verify_callbacks)
		{
			SSL_verify_cb expected = nullptr;
			if (slot.callback.compare_exchange_strong(expected, callback, std::memory_order_release, std::memory_order_acquire) ||
				expected == callback)
				return &slot;
		}
		return nullptr;
	}

	int verify_callback(void* context, mbedtls_x509_crt*, int, uint32_t* flags)
	{
		if (!context || !flags)
			return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
		auto* slot = static_cast<VerifyCallbackSlot*>(context);
		SSL_verify_cb callback = slot->callback.load(std::memory_order_acquire);
		if (!callback)
			return 0;

		x509_store_ctx_st store_context{static_cast<int>(*flags), flags};
		if (callback(*flags == 0 ? 1 : 0, &store_context))
			*flags = 0;
		else if (*flags == 0)
			*flags = MBEDTLS_X509_BADCERT_OTHER;
		return 0;
	}

	struct ClientIdentityState
	{
		mbedtls_ssl_config* config{};
		X509* certificate{};
		RSA* private_key{};
		bool configured{};
	};

	thread_local ClientIdentityState s_client_identity;

	struct ContextCaStore
	{
		ContextCaStore()
		{
			mbedtls_x509_crt_init(&store.owned_chain);
			store.chain = &store.owned_chain;
		}
		~ContextCaStore()
		{
			mbedtls_x509_crt_free(&store.owned_chain);
		}

		void reset()
		{
			std::scoped_lock lock(store.mutex);
			mbedtls_x509_crt_free(&store.owned_chain);
			mbedtls_x509_crt_init(&store.owned_chain);
			store.chain = &store.owned_chain;
		}

		void set_chain(mbedtls_x509_crt* chain)
		{
			std::scoped_lock lock(store.mutex);
			store.chain = chain;
		}

		x509_store_st store;
	};

	std::mutex s_ca_stores_mutex;
	std::unordered_map<mbedtls_ssl_config*, std::unique_ptr<ContextCaStore>> s_ca_stores;

	ClientIdentityState& get_client_identity(mbedtls_ssl_config* config)
	{
		if (s_client_identity.config != config)
			s_client_identity = {config, nullptr, nullptr, false};
		return s_client_identity;
	}

	int configure_client_identity(ClientIdentityState& identity)
	{
		if (!identity.config || !identity.certificate || !identity.private_key)
			return 0;
		if (identity.configured)
			return 1;
		if (!mbedtls_pk_can_do(&identity.private_key->key, MBEDTLS_PK_RSA) ||
			mbedtls_pk_check_pair(&identity.certificate->certificate.pk, &identity.private_key->key) != 0)
			return 0;
		if (mbedtls_ssl_conf_own_cert(identity.config, &identity.certificate->certificate, &identity.private_key->key) != 0)
			return 0;
		identity.configured = true;
		return 1;
	}
} // namespace

long SSL_CTX_set_mode(SSL_CTX* ctx, long mode)
{
	return ctx ? mode : 0;
}
void SSL_CTX_set_verify(SSL_CTX* ctx, int mode, SSL_verify_cb callback)
{
	if (!ctx)
		return;
	auto* config = reinterpret_cast<mbedtls_ssl_config*>(ctx);
	if ((mode & SSL_VERIFY_PEER) != 0)
	{
		mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
		if (callback)
		{
			if (auto* slot = get_verify_callback_slot(callback))
				mbedtls_ssl_conf_verify(config, verify_callback, slot);
			else
				mbedtls_ssl_conf_verify(config, nullptr, nullptr);
		}
		else
		{
			mbedtls_ssl_conf_verify(config, nullptr, nullptr);
		}
	}
	else
	{
		mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_verify(config, nullptr, nullptr);
	}
}
void SSL_CTX_set_verify_depth(SSL_CTX*, int) {}
int SSL_CTX_set_cipher_list(SSL_CTX* ctx, const char* cipher_list)
{
	if (!ctx || !cipher_list)
		return 0;
	static const int s_aes256_sha[] = {MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA, 0};
	if (std::strcmp(cipher_list, "AES256-SHA") != 0 ||
		!mbedtls_ssl_ciphersuite_from_id(MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA))
		return 0;
	mbedtls_ssl_conf_ciphersuites(reinterpret_cast<mbedtls_ssl_config*>(ctx), s_aes256_sha);
	return 1;
}
X509_STORE* SSL_CTX_get_cert_store(const SSL_CTX* ctx)
{
	if (!ctx)
		return nullptr;
	auto* config = reinterpret_cast<mbedtls_ssl_config*>(const_cast<SSL_CTX*>(ctx));
	try
	{
		std::scoped_lock lock(s_ca_stores_mutex);
		auto& state = s_ca_stores[config];
		if (!state)
			state = std::make_unique<ContextCaStore>();
		if (!config->ca_chain)
		{
			state->reset();
			mbedtls_ssl_conf_ca_chain(config, &state->store.owned_chain, nullptr);
		}
		state->set_chain(config->ca_chain);
		return &state->store;
	} catch (...)
	{
		return nullptr;
	}
}
int SSL_CTX_use_certificate(SSL_CTX* ctx, X509* certificate)
{
	if (!ctx || !certificate || !certificate->certificate.raw.p)
		return 0;
	auto& identity = get_client_identity(reinterpret_cast<mbedtls_ssl_config*>(ctx));
	identity.certificate = certificate;
	identity.configured = false;
	return identity.private_key ? configure_client_identity(identity) : 1;
}
int SSL_CTX_use_RSAPrivateKey(SSL_CTX* ctx, RSA* private_key)
{
	if (!ctx || !private_key || !mbedtls_pk_can_do(&private_key->key, MBEDTLS_PK_RSA))
		return 0;
	auto& identity = get_client_identity(reinterpret_cast<mbedtls_ssl_config*>(ctx));
	identity.private_key = private_key;
	identity.configured = false;
	return identity.certificate ? configure_client_identity(identity) : 1;
}
int SSL_CTX_check_private_key(const SSL_CTX* ctx)
{
	if (!ctx)
		return 0;
	auto& identity = get_client_identity(reinterpret_cast<mbedtls_ssl_config*>(const_cast<SSL_CTX*>(ctx)));
	return configure_client_identity(identity);
}
int ECDH_compute_key(void* out, size_t outlen, const EC_POINT* public_key, EC_KEY* private_key, void*)
{
	if (!out || outlen < switch_crypto::sect233r1::ElementSize || !public_key || !private_key ||
		private_key->nid != NID_sect233r1)
		return -1;
	SectElement privateKey{};
	SectPoint publicKey{};
	SectElement secret{};
	if (!bn_to_sect_element(private_key->priv, privateKey) || !ec_point_to_sect(public_key, publicKey) ||
		!switch_crypto::sect233r1::SharedSecret(privateKey, publicKey, secret))
		return -1;
	std::memcpy(out, secret.data(), secret.size());
	return static_cast<int>(secret.size());
}
