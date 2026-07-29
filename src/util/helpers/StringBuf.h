#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

class StringBuf
{
public:
	StringBuf(uint32 bufferSize)
	{
		this->str = static_cast<uint8*>(malloc(static_cast<size_t>(bufferSize) + 4));
		if (!this->str)
			throw std::bad_alloc();
		this->allocated = true;
		this->length = 0;
		this->limit = bufferSize;
		this->str[0] = '\0';
	}

	~StringBuf()
	{
		if (this->allocated)
			free(this->str);
	}

	template<typename TFmt, typename ... TArgs>
	void addFmt(const TFmt& format, TArgs&&... args)
	{
		const auto formatView = fmt::detail::to_string_view(format);
		size_t available = this->limit - this->length;
		auto result = fmt::vformat_to_n(reinterpret_cast<char*>(this->str + this->length),
			available, formatView, fmt::make_format_args(args...));
		if (result.size > available)
		{
			_ensureCapacity(result.size);
			result = fmt::vformat_to_n(reinterpret_cast<char*>(this->str + this->length),
				this->limit - this->length, formatView, fmt::make_format_args(args...));
		}
		this->length += static_cast<uint32>(result.size);
		this->str[this->length] = '\0';
	}

	void add(const char* appendedStr)
	{
		while (true)
		{
			const char c = *appendedStr++;
			if (c == '\0')
			{
				this->str[this->length] = '\0';
				return;
			}
			if (this->length == this->limit)
				_ensureCapacity(1);
			this->str[this->length++] = static_cast<uint8>(c);
		}
	}

	void add(std::string_view appendedStr)
	{
		const size_t copyLen = appendedStr.size();
		_ensureCapacity(copyLen);
		if (copyLen != 0)
			memcpy(this->str + this->length, appendedStr.data(), copyLen);
		this->length += static_cast<uint32>(copyLen);
		this->str[this->length] = '\0';
	}

	void reset()
	{
		length = 0;
		str[0] = '\0';
	}

	uint32 getLen() const
	{
		return length;
	}

	const char* c_str() const
	{
		str[length] = '\0';
		return (const char*)str;
	}

	void shrink_to_fit()
	{
		if (!this->allocated || this->length == this->limit)
			return;
		const uint32 newLimit = this->length;
		void* newStr = realloc(this->str, static_cast<size_t>(newLimit) + 4);
		if (newStr)
		{
			this->str = static_cast<uint8*>(newStr);
			this->limit = newLimit;
		}
	}

private:
	void _ensureCapacity(size_t additionalLength)
	{
		constexpr uint64 kMaxLength = std::numeric_limits<uint32>::max();
		if (additionalLength > kMaxLength - this->length)
			throw std::length_error("StringBuf capacity exceeded");
		const uint64 requiredLimit = static_cast<uint64>(this->length) + additionalLength;
		if (requiredLimit <= this->limit)
			return;
		const uint64 grownLimit = std::max<uint64>(requiredLimit,
			std::max<uint64>(64, static_cast<uint64>(this->limit) * 2));
		_reserve(static_cast<uint32>(std::min<uint64>(grownLimit, kMaxLength)));
	}

	void _reserve(uint32 newLimit)
	{
		cemu_assert_debug(newLimit > length);
		void* newStr = realloc(this->str, static_cast<size_t>(newLimit) + 4);
		if (!newStr)
			throw std::bad_alloc();
		this->str = static_cast<uint8*>(newStr);
		this->limit = newLimit;
	}

	uint8*	str;
	uint32	length; /* in bytes */
	uint32	limit; /* in bytes */
	bool	allocated;
};
