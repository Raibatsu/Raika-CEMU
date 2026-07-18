#include "util/MemMapper/MemMapper.h"

#include <switch.h>
#include <mutex>
#include <map>
#include <vector>
#include <cstdlib>
#include <cstring>

#include "platform/switch/SwitchMemoryBudget.h"

// Dedicated backing for sparse guest mappings.
extern void* g_switchGuestPoolBase;
extern size_t g_switchGuestPoolSize;

namespace MemMapper
{
	static constexpr size_t kPageSize = 0x1000;

	size_t GetPageSize() { return kPageSize; }

	namespace
	{
		std::mutex s_mutex;

		bool alignToPage(size_t size, size_t& alignedSize)
		{
			if (size == 0 || size > SIZE_MAX - (kPageSize - 1))
				return false;
			alignedSize = (size + kPageSize - 1) & ~(kPageSize - 1);
			return true;
		}

		bool s_poolInit = false;
		uintptr_t s_poolCursor = 0;
		uintptr_t s_poolEnd = 0;
		std::map<uintptr_t, size_t> s_freePoolBlocks;

		struct PoolMap
		{
			void* dst;
			size_t size;
			Handle handle;
			void* source;
			bool heapSource;
			bool retained;
		};
		std::vector<PoolMap> s_poolMaps;
		struct DirectAllocation
		{
			size_t size;
			bool heapSource;
		};
		std::map<uintptr_t, DirectAllocation> s_directAllocations;
		struct AddressReservation
		{
			void* base;
			size_t size;
			VirtmemReservation* reservation;
		};
		std::vector<AddressReservation> s_reservations;
		uintptr_t s_reserveBase = 0;
		size_t s_reserveSize = 0;
		uintptr_t s_instBase = 0, s_instEnd = 0;

		void ensurePool()
		{
			if (s_poolInit)
				return;
			s_poolInit = true;
			const uintptr_t poolBase = reinterpret_cast<uintptr_t>(g_switchGuestPoolBase);
			if (!poolBase || g_switchGuestPoolSize > UINTPTR_MAX - poolBase || poolBase > UINTPTR_MAX - (kPageSize - 1))
				return;
			s_poolCursor = (poolBase + kPageSize - 1) & ~(uintptr_t)(kPageSize - 1);
			s_poolEnd = poolBase + g_switchGuestPoolSize;
			if (s_poolCursor > s_poolEnd)
				s_poolCursor = s_poolEnd = 0;
			s_poolMaps.reserve(32);
			s_reservations.reserve(2);
		}

		void* virtReserve(void* requestedBase, size_t size, VirtmemReservation*& reservation)
		{
			reservation = nullptr;
			virtmemLock();
			void* address = requestedBase ? requestedBase : virtmemFindAslr(size, 0);
			if (address)
				reservation = virtmemAddReservation(address, size);
			virtmemUnlock();
			return reservation ? address : nullptr;
		}

		bool takePoolSource(size_t size, void*& source)
		{
			ensurePool();
			for (auto it = s_freePoolBlocks.begin(); it != s_freePoolBlocks.end(); ++it)
			{
				if (it->second < size)
					continue;
				const uintptr_t address = it->first;
				const size_t blockSize = it->second;
				s_freePoolBlocks.erase(it);
				if (blockSize > size)
					s_freePoolBlocks.emplace(address + size, blockSize - size);
				source = reinterpret_cast<void*>(address);
				return true;
			}
			if (s_poolCursor == 0 || s_poolCursor > s_poolEnd || size > s_poolEnd - s_poolCursor)
				return false;
			source = reinterpret_cast<void*>(s_poolCursor);
			s_poolCursor += size;
			return true;
		}

		void releasePoolSource(void* source, size_t size)
		{
			uintptr_t begin = reinterpret_cast<uintptr_t>(source);
			uintptr_t end = begin + size;
			auto next = s_freePoolBlocks.lower_bound(begin);
			if (next != s_freePoolBlocks.begin())
			{
				auto previous = std::prev(next);
				if (previous->first + previous->second == begin)
				{
					begin = previous->first;
					s_freePoolBlocks.erase(previous);
				}
			}
			next = s_freePoolBlocks.lower_bound(begin);
			if (next != s_freePoolBlocks.end() && end == next->first)
			{
				end += next->second;
				s_freePoolBlocks.erase(next);
			}
			if (end == s_poolCursor)
			{
				s_poolCursor = begin;
				return;
			}
			s_freePoolBlocks.emplace(begin, end - begin);
		}

