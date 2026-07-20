#include <switch.h>

#include "WindowSystem.h"

#include "util/crypto/aes128.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/NetworkSettings.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"

#include "input/InputManager.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "audio/SwitchAudioAPI.h"
#include "platform/switch/SwitchAmiibo.h"
#include "platform/switch/SwitchSwkbd.h"
#include "platform/switch/SwitchInput.h"
#include "platform/switch/SwitchJit.h"
#include "platform/switch/SwitchOverlay.h"
#include "platform/switch/SwitchPlatform.h"
#include "util/MemMapper/MemMapper.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cemu/ncrypto/ncrypto.h"

#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>

void PPCTimer_init();
bool SwitchCreateRenderer(int width, int height);

namespace
{
	bool s_switchTripleBuffer = true;
	std::atomic_bool s_gameBootBoostActive{false};
	std::atomic_bool s_gameBootBoostHandoffArmed{false};
	std::atomic<uint64_t> s_gameBootBoostDeadlineTick{0};
	std::atomic_int s_surfaceWidth{1280};
	std::atomic_int s_surfaceHeight{720};
	std::atomic_bool s_exitRequested{false};
	std::atomic_bool s_returnToLauncher{true};

	constexpr uint64_t kGameBootBoostHandoffTimeoutNs = 300'000'000'000ULL;

	void RestoreNormalCpuBoostMode()
	{
		s_gameBootBoostHandoffArmed.store(false, std::memory_order_release);
		s_gameBootBoostDeadlineTick.store(0, std::memory_order_release);

		if (!s_gameBootBoostActive.exchange(false, std::memory_order_acq_rel))
			return;

		appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
	}
}

void SwitchPlatform_ApplyWindowSurfaceSize(int width, int height)
{
	if (NWindow* window = nwindowGetDefault())
		nwindowSetDimensions(window, static_cast<u32>(width), static_cast<u32>(height));
}

bool SwitchPlatform_ShouldReturnToLauncher()
{
	return s_returnToLauncher.load(std::memory_order_acquire);
}

bool SwitchPlatform_UseTripleBuffer()
{
	return s_switchTripleBuffer;
}

void SwitchPlatform_BeginGameBootBoost()
{
	s_gameBootBoostHandoffArmed.store(false, std::memory_order_release);
	s_gameBootBoostDeadlineTick.store(0, std::memory_order_release);

	if (s_gameBootBoostActive.exchange(true, std::memory_order_acq_rel))
		return;

	const Result result = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
	if (R_FAILED(result))
	{
		s_gameBootBoostActive.store(false, std::memory_order_release);
		return;
	}
}

void SwitchPlatform_ArmGameBootBoostHandoff()
{
	const uint64_t now = armGetSystemTick();
	if (!s_gameBootBoostActive.load(std::memory_order_acquire))
		return;

	s_gameBootBoostDeadlineTick.store(now + armNsToTicks(kGameBootBoostHandoffTimeoutNs), std::memory_order_release);
	s_gameBootBoostHandoffArmed.store(true, std::memory_order_release);
}

void SwitchPlatform_EndGameBootBoost()
{
	RestoreNormalCpuBoostMode();
}

void SwitchPlatform_NotifyGameFrameSubmitted()
{
	if (s_gameBootBoostHandoffArmed.exchange(false, std::memory_order_acq_rel))
		RestoreNormalCpuBoostMode();
}

void SwitchPlatform_PollGameBootState()
{
	const uint64_t now = armGetSystemTick();
	const uint64_t boostDeadline = s_gameBootBoostDeadlineTick.load(std::memory_order_acquire);
	if (s_gameBootBoostActive.load(std::memory_order_acquire) &&
		s_gameBootBoostHandoffArmed.load(std::memory_order_acquire) && boostDeadline != 0 &&
		static_cast<int64_t>(now - boostDeadline) >= 0)
	{
		RestoreNormalCpuBoostMode();
	}
}

