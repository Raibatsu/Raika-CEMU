#include "sect233r1.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace switch_crypto::sect233r1
{
	namespace
	{
		using Limbs = std::array<uint64_t, 4>;
		using WideLimbs = std::array<uint64_t, 8>;

		struct Field
		{
			Limbs words{};
		};

		struct Scalar
		{
			Limbs words{};
		};

		struct InternalPoint
		{
			Field x{};
			Field y{};
			bool infinity{true};
		};

		constexpr Element CurveB{
			0x00, 0x66, 0x64, 0x7e, 0xde, 0x6c, 0x33, 0x2c, 0x7f, 0x8c,
			0x09, 0x23, 0xbb, 0x58, 0x21, 0x3b, 0x33, 0x3b, 0x20, 0xe9,
			0xce, 0x42, 0x81, 0xfe, 0x11, 0x5f, 0x7d, 0x8f, 0x90, 0xad};
		constexpr Element GeneratorX{
			0x00, 0xfa, 0xc9, 0xdf, 0xcb, 0xac, 0x83, 0x13, 0xbb, 0x21,
			0x39, 0xf1, 0xbb, 0x75, 0x5f, 0xef, 0x65, 0xbc, 0x39, 0x1f,
			0x8b, 0x36, 0xf8, 0xf8, 0xeb, 0x73, 0x71, 0xfd, 0x55, 0x8b};
		constexpr Element GeneratorY{
			0x01, 0x00, 0x6a, 0x08, 0xa4, 0x19, 0x03, 0x35, 0x06, 0x78,
			0xe5, 0x85, 0x28, 0xbe, 0xbf, 0x8a, 0x0b, 0xef, 0xf8, 0x67,
			0xa7, 0xca, 0x36, 0x71, 0x6f, 0x7e, 0x01, 0xf8, 0x10, 0x52};
		constexpr Element GroupOrder{
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0xe9, 0x74, 0xe7, 0x2f,
			0x8a, 0x69, 0x22, 0x03, 0x1d, 0x26, 0x03, 0xcf, 0xe0, 0xd7};

		template<typename T>
		bool IsZero(const T& value)
		{
			return std::all_of(value.words.begin(), value.words.end(), [](uint64_t word) { return word == 0; });
		}

		template<typename T>
		bool Equal(const T& lhs, const T& rhs)
		{
			return lhs.words == rhs.words;
		}

		template<typename T>
		T FromBytes(const Element& bytes)
		{
			T result{};
			for (size_t i = 0; i < bytes.size(); ++i)
			{
				const size_t offset = bytes.size() - 1 - i;
				result.words[offset / 8] |= static_cast<uint64_t>(bytes[i]) << ((offset % 8) * 8);
			}
			return result;
		}

		template<typename T>
		Element ToBytes(const T& value)
		{
			Element result{};
			for (size_t i = 0; i < result.size(); ++i)
			{
				const size_t offset = result.size() - 1 - i;
				result[i] = static_cast<uint8_t>(value.words[offset / 8] >> ((offset % 8) * 8));
			}
			return result;
		}

		bool GetBit(const Limbs& words, unsigned bit)
		{
			return bit < 256 && ((words[bit / 64] >> (bit % 64)) & 1) != 0;
		}

		bool GetBit(const WideLimbs& words, unsigned bit)
		{
			return bit < 512 && ((words[bit / 64] >> (bit % 64)) & 1) != 0;
		}

		void ToggleBit(WideLimbs& words, unsigned bit)
		{
			words[bit / 64] ^= uint64_t{1} << (bit % 64);
		}

		int Degree(const WideLimbs& words)
		{
			for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i)
			{
				if (words[i] != 0)
					return i * 64 + 63 - __builtin_clzll(words[i]);
			}
			return -1;
		}

		void XorShifted(WideLimbs& destination, const WideLimbs& source, unsigned shift)
		{
			const unsigned wordShift = shift / 64;
			const unsigned bitShift = shift % 64;
			for (unsigned i = 0; i < source.size() && i + wordShift < destination.size(); ++i)
			{
				destination[i + wordShift] ^= source[i] << bitShift;
				if (bitShift != 0 && i + wordShift + 1 < destination.size())
					destination[i + wordShift + 1] ^= source[i] >> (64 - bitShift);
			}
		}

		void Reduce(WideLimbs& value)
		{
			for (int bit = 511; bit >= 233; --bit)
			{
				if (!GetBit(value, bit))
					continue;
				ToggleBit(value, bit);
				ToggleBit(value, static_cast<unsigned>(bit - 233));
				ToggleBit(value, static_cast<unsigned>(bit - 159));
			}
		}

		Field AddField(const Field& lhs, const Field& rhs)
		{
			Field result{};
			for (size_t i = 0; i < result.words.size(); ++i)
				result.words[i] = lhs.words[i] ^ rhs.words[i];
			return result;
		}

		Field MultiplyField(const Field& lhs, const Field& rhs)
		{
			WideLimbs product{};
			for (unsigned limb = 0; limb < lhs.words.size(); ++limb)
			{
				uint64_t bits = lhs.words[limb];
				while (bits != 0)
				{
					const unsigned bit = __builtin_ctzll(bits);
					WideLimbs wideRhs{};
					std::copy(rhs.words.begin(), rhs.words.end(), wideRhs.begin());
					XorShifted(product, wideRhs, limb * 64 + bit);
					bits &= bits - 1;
				}
			}
			Reduce(product);
			Field result{};
			std::copy_n(product.begin(), result.words.size(), result.words.begin());
			return result;
		}

		bool InvertField(const Field& value, Field& inverse)
		{
			if (IsZero(value))
				return false;

			WideLimbs u{};
			WideLimbs v{};
			WideLimbs g1{};
			WideLimbs g2{};
			std::copy(value.words.begin(), value.words.end(), u.begin());
			ToggleBit(v, 233);
			ToggleBit(v, 74);
			ToggleBit(v, 0);
			ToggleBit(g1, 0);

			for (unsigned iteration = 0; iteration < 1024 && Degree(u) > 0; ++iteration)
			{
				int shift = Degree(u) - Degree(v);
				if (shift < 0)
				{
					std::swap(u, v);
					std::swap(g1, g2);
					shift = -shift;
				}
				XorShifted(u, v, static_cast<unsigned>(shift));
				XorShifted(g1, g2, static_cast<unsigned>(shift));
			}
			if (Degree(u) != 0)
				return false;
			Reduce(g1);
			std::copy_n(g1.begin(), inverse.words.size(), inverse.words.begin());
			return true;
		}

		Field SquareField(const Field& value)
		{
			return MultiplyField(value, value);
		}

		Field DivideField(const Field& numerator, const Field& denominator, bool& ok)
		{
			Field inverse{};
			ok = InvertField(denominator, inverse);
			return ok ? MultiplyField(numerator, inverse) : Field{};
		}

		const Field& B()
		{
			static const Field value = FromBytes<Field>(CurveB);
			return value;
		}

		const InternalPoint& Generator()
		{
			static const InternalPoint value{FromBytes<Field>(GeneratorX), FromBytes<Field>(GeneratorY), false};
			return value;
		}

		InternalPoint DoublePoint(const InternalPoint& point)
		{
			if (point.infinity || IsZero(point.x))
				return {};
			bool ok = false;
			const Field quotient = DivideField(point.y, point.x, ok);
			if (!ok)
				return {};
			const Field lambda = AddField(point.x, quotient);
			Field x = AddField(AddField(SquareField(lambda), lambda), Field{{1, 0, 0, 0}});
			Field y = AddField(SquareField(point.x), MultiplyField(AddField(lambda, Field{{1, 0, 0, 0}}), x));
			return {x, y, false};
		}

		InternalPoint AddPoint(const InternalPoint& lhs, const InternalPoint& rhs)
		{
			if (lhs.infinity)
				return rhs;
			if (rhs.infinity)
				return lhs;
			if (Equal(lhs.x, rhs.x))
				return Equal(lhs.y, rhs.y) ? DoublePoint(lhs) : InternalPoint{};

			bool ok = false;
			const Field lambda = DivideField(AddField(lhs.y, rhs.y), AddField(lhs.x, rhs.x), ok);
			if (!ok)
				return {};
			Field x = AddField(AddField(AddField(SquareField(lambda), lambda), lhs.x), rhs.x);
			x = AddField(x, Field{{1, 0, 0, 0}});
			const Field y = AddField(AddField(MultiplyField(lambda, AddField(lhs.x, x)), x), lhs.y);
			return {x, y, false};
		}

		InternalPoint MultiplyPoint(const InternalPoint& point, const Scalar& scalar)
		{
			InternalPoint result{};
			InternalPoint addend = point;
			for (unsigned bit = 0; bit < 233; ++bit)
			{
				if (GetBit(scalar.words, bit))
					result = AddPoint(result, addend);
				addend = DoublePoint(addend);
			}
			return result;
		}

		bool IsOnCurve(const InternalPoint& point)
		{
			if (point.infinity)
				return true;
			const Field x2 = SquareField(point.x);
			const Field left = AddField(SquareField(point.y), MultiplyField(point.x, point.y));
			const Field right = AddField(AddField(MultiplyField(x2, point.x), x2), B());
			return Equal(left, right);
		}

		int CompareScalar(const Scalar& lhs, const Scalar& rhs)
		{
			for (int i = static_cast<int>(lhs.words.size()) - 1; i >= 0; --i)
			{
				if (lhs.words[i] < rhs.words[i])
					return -1;
				if (lhs.words[i] > rhs.words[i])
					return 1;
			}
			return 0;
		}

		Scalar SubtractScalar(const Scalar& lhs, const Scalar& rhs)
		{
			Scalar result{};
			uint64_t borrow = 0;
			for (size_t i = 0; i < result.words.size(); ++i)
			{
				const uint64_t rhsWithBorrow = rhs.words[i] + borrow;
				const bool carry = rhsWithBorrow < rhs.words[i];
				result.words[i] = lhs.words[i] - rhsWithBorrow;
				borrow = carry || lhs.words[i] < rhsWithBorrow;
			}
			return result;
		}

		const Scalar& Order()
		{
			static const Scalar value = FromBytes<Scalar>(GroupOrder);
			return value;
		}

		Scalar AddScalar(const Scalar& lhs, const Scalar& rhs)
		{
			Scalar result{};
			uint64_t carry = 0;
			for (size_t i = 0; i < result.words.size(); ++i)
			{
				const unsigned __int128 sum = static_cast<unsigned __int128>(lhs.words[i]) + rhs.words[i] + carry;
				result.words[i] = static_cast<uint64_t>(sum);
				carry = static_cast<uint64_t>(sum >> 64);
			}
			if (carry != 0 || CompareScalar(result, Order()) >= 0)
				result = SubtractScalar(result, Order());
			return result;
		}

		Scalar MultiplyScalar(const Scalar& lhs, const Scalar& rhs)
		{
			Scalar result{};
			Scalar addend = lhs;
			for (unsigned bit = 0; bit < 233; ++bit)
			{
				if (GetBit(rhs.words, bit))
					result = AddScalar(result, addend);
				addend = AddScalar(addend, addend);
			}
			return result;
		}

		Scalar InvertScalar(const Scalar& value)
		{
			Scalar exponent = Order();
			Scalar two{};
			two.words[0] = 2;
			exponent = SubtractScalar(exponent, two);
			Scalar result{};
			result.words[0] = 1;
			Scalar base = value;
			for (unsigned bit = 0; bit < 233; ++bit)
			{
				if (GetBit(exponent.words, bit))
					result = MultiplyScalar(result, base);
				base = MultiplyScalar(base, base);
			}
			return result;
		}

		bool IsValidScalar(const Scalar& value)
		{
			return !IsZero(value) && CompareScalar(value, Order()) < 0;
		}

		Scalar BitsToScalar(const uint8_t* data, size_t length)
		{
			Scalar value{};
			const size_t bits = std::min<size_t>(length * 8, 233);
			for (size_t i = 0; i < bits; ++i)
			{
				uint64_t carry = 0;
				for (size_t limb = 0; limb < value.words.size(); ++limb)
				{
					const uint64_t nextCarry = value.words[limb] >> 63;
					value.words[limb] = (value.words[limb] << 1) | carry;
					carry = nextCarry;
				}
				value.words[0] |= (data[i / 8] >> (7 - (i % 8))) & 1;
			}
			return value;
		}

		Scalar DigestToScalar(const uint8_t* digest, size_t digestLength)
		{
			Scalar value = BitsToScalar(digest, digestLength);
			if (CompareScalar(value, Order()) >= 0)
				value = SubtractScalar(value, Order());
			return value;
		}

		InternalPoint ImportPoint(const Point& point)
		{
			if (point.infinity)
				return {};
			return {FromBytes<Field>(point.x), FromBytes<Field>(point.y), false};
		}

		Point ExportPoint(const InternalPoint& point)
		{
			if (point.infinity)
				return {};
			return {ToBytes(point.x), ToBytes(point.y), false};
		}

		bool HasValidEncoding(const Point& point)
		{
			return point.infinity || ((point.x[0] & 0xfe) == 0 && (point.y[0] & 0xfe) == 0);
		}

		bool IsInSubgroup(const InternalPoint& point)
		{
			return MultiplyPoint(point, Order()).infinity;
		}

		class Rfc6979
		{
		  public:
			Rfc6979(const Element& privateKey, const Scalar& digest, HmacSha256 hmac) : m_hmac(hmac)
			{
				m_v.fill(0x01);
				m_k.fill(0x00);
				Element digestBytes = ToBytes(digest);
				std::array<uint8_t, 95> input{};
				std::copy(m_v.begin(), m_v.end(), input.begin());
				input[32] = 0;
				std::copy(privateKey.begin(), privateKey.end(), input.begin() + 33);
				std::copy(digestBytes.begin(), digestBytes.end(), input.begin() + 63);
				m_valid = Hmac(m_k.data(), m_k.size(), input.data(), 93, m_k) &&
						  Hmac(m_k.data(), m_k.size(), m_v.data(), m_v.size(), m_v);
				if (!m_valid)
					return;
				input[32] = 1;
				std::copy(m_v.begin(), m_v.end(), input.begin());
				m_valid = Hmac(m_k.data(), m_k.size(), input.data(), 93, m_k) &&
						  Hmac(m_k.data(), m_k.size(), m_v.data(), m_v.size(), m_v);
			}

			bool Next(Scalar& scalar)
			{
				while (m_valid)
				{
					m_valid = Hmac(m_k.data(), m_k.size(), m_v.data(), m_v.size(), m_v);
					if (!m_valid)
						return false;
					scalar = BitsToScalar(m_v.data(), m_v.size());
					if (IsValidScalar(scalar))
						return true;
					if (!Reject())
						return false;
				}
				return false;
			}

			bool Reject()
			{
				std::array<uint8_t, 33> input{};
				std::copy(m_v.begin(), m_v.end(), input.begin());
				input.back() = 0;
				m_valid = Hmac(m_k.data(), m_k.size(), input.data(), input.size(), m_k) &&
						  Hmac(m_k.data(), m_k.size(), m_v.data(), m_v.size(), m_v);
				return m_valid;
			}

		  private:
			bool Hmac(const uint8_t* key, size_t keyLength, const uint8_t* data, size_t dataLength,
					  std::array<uint8_t, 32>& output)
			{
				std::array<uint8_t, 32> temporary{};
				if (!m_hmac || !m_hmac(key, keyLength, data, dataLength, temporary.data()))
					return false;
				output = temporary;
				return true;
			}

			HmacSha256 m_hmac{};
			std::array<uint8_t, 32> m_k{};
			std::array<uint8_t, 32> m_v{};
			bool m_valid{true};
		};
	} // namespace

	bool IsValidPrivateKey(const Element& privateKey)
	{
		return (privateKey[0] & 0xfe) == 0 && IsValidScalar(FromBytes<Scalar>(privateKey));
	}

	bool IsValidPublicKey(const Point& publicKey)
	{
		if (publicKey.infinity || !HasValidEncoding(publicKey))
			return false;
		const InternalPoint point = ImportPoint(publicKey);
		return IsOnCurve(point) && IsInSubgroup(point);
	}

	bool GenerateKey(RandomBytes randomBytes, Element& privateKey, Point& publicKey)
	{
		if (!randomBytes)
			return false;
		for (unsigned attempt = 0; attempt < 128; ++attempt)
		{
			if (!randomBytes(privateKey.data(), privateKey.size()))
				return false;
			privateKey[0] &= 1;
			if (IsValidPrivateKey(privateKey))
				return PublicKeyFromPrivate(privateKey, publicKey);
		}
		privateKey.fill(0);
		publicKey = {};
		return false;
	}

	bool PublicKeyFromPrivate(const Element& privateKey, Point& publicKey)
	{
		if (!IsValidPrivateKey(privateKey))
			return false;
		const InternalPoint point = MultiplyPoint(Generator(), FromBytes<Scalar>(privateKey));
		if (point.infinity || !IsOnCurve(point))
			return false;
		publicKey = ExportPoint(point);
		return true;
	}

	bool MultiplyGenerator(const Element& scalar, Point& result)
	{
		if ((scalar[0] & 0xfe) != 0)
			return false;
		result = ExportPoint(MultiplyPoint(Generator(), FromBytes<Scalar>(scalar)));
		return true;
	}

	bool Multiply(const Point& point, const Element& scalar, Point& result)
	{
		if ((scalar[0] & 0xfe) != 0 || !HasValidEncoding(point))
			return false;
		const InternalPoint imported = ImportPoint(point);
		if (!IsOnCurve(imported))
			return false;
		result = ExportPoint(MultiplyPoint(imported, FromBytes<Scalar>(scalar)));
		return true;
	}

	bool Add(const Point& lhs, const Point& rhs, Point& result)
	{
		if (!HasValidEncoding(lhs) || !HasValidEncoding(rhs))
			return false;
		const InternalPoint left = ImportPoint(lhs);
		const InternalPoint right = ImportPoint(rhs);
		if (!IsOnCurve(left) || !IsOnCurve(right))
			return false;
		result = ExportPoint(AddPoint(left, right));
		return true;
	}

	bool Sign(const Element& privateKey, const uint8_t* digest, size_t digestLength,
			  HmacSha256 hmacSha256, Element& rBytes, Element& sBytes)
	{
		rBytes.fill(0);
		sBytes.fill(0);
		if (!IsValidPrivateKey(privateKey) || (!digest && digestLength != 0) || !hmacSha256)
			return false;
		const Scalar privateScalar = FromBytes<Scalar>(privateKey);
		const Scalar digestScalar = DigestToScalar(digest, digestLength);
		Rfc6979 nonce(privateKey, digestScalar, hmacSha256);
		for (unsigned attempt = 0; attempt < 128; ++attempt)
		{
			Scalar k{};
			if (!nonce.Next(k))
				return false;
			const InternalPoint point = MultiplyPoint(Generator(), k);
			if (point.infinity)
			{
				nonce.Reject();
				continue;
			}
			Scalar r = FromBytes<Scalar>(ToBytes(point.x));
			if (CompareScalar(r, Order()) >= 0)
				r = SubtractScalar(r, Order());
			if (IsZero(r))
			{
				nonce.Reject();
				continue;
			}
			const Scalar s = MultiplyScalar(InvertScalar(k), AddScalar(digestScalar, MultiplyScalar(r, privateScalar)));
			if (IsZero(s))
			{
				nonce.Reject();
				continue;
			}
			rBytes = ToBytes(r);
			sBytes = ToBytes(s);
			return true;
		}
		return false;
	}

	bool Verify(const Point& publicKey, const uint8_t* digest, size_t digestLength,
				const Element& rBytes, const Element& sBytes)
	{
		if ((!digest && digestLength != 0) || !IsValidPublicKey(publicKey))
			return false;
		const Scalar r = FromBytes<Scalar>(rBytes);
		const Scalar s = FromBytes<Scalar>(sBytes);
		if ((rBytes[0] & 0xfe) != 0 || (sBytes[0] & 0xfe) != 0 || !IsValidScalar(r) || !IsValidScalar(s))
			return false;
		const Scalar inverse = InvertScalar(s);
		const Scalar digestScalar = DigestToScalar(digest, digestLength);
		const InternalPoint first = MultiplyPoint(Generator(), MultiplyScalar(digestScalar, inverse));
		const InternalPoint second = MultiplyPoint(ImportPoint(publicKey), MultiplyScalar(r, inverse));
		const InternalPoint sum = AddPoint(first, second);
		if (sum.infinity)
			return false;
		Scalar x = FromBytes<Scalar>(ToBytes(sum.x));
		if (CompareScalar(x, Order()) >= 0)
			x = SubtractScalar(x, Order());
		return Equal(x, r);
	}

	bool SharedSecret(const Element& privateKey, const Point& publicKey, Element& secret)
	{
		secret.fill(0);
		if (!IsValidPrivateKey(privateKey) || !IsValidPublicKey(publicKey))
			return false;
		const InternalPoint point = MultiplyPoint(ImportPoint(publicKey), FromBytes<Scalar>(privateKey));
		if (point.infinity)
			return false;
		secret = ToBytes(point.x);
		return true;
	}
} // namespace switch_crypto::sect233r1
