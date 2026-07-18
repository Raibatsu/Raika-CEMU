#pragma once
#include <cstddef>

bool SwitchJit_SyscallsAvailable();
bool SwitchJit_InitCodeArena(size_t size);
void* SwitchJit_AllocRw(size_t size);
bool SwitchJit_FreeRw(void* address);
void SwitchJit_FlushCode(void* rwAddr, size_t size);
void* SwitchJit_RwToRx(void* rw);
void SwitchJit_PinThreadToCore(int coreIndex);