namespace WindowSystem
{
	static WindowInfo s_windowInfo{};
	static NWindow* s_nwindow = nullptr;
	static bool s_cafeInitialized = false;
	static bool s_inputInitialized = false;
	static bool s_titleListInitialized = false;
	static bool s_saveListInitialized = false;
	static AppletHookCookie s_appletHookCookie{};
	static bool s_appletHooked = false;

	static constexpr int kHandheldWidth = 1280;
	static constexpr int kHandheldHeight = 720;
	static constexpr int kDockedWidth = 1920;
	static constexpr int kDockedHeight = 1080;

	static void UpdateOperationMode()
	{
		const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
		s_surfaceWidth.store(docked ? kDockedWidth : kHandheldWidth, std::memory_order_release);
		s_surfaceHeight.store(docked ? kDockedHeight : kHandheldHeight, std::memory_order_release);
	}

	static void AppletHook(AppletHookType hook, void*)
	{
		switch (hook)
		{
		case AppletHookType_OnFocusState:
		case AppletHookType_OnResume:
		{
			const bool active = appletGetFocusState() == AppletFocusState_InFocus;
			s_windowInfo.app_active.store(active, std::memory_order_release);
			SwitchAudioAPI::SetApplicationActive(active);
			break;
		}
		case AppletHookType_OnOperationMode:
			UpdateOperationMode();
			break;
		case AppletHookType_OnExitRequest:
			s_exitRequested.store(true, std::memory_order_release);
			s_returnToLauncher.store(false, std::memory_order_release);
			break;
		default:
			break;
		}
	}

	static void SetupLifecycle()
	{
		UpdateOperationMode();
		appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
		appletHook(&s_appletHookCookie, AppletHook, nullptr);
		s_appletHooked = true;
	}

	static void ShutdownLifecycle()
	{
		if (s_appletHooked)
		{
			appletUnhook(&s_appletHookCookie);
			s_appletHooked = false;
		}
	}

	static void SetupWindowInfo()
	{
		SetupLifecycle();
		s_nwindow = nwindowGetDefault();
		const int width = s_surfaceWidth.load(std::memory_order_acquire);
		const int height = s_surfaceHeight.load(std::memory_order_acquire);
		SwitchPlatform_ApplyWindowSurfaceSize(width, height);

		s_windowInfo.width = width;
		s_windowInfo.height = height;
		s_windowInfo.phys_width = width;
		s_windowInfo.phys_height = height;
		s_windowInfo.dpi_scale = 1.0;
		s_windowInfo.app_active = true;
		s_windowInfo.is_fullscreen = true;
		s_windowInfo.pad_open = false;

		auto& canvas = s_windowInfo.canvas_main;
		canvas.backend = WindowHandleInfo::Backend::ViNN;
		canvas.display = nullptr;
		canvas.surface = s_nwindow;
		s_windowInfo.window_main = canvas;
	}

	static void SetupPaths()
	{
		std::set<fs::path> failedWriteAccess;
		ActiveSettings::SetPaths(
			/*isPortableMode*/ true,
			/*executablePath*/ "sdmc:/switch/Cemu/Cemu.nro",
			/*userDataPath*/   "sdmc:/switch/Cemu",
			/*configPath*/     "sdmc:/switch/Cemu",
			/*cachePath*/      "sdmc:/switch/Cemu/cache",
			/*dataPath*/       "romfs:/",
			failedWriteAccess);
		std::error_code mkdirEc;
		fs::create_directories(_utf8ToPath("sdmc:/switch/Cemu/games"), mkdirEc);
	}

