#include <switch.h>
#include <map>
#include <mutex>
#include <unordered_map>

#include "platform/switch/SwitchJit.h"

namespace
{
	Jit s_jit{};
	bool s_ready = false;
	uint8_t* s_rwBase = nullptr;
	size_t s_size = 0;
	size_t s_cursor = 0;
	std::map<size_t, size_t> s_freeBlocks;
	std::unordered_map<uintptr_t, size_t> s_allocations;
	std::mutex s_mutex;
	uintptr_t s_rxDelta = 0;

	constexpr size_t kCodeAlignment = 16;
	bool AlignCodeSize(size_t size, size_t& alignedSize)
	{
		if (size == 0 || size > SIZE_MAX - (kCodeAlignment - 1))
			return false;
		alignedSize = (size + kCodeAlignment - 1) & ~(kCodeAlignment - 1);
		return true;
	}
}

bool SwitchJit_SyscallsAvailable()
{
	return envIsSyscallHinted(0x4B) && envIsSyscallHinted(0x4C);
}

bool SwitchJit_InitCodeArena(size_t size)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_ready)
		return true;
	if (size == 0 || (size & 0xFFF) != 0)
		return false;
	if (!SwitchJit_SyscallsAvailable())
		return false;

	Result rc = jitCreate(&s_jit, size);
	if (R_FAILED(rc))
		return false;
	if (s_jit.type != JitType_CodeMemory)
	{
		jitClose(&s_jit);
		return false;
	}
	rc = jitTransitionToExecutable(&s_jit);
	if (R_FAILED(rc))
	{
		jitClose(&s_jit);
		return false;
	}
	s_rwBase = static_cast<uint8_t*>(jitGetRwAddr(&s_jit));
	void* rxBase = jitGetRxAddr(&s_jit);
	if (!s_rwBase || !rxBase)
	{
		jitClose(&s_jit);
		s_rwBase = nullptr;
		return false;
	}
	s_rxDelta = reinterpret_cast<uintptr_t>(rxBase) - reinterpret_cast<uintptr_t>(s_rwBase);
	s_size = size;
	s_cursor = 0;
	s_freeBlocks.clear();
	s_allocations.clear();
	s_ready = true;
	return true;
}

void* SwitchJit_AllocRw(size_t size)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_ready)
		return nullptr;
	if (!AlignCodeSize(size, size))
		return nullptr;

	for (auto it = s_freeBlocks.begin(); it != s_freeBlocks.end(); ++it)
	{
		if (it->second < size)
			continue;
		const size_t offset = it->first;
		const size_t blockSize = it->second;
		s_freeBlocks.erase(it);
		if (blockSize > size)
			s_freeBlocks.emplace(offset + size, blockSize - size);
		void* address = s_rwBase + offset;
		s_allocations.emplace(reinterpret_cast<uintptr_t>(address), size);
		return address;
	}

	if (s_cursor > s_size || size > s_size - s_cursor)
		return nullptr;

	void* p = s_rwBase + s_cursor;
	s_cursor += size;
	s_allocations.emplace(reinterpret_cast<uintptr_t>(p), size);
	return p;
}

bool SwitchJit_FreeRw(void* address)
{
	if (!address)
		return true;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_ready)
		return false;

	uintptr_t ptr = reinterpret_cast<uintptr_t>(address);
	const uintptr_t rwBegin = reinterpret_cast<uintptr_t>(s_rwBase);
	const uintptr_t rxBegin = reinterpret_cast<uintptr_t>(SwitchJit_RwToRx(s_rwBase));
	if (ptr >= rxBegin && ptr - rxBegin < s_size)
		ptr = rwBegin + (ptr - rxBegin);

	auto allocation = s_allocations.find(ptr);
	if (allocation == s_allocations.end())
		return false;

	size_t begin = ptr - rwBegin;
	const size_t allocationSize = allocation->second;
	size_t end = begin + allocationSize;
	s_allocations.erase(allocation);

	auto next = s_freeBlocks.lower_bound(begin);
	if (next != s_freeBlocks.begin())
	{
		auto previous = std::prev(next);
		if (previous->first + previous->second == begin)
		{
			begin = previous->first;
			s_freeBlocks.erase(previous);
		}
	}
	next = s_freeBlocks.lower_bound(begin);
	if (next != s_freeBlocks.end() && end == next->first)
	{
		end += next->second;
		s_freeBlocks.erase(next);
	}
	if (end == s_cursor)
	{
		s_cursor = begin;
		return true;
	}
	s_freeBlocks.emplace(begin, end - begin);
	return true;
}

void SwitchJit_FlushCode(void* rwAddr, size_t size)
{
	if (!rwAddr || size == 0)
		return;
	armDCacheClean(rwAddr, size);
	void* rxAddr = SwitchJit_RwToRx(rwAddr);
	armICacheInvalidate(rxAddr, size);
}

void* SwitchJit_RwToRx(void* rw)
{
	return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(rw) + s_rxDelta);
}

void SwitchJit_PinThreadToCore(int coreIndex)
{
	if (coreIndex < 0 || coreIndex > 2)
		return;
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, coreIndex, 1u << coreIndex);
}
