#pragma once

enum class SwitchGamePadLayout
{
	Disabled,
	PadOnly,
	Right,
	Left,
	Below,
	Above,
};

bool SwitchPlatform_UseTripleBuffer();
void SwitchPlatform_BeginGameBootBoost();
void SwitchPlatform_ArmGameBootBoostHandoff();
void SwitchPlatform_EndGameBootBoost();
void SwitchPlatform_NotifyGameFrameSubmitted();
void SwitchPlatform_PollGameBootState();
void SwitchPlatform_ApplyWindowSurfaceSize(int width, int height);
bool SwitchPlatform_ShouldReturnToLauncher();
void SwitchPlatform_SetGamePadLayout(SwitchGamePadLayout layout);
SwitchGamePadLayout SwitchPlatform_GetGamePadLayout();
void SwitchPlatform_CycleGamePadMode();
bool SwitchPlatform_TakeGamePadLayoutChange();
bool SwitchPlatform_IsGamePadOutputActive();
bool SwitchPlatform_IsGamePadOnly();
bool SwitchPlatform_IsGamePadCompositeActive();
void SwitchPlatform_GetGamePadViewRegion(bool padView, int fullWidth, int fullHeight,
	int& x, int& y, int& width, int& height);
void SwitchPlatform_GetGamePadImageRect(bool padView, int fullWidth, int fullHeight,
	int sourceWidth, int sourceHeight, int& x, int& y, int& width, int& height);
