#pragma once

bool SwitchPlatform_UseTripleBuffer();
void SwitchPlatform_BeginGameBootBoost();
void SwitchPlatform_ArmGameBootBoostHandoff();
void SwitchPlatform_EndGameBootBoost();
void SwitchPlatform_NotifyGameFrameSubmitted();
void SwitchPlatform_PollGameBootState();
void SwitchPlatform_ApplyWindowSurfaceSize(int width, int height);
bool SwitchPlatform_ShouldReturnToLauncher();
