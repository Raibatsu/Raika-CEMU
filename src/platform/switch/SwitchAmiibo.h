#pragma once

#include <cstddef>
#include <cstdint>

bool SwitchAmiibo_StartScan();
void SwitchAmiibo_Update();
void SwitchAmiibo_Shutdown();
bool SwitchAmiibo_WriteApplicationArea(std::uint32_t accessId, const void* data, std::size_t size);
bool SwitchAmiibo_DeleteApplicationArea();
void SwitchAmiibo_GetStatus(char* output, std::size_t size);
