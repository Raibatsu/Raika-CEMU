#pragma once

extern "C" {

bool SwitchSwkbd_Show(const char16_t* initialU16, int maxLen, char16_t* outU16, int outCap);

bool SwitchSwkbd_IsAppletActive(void);
void SwitchSwkbd_NotifyGpuIdle(void);

}
