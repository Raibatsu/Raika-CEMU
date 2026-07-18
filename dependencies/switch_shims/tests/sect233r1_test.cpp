#include "../src/sect233r1.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace switch_crypto::sect233r1;

namespace
{
	template<typename T, void (*Free)(T*)>
	using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

	bool HmacCallback(const uint8_t* key, size_t keyLength, const uint8_t* data,
					  size_t dataLength, uint8_t* output)
	{
		unsigned int length = 0;
		return HMAC(EVP_sha256(), key, static_cast<int>(keyLength), data, dataLength, output, &length) && length == 32;
	}

	bool ExportOpenSslPoint(const EC_GROUP* group, const EC_POINT* point, Point& output)
	{
		OpenSslPtr<BIGNUM, BN_free> x(BN_new(), BN_free);
		OpenSslPtr<BIGNUM, BN_free> y(BN_new(), BN_free);
		if (!x || !y || EC_POINT_get_affine_coordinates(group, point, x.get(), y.get(), nullptr) != 1)
			return false;
		output = {};
		output.infinity = false;
		const auto elementSize = static_cast<int>(output.x.size());
		return BN_bn2binpad(x.get(), output.x.data(), elementSize) == elementSize &&
			   BN_bn2binpad(y.get(), output.y.data(), elementSize) == elementSize;
	}

	bool ImportOpenSslPoint(const EC_GROUP* group, const Point& input, EC_POINT* point)
	{
		OpenSslPtr<BIGNUM, BN_free> x(BN_bin2bn(input.x.data(), input.x.size(), nullptr), BN_free);
		OpenSslPtr<BIGNUM, BN_free> y(BN_bin2bn(input.y.data(), input.y.size(), nullptr), BN_free);
		return x && y && EC_POINT_set_affine_coordinates(group, point, x.get(), y.get(), nullptr) == 1;
	}

	bool Check(bool condition, const char* message)
	{
		if (!condition)
			std::fprintf(stderr, "%s\n", message);
		return condition;
	}

	Element ParseElement(const char* text)
	{
		Element result{};
		const size_t length = std::strlen(text);
		const size_t offset = result.size() * 2 - length;
		for (size_t i = 0; i < length; ++i)
		{
			const uint8_t digit = text[i] >= '0' && text[i] <= '9'
									  ? static_cast<uint8_t>(text[i] - '0')
									  : static_cast<uint8_t>((text[i] | 0x20) - 'a' + 10);
			const size_t nibble = offset + i;
			result[nibble / 2] |= static_cast<uint8_t>(digit << ((nibble & 1) ? 0 : 4));
		}
		return result;
	}
} // namespace

