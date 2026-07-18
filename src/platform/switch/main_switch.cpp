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
		if (heapBase && targetGuestPool != 0 && targetGuestPool < heapTotal)
		{
			const size_t heapSize = (heapTotal - targetGuestPool) & ~0x1FFFFFull;
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapSize;
			g_switchGuestPoolBase = heapBase + heapSize;
			g_switchGuestPoolSize = heapTotal - heapSize;
			g_switchNewlibHeapSize = heapSize;
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
	SwitchStorage::InitializeFromConfig("sdmc:/switch/Cemu/launcher.ini");
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
		envSetNextLoad("sdmc:/switch/Cemu/Cemu.nro", "sdmc:/switch/Cemu/Cemu.nro");
	SwitchPlatformExit();
	std::_Exit(status);
}
