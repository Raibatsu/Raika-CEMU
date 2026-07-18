#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nn::nfp
{
	uint32 NFCGetTagInfo(uint32 index, uint32 timeout, MPTR functionPtr, void* userParam);

	COSModule* GetModule();
}

void nnNfp_load();
void nnNfp_update();

bool nnNfp_isInitialized();
bool nnNfp_touchNfcTagFromFile(const fs::path& filePath, uint32* nfcError);

struct NfpHostTag
{
	uint8 uidLength{};
	uint8 uid[10]{};
	uint8 characterId[3]{};
	uint8 seriesId{};
	uint16 modelNumber{};
	uint8 figureType{};
	uint16 tagWriteCounter{};
	uint16 writeCounter{};
	uint16 version{};
	uint32 applicationAreaSize{};
	uint64 applicationId{};
	uint32 accessId{};
	uint8 flags{};
	uint8 mii[0x5C]{};
	uint16 nickname[11]{};
	uint8 applicationArea[0xD8]{};
};

bool nnNfp_touchNfcTagFromHost(const NfpHostTag& tag, uint32* nfcError);
bool nnNfp_isHostTagPresent();
void nnNfp_refreshHostTag();
void nnNfp_removeHostTag();

#define NFP_STATE_NONE			(0)
#define NFP_STATE_INIT			(1)
#define NFP_STATE_RW_SEARCH		(2)
#define NFP_STATE_RW_ACTIVE		(3)
#define NFP_STATE_RW_DEACTIVE	(4)
#define NFP_STATE_RW_MOUNT		(5)
#define NFP_STATE_UNEXPECTED	(6)
#define NFP_STATE_RW_MOUNT_ROM	(7)
