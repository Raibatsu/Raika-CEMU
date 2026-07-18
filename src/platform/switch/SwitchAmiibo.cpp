#include "platform/switch/SwitchAmiibo.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "Cafe/OS/libs/nn_nfp/nn_nfp.h"

namespace
{
enum class ScanState
{
	Idle,
	Scanning,
	Present,
};

constexpr std::size_t kMaxDevices = 8;
constexpr std::uint64_t kScanTimeoutNs = 15'000'000'000ULL;

std::mutex s_mutex;
ScanState s_state = ScanState::Idle;
bool s_serviceInitialized = false;
bool s_mounted = false;
std::array<NfcDeviceHandle, kMaxDevices> s_devices{};
int s_deviceCount = 0;
int s_activeDevice = -1;
std::uint64_t s_deadline = 0;
NfpHostTag s_tag{};
char s_status[96] = "Ready";

void SetStatus(const char* status)
{
	std::snprintf(s_status, sizeof(s_status), "%s", status);
}

void SetResultStatus(const char* operation, Result result)
{
	std::snprintf(s_status, sizeof(s_status), "%s failed (0x%08x)", operation,
	              static_cast<unsigned int>(result));
}

void CloseService()
{
	if (!s_serviceInitialized)
		return;

	if (s_mounted && s_activeDevice >= 0)
		nfpUnmount(&s_devices[s_activeDevice]);
	for (int index = 0; index < s_deviceCount; ++index)
		nfpStopDetection(&s_devices[index]);
	nfpExit();

	s_serviceInitialized = false;
	s_mounted = false;
	s_deviceCount = 0;
	s_activeDevice = -1;
	s_deadline = 0;
	s_state = ScanState::Idle;
}

bool ReadTag(int deviceIndex, NfpHostTag& tag)
{
	const NfcDeviceHandle& device = s_devices[deviceIndex];
	Result result = nfpMount(&device, NfpDeviceType_Amiibo, NfpMountTarget_All);
	if (R_FAILED(result))
	{
		SetResultStatus("Mount", result);
		return false;
	}
	s_mounted = true;
	s_activeDevice = deviceIndex;

	NfpData data{};
	NfpTagInfo tagInfo{};
	NfpModelInfo modelInfo{};
	result = nfpGetAll(&device, &data);
	if (R_SUCCEEDED(result))
		result = nfpGetTagInfo(&device, &tagInfo);
	if (R_SUCCEEDED(result))
		result = nfpGetModelInfo(&device, &modelInfo);
	if (R_FAILED(result))
	{
		SetResultStatus("Read", result);
		nfpUnmount(&device);
		s_mounted = false;
		s_activeDevice = -1;
		return false;
	}

	tag = {};
	tag.uidLength = std::min<std::uint8_t>(tagInfo.uid.uid_length, sizeof(tag.uid));
	std::memcpy(tag.uid, tagInfo.uid.uid, tag.uidLength);
	std::memcpy(tag.characterId, modelInfo.character_id, sizeof(tag.characterId));
	tag.seriesId = modelInfo.series_id;
	tag.modelNumber = modelInfo.numbering_id;
	tag.figureType = modelInfo.nfp_type;
	tag.tagWriteCounter = data.tag_write_counter;
	tag.writeCounter = data.write_counter;
	tag.version = data.version;
	tag.applicationAreaSize = std::min<std::uint32_t>(data.application_area_size,
	                                                     sizeof(tag.applicationArea));
	tag.applicationId = data.application_id;
	tag.accessId = data.access_id;
	tag.flags = data.flags;
	std::memcpy(tag.mii, &data.mii_v3, std::min(sizeof(tag.mii), sizeof(data.mii_v3)));
	std::memcpy(tag.nickname, data.amiibo_name,
	            std::min(sizeof(tag.nickname), sizeof(data.amiibo_name)));
	std::memcpy(tag.applicationArea, data.application_area, sizeof(tag.applicationArea));
	return true;
}
} // namespace

bool SwitchAmiibo_StartScan()
{
	bool removePrevious = false;
	bool started = false;
	{
		std::lock_guard lock(s_mutex);
		removePrevious = s_state == ScanState::Present;
		CloseService();

		Result result = nfpInitialize(NfpServiceType_Debug);
		if (R_FAILED(result))
		{
			SetResultStatus("NFC service", result);
		}
		else
		{
			s_serviceInitialized = true;
			s32 deviceCount = 0;
			result = nfpListDevices(&deviceCount, s_devices.data(), static_cast<s32>(s_devices.size()));
			if (R_FAILED(result) || deviceCount <= 0)
			{
				if (R_FAILED(result))
					SetResultStatus("Controller scan", result);
				else
					SetStatus("No NFC-capable controller found");
				CloseService();
			}
			else
			{
				s_deviceCount = std::min<int>(deviceCount, static_cast<int>(s_devices.size()));
				int activeDetectors = 0;
				for (int index = 0; index < s_deviceCount; ++index)
				{
					if (R_SUCCEEDED(nfpStartDetection(&s_devices[index])))
						++activeDetectors;
				}
				if (activeDetectors == 0)
				{
					SetStatus("NFC is unavailable on connected controllers");
					CloseService();
				}
				else
				{
					s_state = ScanState::Scanning;
					s_deadline = armGetSystemTick() + armNsToTicks(kScanTimeoutNs);
					SetStatus("Hold an Amiibo on the right Joy-Con");
					started = true;
				}
			}
		}
	}

	if (removePrevious)
		nnNfp_removeHostTag();
	return started;
}

