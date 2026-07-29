#pragma once

#include <cstddef>
#include <string>

enum class SwitchToyType
{
	Skylanders,
	Infinity,
	Dimensions,
};

void SwitchToys_Initialize();
bool SwitchToys_IsEnabled(SwitchToyType type);
int SwitchToys_GetSlotCount(SwitchToyType type);
std::string SwitchToys_GetSlotTitle(SwitchToyType type, int slot);
std::string SwitchToys_GetSlotValue(SwitchToyType type, int slot);
void SwitchToys_RefreshFiles(SwitchToyType type);
int SwitchToys_GetFileCount(SwitchToyType type);
std::string SwitchToys_GetFileName(SwitchToyType type, int index);
bool SwitchToys_LoadFile(SwitchToyType type, int slot, int fileIndex);
bool SwitchToys_ClearSlot(SwitchToyType type, int slot);
std::string SwitchToys_GetStatus();
