#pragma once

enum class SwitchControllerType
{
	GamePad,
	Pro,
	Classic,
};

void SwitchInput_Setup();
void SwitchInput_Shutdown();
bool SwitchInput_SetControllerType(SwitchControllerType type);
SwitchControllerType SwitchInput_GetControllerType();