void SwitchAmiibo_Update()
{
	enum class Action
	{
		None,
		Touch,
		Refresh,
		Remove,
	};

	Action action = Action::None;
	NfpHostTag tag{};
	{
		std::lock_guard lock(s_mutex);
		if (s_state == ScanState::Scanning)
		{
			for (int index = 0; index < s_deviceCount; ++index)
			{
				NfpDeviceState state{};
				if (R_SUCCEEDED(nfpGetDeviceState(&s_devices[index], &state)) &&
				    state == NfpDeviceState_TagFound && ReadTag(index, tag))
				{
					s_tag = tag;
					s_state = ScanState::Present;
					SetStatus("Amiibo connected");
					for (int other = 0; other < s_deviceCount; ++other)
					{
						if (other != index)
							nfpStopDetection(&s_devices[other]);
					}
					action = Action::Touch;
					break;
				}
			}

			if (s_state == ScanState::Scanning &&
			    static_cast<std::int64_t>(armGetSystemTick() - s_deadline) >= 0)
			{
				CloseService();
				SetStatus("Amiibo scan timed out");
			}
		}
		else if (s_state == ScanState::Present)
		{
			NfpDeviceState state{};
			if (s_activeDevice < 0 ||
			    R_FAILED(nfpGetDeviceState(&s_devices[s_activeDevice], &state)) ||
			    state == NfpDeviceState_TagRemoved || state == NfpDeviceState_Unavailable ||
			    state == NfpDeviceState_Initialized)
			{
				CloseService();
				SetStatus("Amiibo removed");
				action = Action::Remove;
			}
			else
			{
				tag = s_tag;
				action = Action::Refresh;
			}
		}
	}

	if (action == Action::Touch)
	{
		std::uint32_t error = 0;
		if (!nnNfp_touchNfcTagFromHost(tag, &error))
		{
			std::lock_guard lock(s_mutex);
			SetStatus("Cemu rejected the Amiibo");
		}
	}
	else if (action == Action::Refresh)
	{
		if (nnNfp_isHostTagPresent())
			nnNfp_refreshHostTag();
		else if (nnNfp_isInitialized())
		{
			std::uint32_t error = 0;
			nnNfp_touchNfcTagFromHost(tag, &error);
		}
	}
	else if (action == Action::Remove)
	{
		nnNfp_removeHostTag();
	}
}

void SwitchAmiibo_Shutdown()
{
	bool removeTag = false;
	{
		std::lock_guard lock(s_mutex);
		removeTag = s_state == ScanState::Present;
		CloseService();
		SetStatus("Ready");
	}
	if (removeTag)
		nnNfp_removeHostTag();
}

bool SwitchAmiibo_WriteApplicationArea(std::uint32_t accessId, const void* data, std::size_t size)
{
	if (!data || size == 0)
		return false;

	std::lock_guard lock(s_mutex);
	if (s_state != ScanState::Present || !s_serviceInitialized || !s_mounted || s_activeDevice < 0)
		return false;

	const NfcDeviceHandle& device = s_devices[s_activeDevice];
	const std::size_t writeSize = std::min<std::size_t>(size, sizeof(s_tag.applicationArea));
	Result result = nfpOpenApplicationArea(&device, accessId);
	if (R_SUCCEEDED(result))
	{
		result = nfpSetApplicationArea(&device, data, writeSize);
	}
	else
	{
		bool exists = false;
		if (R_SUCCEEDED(nfpExistsApplicationArea(&device, &exists)) && exists)
			result = nfpRecreateApplicationArea(&device, accessId, data, writeSize);
		else
			result = nfpCreateApplicationArea(&device, accessId, data, writeSize);
	}

	if (R_SUCCEEDED(result))
		result = nfpFlush(&device);
	if (R_FAILED(result))
	{
		SetResultStatus("Amiibo write", result);
		return false;
	}

	s_tag.accessId = accessId;
	s_tag.applicationAreaSize = static_cast<std::uint32_t>(writeSize);
	s_tag.flags |= NfpAmiiboFlag_ApplicationAreaExists;
	std::memcpy(s_tag.applicationArea, data, writeSize);
	SetStatus("Amiibo saved");
	return true;
}

bool SwitchAmiibo_DeleteApplicationArea()
{
	std::lock_guard lock(s_mutex);
	if (s_state != ScanState::Present || !s_serviceInitialized || !s_mounted || s_activeDevice < 0)
		return false;

	const NfcDeviceHandle& device = s_devices[s_activeDevice];
	bool exists = false;
	Result result = nfpExistsApplicationArea(&device, &exists);
	if (R_SUCCEEDED(result) && exists)
	{
		result = nfpDeleteApplicationArea(&device);
		if (R_SUCCEEDED(result))
			result = nfpFlush(&device);
	}
	if (R_FAILED(result))
	{
		SetResultStatus("Amiibo delete", result);
		return false;
	}

	s_tag.accessId = 0;
	s_tag.applicationAreaSize = 0;
	s_tag.flags &= ~NfpAmiiboFlag_ApplicationAreaExists;
	std::memset(s_tag.applicationArea, 0, sizeof(s_tag.applicationArea));
	SetStatus("Amiibo saved");
	return true;
}

void SwitchAmiibo_GetStatus(char* output, std::size_t size)
{
	if (!output || size == 0)
		return;
	std::lock_guard lock(s_mutex);
	std::snprintf(output, size, "%s", s_status);
}
