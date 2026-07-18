#pragma once

#include <cstddef>

size_t SwitchMemoryBudget_ComputeGuestPoolSize(size_t heapTotal);
size_t SwitchMemoryBudget_GetJitArenaSize();
size_t SwitchMemoryBudget_GetBufferCacheSize();
size_t SwitchMemoryBudget_GetJumpTableChunkSize();

bool SwitchMemoryBudget_ShouldRecycleStaging(size_t currentSize, size_t growthSize);
size_t SwitchMemoryBudget_SelectTextureChunkSize(size_t minimumSize, size_t preferredSize);