		void* directAllocate(size_t size)
		{
			if (!alignToPage(size, size))
				return nullptr;

			void* allocation = nullptr;
			bool heapSource = false;
			if (!takePoolSource(size, allocation))
			{
				allocation = aligned_alloc(kPageSize, size);
				heapSource = allocation != nullptr;
			}
			if (!allocation)
				return nullptr;

			s_directAllocations.emplace(reinterpret_cast<uintptr_t>(allocation),
				DirectAllocation{size, heapSource});
			return allocation;
		}

		void directRelease(void* allocation)
		{
			auto it = s_directAllocations.find(reinterpret_cast<uintptr_t>(allocation));
			if (it == s_directAllocations.end())
			{
				free(allocation);
				return;
			}

			const DirectAllocation metadata = it->second;
			s_directAllocations.erase(it);
			if (metadata.heapSource)
				free(allocation);
			else
				releasePoolSource(allocation, metadata.size);
		}

		bool poolCommit(void* dst, size_t size, bool allowHeapSource = false, bool retained = false,
			bool clearMemory = true)
		{
			if ((reinterpret_cast<uintptr_t>(dst) & (kPageSize - 1)) != 0 || !alignToPage(size, size))
				return false;
			void* source = nullptr;
			bool heapSource = false;
			if (!takePoolSource(size, source))
			{
				if (allowHeapSource)
				{
					source = aligned_alloc(kPageSize, size);
					heapSource = source != nullptr;
				}
				if (!source)
					return false;
			}
			auto releaseSource = [&]() {
				if (heapSource)
					free(source);
				else
					releasePoolSource(source, size);
			};
			Handle handle = INVALID_HANDLE;
			Result rc = svcCreateCodeMemory(&handle, source, size);
			if (R_FAILED(rc))
			{
				releaseSource();
				return false;
			}
			rc = svcControlCodeMemory(handle, CodeMapOperation_MapOwner, dst, size, Perm_Rw);
			if (R_FAILED(rc))
			{
				svcCloseHandle(handle);
				releaseSource();
				return false;
			}
			if (clearMemory)
				std::memset(dst, 0, size);
			s_poolMaps.push_back({dst, size, handle, source, heapSource, retained});
			return true;
		}

		void poolRelease(void* dst, size_t size)
		{
			const uintptr_t address = reinterpret_cast<uintptr_t>(dst);
			for (size_t i = 0; i < s_poolMaps.size();)
			{
				const uintptr_t mapStart = reinterpret_cast<uintptr_t>(s_poolMaps[i].dst);
				if (s_poolMaps[i].retained && address >= mapStart && address - mapStart < s_poolMaps[i].size)
				{
					++i;
					continue;
				}
				const bool isInstanceAddress = address >= s_instBase && address < s_instEnd;
				const bool startsInRange = mapStart >= address && mapStart - address < size;
				if (s_poolMaps[i].dst != dst && !startsInRange && !(isInstanceAddress && address >= mapStart && address - mapStart < s_poolMaps[i].size))
				{
					++i;
					continue;
				}
				const PoolMap mapping = s_poolMaps[i];
				Result rc = svcControlCodeMemory(mapping.handle, CodeMapOperation_UnmapOwner, mapping.dst, mapping.size, 0);
				if (R_FAILED(rc))
				{
					++i;
					continue;
				}
				svcCloseHandle(mapping.handle);
				s_poolMaps.erase(s_poolMaps.begin() + i);
				if (mapping.heapSource)
					free(mapping.source);
				else
					releasePoolSource(mapping.source, mapping.size);
			}
		}

		// Coalescing adjacent ranges conserves Horizon kernel objects.
		struct Zone { uintptr_t start, end; };
		const Zone kZones[] = {
			{0x00010000, 0x50000000},     // LOW0 + TRAMPOLINE + CODECAVE + TEXT + CEMU + MEM2
			{0xF4000000, 0xFA000000},     // MEM1 + RPLLOADER + SHARED
			{0xFFC00000, 0x100000000ull}, // CORE0/1/2 locked cache + PER-CORE
		};

