#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace switch_crypto::sect233r1
{
	constexpr size_t ElementSize = 30;
	using Element = std::array<uint8_t, ElementSize>;
	using RandomBytes = bool (*)(void*, size_t);
	using HmacSha256 = bool (*)(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*);

	struct Point
	{
		Element x{};
		Element y{};
		bool infinity{true};
	};

	bool IsValidPrivateKey(const Element& privateKey);
	bool IsValidPublicKey(const Point& publicKey);
	bool GenerateKey(RandomBytes randomBytes, Element& privateKey, Point& publicKey);
	bool PublicKeyFromPrivate(const Element& privateKey, Point& publicKey);
	bool MultiplyGenerator(const Element& scalar, Point& result);
	bool Multiply(const Point& point, const Element& scalar, Point& result);
	bool Add(const Point& lhs, const Point& rhs, Point& result);
	bool Sign(const Element& privateKey, const uint8_t* digest, size_t digestLength,
			  HmacSha256 hmacSha256, Element& r, Element& s);
	bool Verify(const Point& publicKey, const uint8_t* digest, size_t digestLength,
				const Element& r, const Element& s);
	bool SharedSecret(const Element& privateKey, const Point& publicKey, Element& secret);
} // namespace switch_crypto::sect233r1