	static void InitializeMlcLayout()
	{
		const fs::path mlc = ActiveSettings::GetMlcPath();
		const fs::path directories[] = {
			mlc / "sys",
			mlc / "usr",
			mlc / "usr/title/00050000",
			mlc / "usr/title/0005000c",
			mlc / "usr/title/0005000e",
			mlc / "usr/save/00050010/1004a000/user/common/db",
			mlc / "usr/save/00050010/1004a100/user/common/db",
			mlc / "usr/save/00050010/1004a200/user/common/db",
			mlc / "sys/title/0005001b/1005c000/content",
		};
		for (const auto& directory : directories)
		{
			std::error_code ec;
			fs::create_directories(directory, ec);
			if (ec)
				throw std::runtime_error("failed to create MLC directory " + _pathToUtf8(directory) + ": " + ec.message());
		}

		const fs::path languagePath = mlc / "sys/title/0005001b/1005c000/content/language.txt";
		std::error_code ec;
		if (!fs::exists(languagePath, ec))
		{
			std::ofstream languageFile(languagePath);
			if (!languageFile.is_open())
				throw std::runtime_error("failed to create " + _pathToUtf8(languagePath));
			const char* languages[] = { "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
			for (const char* language : languages)
				languageFile << '"' << language << "\",\n";
			languageFile.flush();
			if (!languageFile)
				throw std::runtime_error("failed to write " + _pathToUtf8(languagePath));
		}

		const fs::path countryPath = mlc / "sys/title/0005001b/1005c000/content/country.txt";
		ec.clear();
		if (!fs::exists(countryPath, ec))
		{
			std::ofstream countryFile(countryPath);
			if (!countryFile.is_open())
				throw std::runtime_error("failed to create " + _pathToUtf8(countryPath));
			for (size_t index = 0; index < NCrypto::GetCountryCount(); ++index)
			{
				const char* country = NCrypto::GetCountryAsString(index);
				countryFile << (std::string_view(country) == "NN" ? "NULL" : country) << ",\n";
			}
			countryFile.flush();
			if (!countryFile)
				throw std::runtime_error("failed to write " + _pathToUtf8(countryPath));
		}
	}

	static std::map<std::string, std::string> s_handoff;
	static void LoadHandoff()
	{
		fs::path p = ActiveSettings::GetUserDataPath("switch.ini");
		std::ifstream in(p);
		if (!in.is_open())
			return;
		std::string line;
		while (std::getline(in, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			size_t eq = line.find('=');
			if (eq != std::string::npos)
				s_handoff[line.substr(0, eq)] = line.substr(eq + 1);
		}
		in.close();
		std::error_code ec;
		fs::remove(p, ec);
	}
	static std::string Handoff(const char* key, const char* def = "")
	{
		auto it = s_handoff.find(key);
		return it != s_handoff.end() ? it->second : def;
	}

	static void CommonInit()
	{
		AES128_init();
		PPCTimer_init();

		SetupPaths();
		ActiveSettings::Init();

		GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
		GetConfigHandle().Load();
		InitializeMlcLayout();
		if (NetworkConfig::XMLExists())
			n_config.Load();

		LoadHandoff();
		if (int shift = atoi(Handoff("timer_shift", "3").c_str()); shift >= 0 && shift <= 7)
			ActiveSettings::SetTimerShiftFactor((uint8)shift);
		s_switchTripleBuffer = (Handoff("triple_buffer", "1") != "0");
		const std::string cpuMode = Handoff("cpu_mode");
		if (cpuMode == "0")
			ActiveSettings::SetCPUModeOverride(CPUMode::SinglecoreInterpreter);
		else if (cpuMode == "1")
			ActiveSettings::SetCPUModeOverride(CPUMode::SinglecoreRecompiler);
		else if (cpuMode == "3")
			ActiveSettings::SetCPUModeOverride(CPUMode::MulticoreRecompiler);

		LatteOverlay_init();

		GetConfig().audio_api = IAudioAPI::SwitchAudio;
		GetConfig().tv_device = L"default";
		GetConfig().tv_channels = kStereo;
		if (!GetConfig().pad_device.empty())
			GetConfig().pad_device = L"default";
		GetConfig().pad_channels = kStereo;
		GetConfig().portal_device.clear();

		IAudioAPI::InitializeStatic();
		IAudioInputAPI::InitializeStatic();
		GraphicPack2::LoadAll();
		InputManager::instance().load();
		s_inputInitialized = true;
		SwitchInput_Setup();

		CafeSystem::Initialize();
		s_cafeInitialized = true;

		CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
		s_titleListInitialized = true;
		if (GetConfig().game_paths.empty())
			CafeTitleList::AddScanPath(_utf8ToPath("sdmc:/switch/Cemu/games"));
		else
			for (auto& it : GetConfig().game_paths)
				CafeTitleList::AddScanPath(_utf8ToPath(it));
		fs::path mlcPath = ActiveSettings::GetMlcPath();
		if (!mlcPath.empty())
			CafeTitleList::SetMLCPath(mlcPath);
		CafeTitleList::Refresh();

		CafeSaveList::Initialize();
		s_saveListInitialized = true;
		if (!mlcPath.empty())
		{
			CafeSaveList::SetMLCPath(mlcPath);
			CafeSaveList::Refresh();
		}
	}

	static void CommonShutdown()
	{
		SwitchPlatform_EndGameBootBoost();
		SwitchAmiibo_Shutdown();
		if (s_cafeInitialized)
		{
			CafeSystem::Shutdown();
			s_cafeInitialized = false;
		}
		if (s_saveListInitialized)
		{
			CafeSaveList::Shutdown();
			s_saveListInitialized = false;
		}
		if (s_titleListInitialized)
		{
			CafeTitleList::Shutdown();
			s_titleListInitialized = false;
		}
		if (s_inputInitialized)
		{
			InputManager::instance().Shutdown();
			SwitchInput_Shutdown();
			s_inputInitialized = false;
		}
		FileCache::ShutdownAsyncWriter();
		SwitchJit_Shutdown();
		MemMapper::Shutdown();
	}

	static void WaitForTitleScan()
	{
		for (int waitedMs = 0; CafeTitleList::IsScanning() && waitedMs < 30000; waitedMs += 20)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	static bool LaunchPreparedTitle()
	{
		if (!SwitchCreateRenderer(s_surfaceWidth.load(std::memory_order_acquire),
		                          s_surfaceHeight.load(std::memory_order_acquire)))
			throw std::runtime_error("failed to initialize the Vulkan renderer");
		CafeSystem::LaunchForegroundTitle();
		return true;
	}

	static bool BootTitle()
	{
		auto loadFile = LaunchSettings::GetLoadFile();
		if (loadFile.has_value())
		{
			fs::path path = loadFile.value();
			std::string ext = _pathToUtf8(path.extension());
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
			if (ext == ".rpx" || ext == ".elf")
			{
				auto r = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(path);
				if (r == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
				return false;
			}
			TitleInfo launchTitle{path};
			if (launchTitle.IsValid())
			{
				CafeTitleList::AddTitleFromPath(path);
				TitleId baseTitleId;
				if (CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId) &&
					CafeSystem::PrepareForegroundTitle(baseTitleId) == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
			}
			return false;
		}

		if (auto chosen = LaunchSettings::GetLoadTitleID(); chosen.has_value())
		{
			WaitForTitleScan();
			TitleId baseTitleId;
			if (CafeTitleList::FindBaseTitleId(chosen.value(), baseTitleId))
			{
				auto result = CafeSystem::PrepareForegroundTitle(baseTitleId);
				if (result == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
				return false;
			}
			return false;
		}

		if (std::string line = Handoff("game"); !line.empty())
		{
			WaitForTitleScan();
			TitleId baseTitleId = 0;
			bool resolved = false;
			if (line.rfind("id:", 0) == 0)
			{
				TitleId chosenId = 0;
				const std::string_view idText{line.data() + 3, line.size() - 3};
				auto [end, error] = std::from_chars(idText.data(), idText.data() + idText.size(), chosenId, 16);
				if (error == std::errc{} && end == idText.data() + idText.size())
					resolved = CafeTitleList::FindBaseTitleId(chosenId, baseTitleId);
			}
			else
			{
				fs::path gamePath = _utf8ToPath(line);
				TitleInfo ti{gamePath};
				if (ti.IsValid())
				{
					CafeTitleList::AddTitleFromPath(gamePath);
					resolved = CafeTitleList::FindBaseTitleId(ti.GetAppTitleId(), baseTitleId);
				}
			}
			if (resolved)
			{
				auto result = CafeSystem::PrepareForegroundTitle(baseTitleId);
				if (result == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
				return false;
			}
			return false;
		}

		WaitForTitleScan();

		auto titleIds = CafeTitleList::GetAllTitleIds();
		for (auto titleId : titleIds)
		{
			TitleId baseTitleId;
			if (!CafeTitleList::FindBaseTitleId(titleId, baseTitleId))
			{
				continue;
			}
			auto r = CafeSystem::PrepareForegroundTitle(baseTitleId);
			if (r == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
				return LaunchPreparedTitle();
			return false;
		}
		return false;
	}

	static void RunLoop()
	{
		PadState pad;
		padInitializeDefault(&pad);
		appletSetMediaPlaybackState(true);
		for (;;)
		{
			if (!appletMainLoop())
			{
				s_returnToLauncher.store(false, std::memory_order_release);
				break;
			}
			if (s_exitRequested.load(std::memory_order_acquire))
				break;
			SwitchPlatform_PollGameBootState();
			SwitchAmiibo_Update();
			if (SwitchSwkbd_IsAppletActive())
			{
				svcSleepThread(16'000'000ULL);
				continue;
			}
			padUpdate(&pad);
			const u64 down = padGetButtonsDown(&pad);
			const u64 held = padGetButtons(&pad);
			SwitchOverlay_UpdateInput(down, held);
			if (SwitchOverlay_TakeAmiiboScanRequest())
				SwitchAmiibo_StartScan();
			if (SwitchOverlay_TakeExitRequest())
			{
				s_returnToLauncher.store(true, std::memory_order_release);
				break;
			}
			constexpr u64 kExitCombo = HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR;
			if ((held & kExitCombo) == kExitCombo)
			{
				s_returnToLauncher.store(true, std::memory_order_release);
				break;
			}
			svcSleepThread(16'000'000ULL);
		}
		appletSetMediaPlaybackState(false);
	}

	void Create()
	{
		s_exitRequested.store(false, std::memory_order_release);
		s_returnToLauncher.store(true, std::memory_order_release);
		try
		{
			SetupWindowInfo();
			CommonInit();
			SwitchPlatform_BeginGameBootBoost();
			if (!BootTitle())
				throw std::runtime_error("no launchable title was found");
			RunLoop();
		}
		catch (...)
		{
			CommonShutdown();
			ShutdownLifecycle();
			throw;
		}
		CommonShutdown();
		ShutdownLifecycle();
	}

	WindowInfo& GetWindowInfo() { return s_windowInfo; }

	void ShowErrorDialog(std::string_view, std::string_view, std::optional<ErrorCategory>) {}

	void GetWindowSize(int& w, int& h) { w = s_surfaceWidth.load(); h = s_surfaceHeight.load(); }
	void GetPadWindowSize(int& w, int& h) { w = 0; h = 0; }
	void GetWindowPhysSize(int& w, int& h) { w = s_surfaceWidth.load(); h = s_surfaceHeight.load(); }
	void GetPadWindowPhysSize(int& w, int& h) { w = 0; h = 0; }
	double GetWindowDPIScale() { return 1.0; }
	double GetPadDPIScale() { return 1.0; }
	bool IsPadWindowOpen() { return false; }
	bool IsKeyDown(uint32) { return false; }
	bool IsKeyDown(PlatformKeyCodes) { return false; }
	std::string GetKeyCodeName(uint32) { return ""; }
	bool InputConfigWindowHasFocus() { return false; }
	void NotifyGameLoaded() {}
	void NotifyGameExited() {}
	void RefreshGameList() {}
	bool IsFullScreen() { return true; }
	void UpdateWindowTitles(bool, bool, double) {}
	void CaptureInput(const ControllerState&, const ControllerState&) {}
} // namespace WindowSystem