int main()
{
	OpenSslPtr<EC_GROUP, EC_GROUP_free> group(EC_GROUP_new_by_curve_name(NID_sect233r1), EC_GROUP_free);
	OpenSslPtr<EC_POINT, EC_POINT_free> referencePoint(EC_POINT_new(group.get()), EC_POINT_free);
	Element privateKey{};
	privateKey.back() = 1;
	Point publicKey{};
	if (!Check(group && referencePoint, "OpenSSL does not provide sect233r1") ||
		!Check(PublicKeyFromPrivate(privateKey, publicKey), "private-to-public conversion failed") ||
		!Check(IsValidPublicKey(publicKey), "generated public key failed validation"))
		return 1;

	OpenSslPtr<BIGNUM, BN_free> privateBn(BN_bin2bn(privateKey.data(), privateKey.size(), nullptr), BN_free);
	if (!Check(privateBn && EC_POINT_mul(group.get(), referencePoint.get(), privateBn.get(), nullptr, nullptr, nullptr) == 1,
			   "OpenSSL point multiplication failed"))
		return 1;
	Point referencePublic{};
	if (!Check(ExportOpenSslPoint(group.get(), referencePoint.get(), referencePublic) &&
				   publicKey.x == referencePublic.x && publicKey.y == referencePublic.y,
			   "generator mismatch"))
		return 1;

	const Element rfcPrivate = ParseElement("07ADC13DD5BF34D1DDEEB50B2CE23B5F5E6D18067306D60C5F6FF11E5D3");
	const Element rfcPublicX = ParseElement("0FB348B3246B473AA7FBB2A01B78D61B62C4221D0F9AB55FC72DB3DF478");
	const Element rfcPublicY = ParseElement("1162FA1F6C6ACF7FD8D19FC7D74BDD9104076E833898BC4C042A6E6BEBF");
	const Element rfcR = ParseElement("0A797F3B8AEFCE7456202DF1E46CCC291EA5A49DA3D4BDDA9A4B62D5E0D");
	const Element rfcS = ParseElement("01F6F81DA55C22DA4152134C661588F4BD6F82FDBAF0C5877096B070DC2");
	std::array<uint8_t, 32> rfcDigest{};
	unsigned int rfcDigestLength = 0;
	Element rfcActualR{};
	Element rfcActualS{};
	if (!Check(PublicKeyFromPrivate(rfcPrivate, publicKey) && publicKey.x == rfcPublicX && publicKey.y == rfcPublicY,
			   "RFC 6979 public key mismatch") ||
		!Check(EVP_Digest("sample", 6, rfcDigest.data(), &rfcDigestLength, EVP_sha256(), nullptr) == 1 &&
				   rfcDigestLength == rfcDigest.size() &&
				   Sign(rfcPrivate, rfcDigest.data(), rfcDigest.size(), HmacCallback, rfcActualR, rfcActualS) &&
				   rfcActualR == rfcR && rfcActualS == rfcS,
			   "RFC 6979 signature mismatch"))
		return 1;

	privateKey.back() = 2;
	if (!Check(PublicKeyFromPrivate(privateKey, publicKey), "2G calculation failed"))
		return 1;
	BN_set_word(privateBn.get(), 2);
	EC_POINT_mul(group.get(), referencePoint.get(), privateBn.get(), nullptr, nullptr, nullptr);
	ExportOpenSslPoint(group.get(), referencePoint.get(), referencePublic);
	if (!Check(publicKey.x == referencePublic.x && publicKey.y == referencePublic.y, "2G disagrees with OpenSSL"))
		return 1;

	const std::array<uint8_t, 32> digest{
		0x9f, 0x86, 0xd0, 0x81, 0x88, 0x4c, 0x7d, 0x65, 0x9a, 0x2f, 0xea, 0xa0, 0xc5, 0x5a, 0xd0, 0x15,
		0xa3, 0xbf, 0x4f, 0x1b, 0x2b, 0x0b, 0x82, 0x2c, 0xd1, 0x5d, 0x6c, 0x15, 0xb0, 0xf0, 0x0a, 0x08};
	Element r{};
	Element s{};
	if (!Check(Sign(privateKey, digest.data(), digest.size(), HmacCallback, r, s), "deterministic signature failed") ||
		!Check(Verify(publicKey, digest.data(), digest.size(), r, s), "self-verification failed"))
		return 1;

	OpenSslPtr<EC_KEY, EC_KEY_free> key(EC_KEY_new_by_curve_name(NID_sect233r1), EC_KEY_free);
	OpenSslPtr<EC_POINT, EC_POINT_free> importedPoint(EC_POINT_new(group.get()), EC_POINT_free);
	OpenSslPtr<BIGNUM, BN_free> rBn(BN_bin2bn(r.data(), r.size(), nullptr), BN_free);
	OpenSslPtr<BIGNUM, BN_free> sBn(BN_bin2bn(s.data(), s.size(), nullptr), BN_free);
	OpenSslPtr<ECDSA_SIG, ECDSA_SIG_free> signature(ECDSA_SIG_new(), ECDSA_SIG_free);
	if (!Check(key && importedPoint && rBn && sBn && signature &&
				   ImportOpenSslPoint(group.get(), publicKey, importedPoint.get()) &&
				   EC_KEY_set_private_key(key.get(), privateBn.get()) == 1 &&
				   EC_KEY_set_public_key(key.get(), importedPoint.get()) == 1 &&
				   ECDSA_SIG_set0(signature.get(), rBn.release(), sBn.release()) == 1 &&
				   ECDSA_do_verify(digest.data(), digest.size(), signature.get(), key.get()) == 1,
			   "OpenSSL rejected the generated signature"))
		return 1;

	OpenSslPtr<ECDSA_SIG, ECDSA_SIG_free> opensslSignature(
		ECDSA_do_sign(digest.data(), digest.size(), key.get()), ECDSA_SIG_free);
	const BIGNUM* opensslR = nullptr;
	const BIGNUM* opensslS = nullptr;
	ECDSA_SIG_get0(opensslSignature.get(), &opensslR, &opensslS);
	Element importedR{};
	Element importedS{};
	if (!Check(opensslSignature && BN_bn2binpad(opensslR, importedR.data(), importedR.size()) == importedR.size() &&
				   BN_bn2binpad(opensslS, importedS.data(), importedS.size()) == importedS.size() &&
				   Verify(publicKey, digest.data(), digest.size(), importedR, importedS),
			   "OpenSSL signature was rejected"))
		return 1;

	Element otherPrivate{};
	otherPrivate.back() = 3;
	Point otherPublic{};
	Element sharedA{};
	Element sharedB{};
	if (!Check(PublicKeyFromPrivate(otherPrivate, otherPublic) &&
				   SharedSecret(privateKey, otherPublic, sharedA) && SharedSecret(otherPrivate, publicKey, sharedB) &&
				   sharedA == sharedB,
			   "ECDH agreement failed"))
		return 1;

	for (unsigned iteration = 0; iteration < 16; ++iteration)
	{
		Element randomPrivate{};
		do
		{
			if (!Check(RAND_bytes(randomPrivate.data(), randomPrivate.size()) == 1, "random generation failed"))
				return 1;
			randomPrivate[0] &= 1;
		}
		while (!IsValidPrivateKey(randomPrivate));

		Point randomPublic{};
		OpenSslPtr<BIGNUM, BN_free> randomPrivateBn(
			BN_bin2bn(randomPrivate.data(), randomPrivate.size(), nullptr), BN_free);
		if (!Check(PublicKeyFromPrivate(randomPrivate, randomPublic) && randomPrivateBn &&
					   EC_POINT_mul(group.get(), referencePoint.get(), randomPrivateBn.get(), nullptr, nullptr, nullptr) == 1 &&
					   ExportOpenSslPoint(group.get(), referencePoint.get(), referencePublic) &&
					   randomPublic.x == referencePublic.x && randomPublic.y == referencePublic.y,
				   "random point multiplication disagrees with OpenSSL"))
			return 1;

		std::array<uint8_t, 32> randomDigest{};
		RAND_bytes(randomDigest.data(), randomDigest.size());
		if (!Check(Sign(randomPrivate, randomDigest.data(), randomDigest.size(), HmacCallback, r, s) &&
					   Verify(randomPublic, randomDigest.data(), randomDigest.size(), r, s),
				   "random signature failed"))
			return 1;

		OpenSslPtr<EC_POINT, EC_POINT_free> randomPoint(EC_POINT_new(group.get()), EC_POINT_free);
		OpenSslPtr<EC_KEY, EC_KEY_free> randomKey(EC_KEY_new_by_curve_name(NID_sect233r1), EC_KEY_free);
		OpenSslPtr<BIGNUM, BN_free> randomR(BN_bin2bn(r.data(), r.size(), nullptr), BN_free);
		OpenSslPtr<BIGNUM, BN_free> randomS(BN_bin2bn(s.data(), s.size(), nullptr), BN_free);
		OpenSslPtr<ECDSA_SIG, ECDSA_SIG_free> randomSignature(ECDSA_SIG_new(), ECDSA_SIG_free);
		if (!Check(randomPoint && randomKey && randomR && randomS && randomSignature &&
					   ImportOpenSslPoint(group.get(), randomPublic, randomPoint.get()) &&
					   EC_KEY_set_private_key(randomKey.get(), randomPrivateBn.get()) == 1 &&
					   EC_KEY_set_public_key(randomKey.get(), randomPoint.get()) == 1 &&
					   ECDSA_SIG_set0(randomSignature.get(), randomR.release(), randomS.release()) == 1 &&
					   ECDSA_do_verify(randomDigest.data(), randomDigest.size(), randomSignature.get(), randomKey.get()) == 1,
				   "OpenSSL rejected a random signature"))
			return 1;

		OpenSslPtr<ECDSA_SIG, ECDSA_SIG_free> generatedByOpenSsl(
			ECDSA_do_sign(randomDigest.data(), randomDigest.size(), randomKey.get()), ECDSA_SIG_free);
		if (!Check(generatedByOpenSsl != nullptr, "OpenSSL random signing failed"))
			return 1;
		ECDSA_SIG_get0(generatedByOpenSsl.get(), &opensslR, &opensslS);
		if (!Check(BN_bn2binpad(opensslR, importedR.data(), importedR.size()) == importedR.size() &&
					   BN_bn2binpad(opensslS, importedS.data(), importedS.size()) == importedS.size() &&
					   Verify(randomPublic, randomDigest.data(), randomDigest.size(), importedR, importedS),
				   "random OpenSSL signature was rejected"))
			return 1;
	}

	return 0;
}