		bool commitGuest(void* dst, size_t size)
		{
			uintptr_t off = (uintptr_t)dst - s_reserveBase;
			for (const Zone& z : kZones)
			{
				if (off < z.start || off >= z.end)
					continue;
				void* zoneDst = (void*)(s_reserveBase + z.start);
				bool haveZone = false;
				for (const PoolMap& mapping : s_poolMaps)
				{
					if (mapping.dst == zoneDst)
					{
						haveZone = true;
						break;
					}
				}
				if (!haveZone && !poolCommit(zoneDst, z.end - z.start, false, true, false))
					return false;

				const uintptr_t requestEnd = off + size;
				if (requestEnd > z.end)
				{
					void* extensionDst = reinterpret_cast<void*>(s_reserveBase + z.end);
					const size_t extensionSize = requestEnd - z.end;
					bool haveExtension = false;
					for (const PoolMap& mapping : s_poolMaps)
					{
						if (mapping.dst != extensionDst)
							continue;
						if (extensionSize > mapping.size)
							return false;
						haveExtension = true;
						break;
					}
					if (!haveExtension && !poolCommit(extensionDst, extensionSize, true, false, false))
						return false;
				}
				std::memset(dst, 0, size);
				return true;
			}
			for (const PoolMap& mapping : s_poolMaps)
			{
				if (mapping.dst != dst)
					continue;
				if (size > mapping.size)
					return false;
				std::memset(dst, 0, size);
				return true;
			}
			return poolCommit(dst, size, true);
		}

		bool chunkCommit(void* dst, size_t size)
		{
			const uintptr_t chunkSize = SwitchMemoryBudget_GetJumpTableChunkSize();
			if (!alignToPage(size, size))
				return false;
			uintptr_t address = reinterpret_cast<uintptr_t>(dst);
			if (address < s_instBase || address >= s_instEnd)
				return false;
			if (size > s_instEnd - address)
				return false;
			uintptr_t end = address + size;
			uintptr_t first = s_instBase + (((uintptr_t)dst - s_instBase) & ~(chunkSize - 1));
			for (uintptr_t c = first; c < end; c += chunkSize)
			{
				uintptr_t ce = (c + chunkSize > s_instEnd) ? s_instEnd : c + chunkSize;
				bool have = false;
				for (const PoolMap& m : s_poolMaps)
					if (m.dst == (void*)c) { have = true; break; }
				if (!have && !poolCommit((void*)c, ce - c, true, false, false))
					return false;
			}
			std::memset(dst, 0, size);
			return true;
		}
	} // namespace

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		size_t alignedSize = 0;
		if (!alignToPage(size, alignedSize))
			return nullptr;
		VirtmemReservation* reservation = nullptr;
		void* p = virtReserve(baseAddr, alignedSize, reservation);
		if (p)
		{
			s_reservations.push_back({p, alignedSize, reservation});
			if (size >= 0x80000000ull) { s_reserveBase = (uintptr_t)p; s_reserveSize = alignedSize; }
			else { s_instBase = (uintptr_t)p; s_instEnd = (uintptr_t)p + alignedSize; }
		}
		return p;
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		size_t alignedSize = 0;
		if (!baseAddr || !alignToPage(size, alignedSize))
			return;
		for (auto it = s_reservations.begin(); it != s_reservations.end(); ++it)
		{
			if (it->base != baseAddr || it->size != alignedSize)
				continue;
			const uintptr_t begin = reinterpret_cast<uintptr_t>(baseAddr);
			for (const PoolMap& mapping : s_poolMaps)
			{
				const uintptr_t mapBegin = reinterpret_cast<uintptr_t>(mapping.dst);
				if (mapBegin >= begin && mapBegin - begin < alignedSize)
					return;
			}
			virtmemLock();
			virtmemRemoveReservation(it->reservation);
			virtmemUnlock();
			if (begin == s_reserveBase)
				s_reserveBase = s_reserveSize = 0;
			if (begin == s_instBase)
				s_instBase = s_instEnd = 0;
			s_reservations.erase(it);
			return;
		}
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION, bool fromReservation)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (fromReservation && baseAddr)
		{
			uintptr_t a = (uintptr_t)baseAddr;
			if (a >= s_reserveBase && a - s_reserveBase < s_reserveSize)
			{
				if (size > s_reserveSize - (a - s_reserveBase))
					return nullptr;
				return commitGuest(baseAddr, size) ? baseAddr : nullptr;
			}
			if (a >= s_instBase && a < s_instEnd)
				return chunkCommit(baseAddr, size) ? baseAddr : nullptr;
			return baseAddr;
		}
		if (baseAddr)
			return baseAddr;
		return directAllocate(size);
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (fromReservation && baseAddr)
			poolRelease(baseAddr, size);
		else if (!fromReservation && baseAddr)
			directRelease(baseAddr);
	}
}; // namespace MemMapper
