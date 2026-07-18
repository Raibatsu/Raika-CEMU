extern "C" {
#include <switch/applets/swkbd.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "platform/switch/SwitchSwkbd.h"


namespace
{
enum class AppletState
{
	Inactive,
	PauseRequested,
	GpuIdle,
};

std::atomic<AppletState> g_appletState{AppletState::Inactive};
std::mutex g_appletMutex;
std::condition_variable g_appletCondition;

std::string u16ToU8(const char16_t* s)
{
	std::string out;
	if (!s)
		return out;
	for (size_t i = 0; s[i];)
	{
		uint32_t cp = s[i++];
		if (cp >= 0xD800 && cp <= 0xDBFF)
		{
			if (s[i] >= 0xDC00 && s[i] <= 0xDFFF)
				cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i++] - 0xDC00);
			else
				cp = 0xFFFD;
		}
		else if (cp >= 0xDC00 && cp <= 0xDFFF)
			cp = 0xFFFD;
		if (cp < 0x80)
			out.push_back((char)cp);
		else if (cp < 0x800)
		{
			out.push_back((char)(0xC0 | (cp >> 6)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		}
		else if (cp < 0x10000)
		{
			out.push_back((char)(0xE0 | (cp >> 12)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		}
		else
		{
			out.push_back((char)(0xF0 | (cp >> 18)));
			out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

void u8ToU16(const char* s, char16_t* out, int cap)
{
	const size_t inputLength = std::strlen(s);
	int n = 0;
	for (size_t i = 0; i < inputLength && n < cap - 1;)
	{
		const uint8_t c = (uint8_t)s[i];
		uint32_t cp = 0;
		uint32_t minimum = 0;
		int len = 0;
		if (c < 0x80)
		{
			cp = c;
			len = 1;
		}
		else if ((c & 0xE0) == 0xC0)
		{
			cp = c & 0x1F;
			minimum = 0x80;
			len = 2;
		}
		else if ((c & 0xF0) == 0xE0)
		{
			cp = c & 0x0F;
			minimum = 0x800;
			len = 3;
		}
		else if ((c & 0xF8) == 0xF0)
		{
			cp = c & 0x07;
			minimum = 0x10000;
			len = 4;
		}
		else
		{
			i++;
			continue;
		}

		bool valid = true;
		if (i + len > inputLength)
			valid = false;
		for (int k = 1; k < len; k++)
		{
			if (!valid)
				break;
			const uint8_t continuation = (uint8_t)s[i + k];
			if ((continuation & 0xC0) != 0x80)
			{
				valid = false;
				break;
			}
			cp = (cp << 6) | (continuation & 0x3F);
		}
		if (!valid || cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		{
			i++;
			continue;
		}

		if (cp >= 0x10000)
		{
			if (n + 2 >= cap)
				break;
			cp -= 0x10000;
			out[n++] = (char16_t)(0xD800 + (cp >> 10));
			out[n++] = (char16_t)(0xDC00 + (cp & 0x3FF));
		}
		else
			out[n++] = (char16_t)cp;
		i += len;
	}
	out[n] = 0;
}
} // namespace

extern "C" bool SwitchSwkbd_IsAppletActive(void)
{
	return g_appletState.load(std::memory_order_acquire) != AppletState::Inactive;
}

extern "C" void SwitchSwkbd_NotifyGpuIdle(void)
{
	{
		std::scoped_lock lock(g_appletMutex);
		if (g_appletState.load(std::memory_order_relaxed) != AppletState::PauseRequested)
			return;
		g_appletState.store(AppletState::GpuIdle, std::memory_order_release);
	}
	g_appletCondition.notify_all();
}

extern "C" bool SwitchSwkbd_Show(const char16_t* initialU16, int maxLen, char16_t* outU16, int outCap)
{
	if (!outU16 || outCap < 2)
		return false;
	outU16[0] = 0;

	AppletState expected = AppletState::Inactive;
	if (!g_appletState.compare_exchange_strong(expected, AppletState::PauseRequested, std::memory_order_acq_rel))
		return false;
	{
		std::unique_lock lock(g_appletMutex);
		if (!g_appletCondition.wait_for(lock, std::chrono::seconds(10), [] {
			return g_appletState.load(std::memory_order_acquire) == AppletState::GpuIdle;
		}))
		{
			g_appletState.store(AppletState::Inactive, std::memory_order_release);
			return false;
		}
	}

	bool ok = false;
	try
	{
		SwkbdConfig cfg{};
		if (R_SUCCEEDED(swkbdCreate(&cfg, 0)))
		{
			struct ConfigGuard
			{
				SwkbdConfig* config;
				~ConfigGuard() { swkbdClose(config); }
			} guard{&cfg};
			swkbdConfigMakePresetDefault(&cfg);
			const int chars = std::clamp(maxLen, 1, outCap - 1);
			swkbdConfigSetStringLenMax(&cfg, (u32)chars);
			std::string initU8 = u16ToU8(initialU16);
			if (!initU8.empty())
				swkbdConfigSetInitialText(&cfg, initU8.c_str());

			std::vector<char> outU8((size_t)(chars + 1) * 4 + 1);
			if (R_SUCCEEDED(swkbdShow(&cfg, outU8.data(), outU8.size())))
			{
				u8ToU16(outU8.data(), outU16, outCap);
				ok = true;
			}
		}
	}
	catch (...)
	{
	}

	{
		std::scoped_lock lock(g_appletMutex);
		g_appletState.store(AppletState::Inactive, std::memory_order_release);
	}
	g_appletCondition.notify_all();
	return ok;
}
