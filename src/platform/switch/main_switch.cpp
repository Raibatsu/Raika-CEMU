#include <switch.h>
#include <atomic>
#include <cstdlib>

#include "WindowSystem.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "audio/SwitchAudioAPI.h"
#include "config/LaunchSettings.h"
#include "SwitchPlatform.h"
#include "SwitchMemoryBudget.h"
#include "common/SwitchStorage.h"

std::atomic_bool g_isGPUInitFinished = false;

void* g_switchGuestPoolBase = nullptr;
size_t g_switchGuestPoolSize = 0;
size_t g_switchNewlibHeapSize = 0;
size_t g_switchHeapTotalSize = 0;
size_t g_switchInheritedHeapSize = 0;
size_t g_switchRecoveredHeapSize = 0;
u64 g_switchProcessMemoryTotal = 0;
u64 g_switchProcessMemoryUsed = 0;
Result g_switchHeapExpansionResult = 0;
bool g_switchHeapExpansionAttempted = false;

namespace
{
	constexpr size_t kHeapAlignment = 0x200000;
	constexpr size_t kApplicationReserve = 96 * 1024 * 1024;

	bool RecoverGuestPoolFromApplicationHeap(char* overrideBase, size_t overrideSize)
	{
		u64 heapRegionAddress = 0;
		if (R_FAILED(svcGetInfo(&heapRegionAddress, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
			R_FAILED(svcGetInfo(&g_switchProcessMemoryTotal, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) ||
			R_FAILED(svcGetInfo(&g_switchProcessMemoryUsed, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))
			return false;

		const uintptr_t inheritedStart = reinterpret_cast<uintptr_t>(overrideBase);
		if (overrideSize > UINTPTR_MAX - inheritedStart)
			return false;
		const uintptr_t inheritedEnd = inheritedStart + overrideSize;
		if (heapRegionAddress > inheritedStart || inheritedEnd < heapRegionAddress)
			return false;

		const u64 currentHeapSize64 = inheritedEnd - heapRegionAddress;
		if (currentHeapSize64 > SIZE_MAX || g_switchProcessMemoryTotal <= g_switchProcessMemoryUsed + kApplicationReserve)
			return false;

		size_t expandable = static_cast<size_t>(g_switchProcessMemoryTotal -
			g_switchProcessMemoryUsed - kApplicationReserve) & ~(kHeapAlignment - 1);
		if (expandable < SwitchMemoryBudget_GetMandatoryGuestBackingSize() ||
			overrideSize > SIZE_MAX - expandable)
			return false;

		const size_t desiredPool = SwitchMemoryBudget_ComputeGuestPoolSize(overrideSize + expandable);
		if (desiredPool == 0)
			return false;
		size_t expansionSize = (desiredPool + kHeapAlignment - 1) & ~(kHeapAlignment - 1);
		if (expansionSize > expandable)
			expansionSize = expandable;
		if (expansionSize < SwitchMemoryBudget_GetMandatoryGuestBackingSize())
			return false;

		const size_t currentHeapSize = static_cast<size_t>(currentHeapSize64);
		if (currentHeapSize > SIZE_MAX - expansionSize)
			return false;

		void* expandedHeapBase = nullptr;
		g_switchHeapExpansionAttempted = true;
		g_switchHeapExpansionResult = svcSetHeapSize(&expandedHeapBase, currentHeapSize + expansionSize);
		if (R_FAILED(g_switchHeapExpansionResult) ||
			reinterpret_cast<uintptr_t>(expandedHeapBase) != heapRegionAddress)
			return false;

		g_switchGuestPoolBase = reinterpret_cast<void*>(inheritedEnd);
		g_switchGuestPoolSize = expansionSize;
		g_switchRecoveredHeapSize = expansionSize;
		return true;
	}
}

extern "C"
{
	u32 __nx_applet_type = AppletType_Application;

	extern char* fake_heap_start;
	extern char* fake_heap_end;

	void __libnx_initheap(void)
	{
		char* heapBase = nullptr;
		size_t heapTotal = 0;
		if (envHasHeapOverride())
		{
			heapBase = (char*)envGetHeapOverrideAddr();
			heapTotal = envGetHeapOverrideSize();
			if (!heapBase || heapTotal == 0)
				diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGetInfo_Heap));
			g_switchInheritedHeapSize = heapTotal;
		}
		else
		{
			u64 avail = 0, used = 0;
			Result result = svcGetInfo(&avail, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
			if (R_FAILED(result))
				diagAbortWithResult(result);
			result = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
			if (R_FAILED(result))
				diagAbortWithResult(result);
			if (avail <= used + 0x200000)
				diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
			size_t sz = (avail - used - 0x200000) & ~0x1FFFFFull;
			void* a = nullptr;
			result = svcSetHeapSize(&a, sz);
			if (R_FAILED(result))
				diagAbortWithResult(result);
			if (!a)
				diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
			heapBase = (char*)a;
			heapTotal = sz;
		}

		const size_t targetGuestPool = SwitchMemoryBudget_ComputeGuestPoolSize(heapTotal);
		const bool recoveredGuestPool = targetGuestPool == 0 && envHasHeapOverride() &&
			RecoverGuestPoolFromApplicationHeap(heapBase, heapTotal);
		g_switchHeapTotalSize = heapTotal + g_switchRecoveredHeapSize;
		if (heapBase && targetGuestPool != 0 && targetGuestPool < heapTotal)
		{
			const size_t heapSize = (heapTotal - targetGuestPool) & ~0x1FFFFFull;
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapSize;
			g_switchGuestPoolBase = heapBase + heapSize;
			g_switchGuestPoolSize = heapTotal - heapSize;
			g_switchNewlibHeapSize = heapSize;
		}
		else if (recoveredGuestPool)
		{
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapTotal;
			g_switchNewlibHeapSize = heapTotal;
		}
		else
		{
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapTotal;
			g_switchNewlibHeapSize = heapTotal;
		}
	}
}

void requireConsole() {}

static bool s_romfsMounted = false;
static bool s_hidInitialized = false;
static bool s_socketInitialized = false;
static bool s_storageInitialized = false;

static bool SwitchPlatformInit()
{
	s_romfsMounted = R_SUCCEEDED(romfsInit());
	s_hidInitialized = R_SUCCEEDED(hidInitialize());
	if (s_hidInitialized)
		padConfigureInput(8, HidNpadStyleSet_NpadStandard);
	s_socketInitialized = R_SUCCEEDED(socketInitializeDefault());
	SwitchStorage::InitializeFromConfig("sdmc:/switch/cemu/launcher.ini");
	s_storageInitialized = true;
	return s_romfsMounted && s_hidInitialized;
}

static void SwitchPlatformExit()
{
	SwitchAudioAPI::Destroy();
	if (s_storageInitialized)
	{
		SwitchStorage::Shutdown();
		s_storageInitialized = false;
	}
	if (s_socketInitialized)
		socketExit();
	if (s_hidInitialized)
		hidExit();
	if (s_romfsMounted)
		romfsExit();
}

int main(int argc, char* argv[])
{
	const bool platformInitialized = SwitchPlatformInit();
	if (!platformInitialized || !LaunchSettings::HandleCommandline(argc, argv))
	{
		SwitchPlatformExit();
		std::_Exit(EXIT_FAILURE);
	}

	int status = EXIT_SUCCESS;
	try
	{
		ExceptionHandler_Init();
		WindowSystem::Create();
	}
	catch (...)
	{
		status = EXIT_FAILURE;
	}

	if (SwitchPlatform_ShouldReturnToLauncher() && envHasNextLoad())
		envSetNextLoad("sdmc:/switch/cemu/cemu.nro", "sdmc:/switch/cemu/cemu.nro");
	SwitchPlatformExit();
	std::_Exit(status);
}
