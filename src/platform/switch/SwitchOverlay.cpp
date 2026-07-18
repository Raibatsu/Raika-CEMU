#include "platform/switch/SwitchOverlay.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

#include <imgui.h>

#include "config/CemuConfig.h"
#include "imgui/imgui_extension.h"
#include "platform/switch/SwitchAmiibo.h"
#include "platform/switch/SwitchInput.h"

namespace
{
constexpr std::uint64_t kOpenButtons = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
constexpr int kMenuItems = 7;
constexpr int kPositionCount = 7;

std::atomic_bool s_visible{false};
std::atomic_bool s_captureUntilRelease{false};
std::atomic_int s_selection{0};
std::atomic_int s_fpsToggleRequests{0};
std::atomic_int s_positionRequests{0};
std::atomic_int s_shaderToggleRequests{0};
std::atomic_int s_amiiboScanRequests{0};
std::atomic_int s_exitRequests{0};

const char* ControllerTypeName(SwitchControllerType type)
{
	switch (type)
	{
	case SwitchControllerType::GamePad:
		return "Wii U GamePad";
	case SwitchControllerType::Pro:
		return "Wii U Pro Controller";
	case SwitchControllerType::Classic:
		return "Wii Classic Controller";
	}
	return "Wii U GamePad";
}

SwitchControllerType NextControllerType(int direction)
{
	int type = static_cast<int>(SwitchInput_GetControllerType());
	type = (type + direction + 3) % 3;
	return static_cast<SwitchControllerType>(type);
}

void SetControllerType(int direction)
{
	SwitchInput_SetControllerType(NextControllerType(direction));
}

const char* PositionName(ScreenPosition position)
{
	switch (position)
	{
	case ScreenPosition::kDisabled:
		return "Disabled";
	case ScreenPosition::kTopLeft:
		return "Top left";
	case ScreenPosition::kTopCenter:
		return "Top center";
	case ScreenPosition::kTopRight:
		return "Top right";
	case ScreenPosition::kBottomLeft:
		return "Bottom left";
	case ScreenPosition::kBottomCenter:
		return "Bottom center";
	case ScreenPosition::kBottomRight:
		return "Bottom right";
	}
	return "Disabled";
}

void ApplyOverlaySettings()
{
	auto& config = GetConfig();
	if (s_fpsToggleRequests.exchange(0, std::memory_order_acq_rel) & 1)
	{
		config.overlay.fps = !config.overlay.fps;
		if (config.overlay.fps && config.overlay.position == ScreenPosition::kDisabled)
			config.overlay.position = ScreenPosition::kTopLeft;
	}

	const int positionDelta = s_positionRequests.exchange(0, std::memory_order_acq_rel);
	if (positionDelta != 0)
	{
		const int position = (static_cast<int>(config.overlay.position) +
		                      positionDelta % kPositionCount + kPositionCount) % kPositionCount;
		config.overlay.position = static_cast<ScreenPosition>(position);
	}

	if (s_shaderToggleRequests.exchange(0, std::memory_order_acq_rel) & 1)
		config.notification.shader_compiling = !config.notification.shader_compiling;
}

void CenteredText(const char* text)
{
	const float width = ImGui::CalcTextSize(text).x;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - width) * 0.5f));
	ImGui::TextUnformatted(text);
}
} // namespace

void SwitchOverlay_UpdateInput(std::uint64_t down, std::uint64_t held)
{
	if (s_captureUntilRelease.load(std::memory_order_relaxed) && held == 0)
		s_captureUntilRelease.store(false, std::memory_order_release);

	if ((held & kOpenButtons) == kOpenButtons && (down & kOpenButtons))
	{
		const bool visible = !s_visible.load(std::memory_order_relaxed);
		s_visible.store(visible, std::memory_order_release);
		if (visible)
			s_selection.store(0, std::memory_order_relaxed);
		else
			s_captureUntilRelease.store(true, std::memory_order_release);
		return;
	}

	if (!s_visible.load(std::memory_order_acquire))
		return;

	int selection = s_selection.load(std::memory_order_relaxed);
	if (down & HidNpadButton_Up)
		selection = (selection + kMenuItems - 1) % kMenuItems;
	else if (down & HidNpadButton_Down)
		selection = (selection + 1) % kMenuItems;
	s_selection.store(selection, std::memory_order_relaxed);

	if (down & HidNpadButton_B)
	{
		s_visible.store(false, std::memory_order_release);
		s_captureUntilRelease.store(true, std::memory_order_release);
		return;
	}

	const bool previous = down & HidNpadButton_Left;
	const bool next = down & (HidNpadButton_Right | HidNpadButton_A);
	if (selection == 0 && previous)
		SetControllerType(-1);
	else if (selection == 0 && next)
		SetControllerType(1);
	else if (selection == 1 && (previous || next))
		s_fpsToggleRequests.fetch_add(1, std::memory_order_release);
	else if (selection == 2 && previous)
		s_positionRequests.fetch_sub(1, std::memory_order_release);
	else if (selection == 2 && next)
		s_positionRequests.fetch_add(1, std::memory_order_release);
	else if (selection == 3 && (previous || next))
		s_shaderToggleRequests.fetch_add(1, std::memory_order_release);
	else if (selection == 4 && (down & HidNpadButton_A))
		s_amiiboScanRequests.fetch_add(1, std::memory_order_release);
	else if (selection == 5 && (down & HidNpadButton_A))
	{
		s_exitRequests.fetch_add(1, std::memory_order_release);
		s_visible.store(false, std::memory_order_release);
		s_captureUntilRelease.store(true, std::memory_order_release);
	}
	else if (selection == 6 && (down & HidNpadButton_A))
	{
		s_visible.store(false, std::memory_order_release);
		s_captureUntilRelease.store(true, std::memory_order_release);
	}
}

bool SwitchOverlay_TakeAmiiboScanRequest()
{
	return s_amiiboScanRequests.exchange(0, std::memory_order_acq_rel) != 0;
}

bool SwitchOverlay_TakeExitRequest()
{
	return s_exitRequests.exchange(0, std::memory_order_acq_rel) != 0;
}

bool SwitchOverlay_IsInputCaptured(std::uint64_t held)
{
	return s_visible.load(std::memory_order_acquire) ||
		s_captureUntilRelease.load(std::memory_order_acquire) ||
		(held & kOpenButtons) == kOpenButtons;
}

void SwitchOverlay_Render()
{
	ApplyOverlaySettings();
	if (!s_visible.load(std::memory_order_acquire))
		return;

	ImFont* font = ImGui_GetFont(24.0f);
	if (!font)
		return;

	const ImGuiIO& io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(0, 0, 0, 120));
	ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowSize({720.0f, 610.0f}, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f, 22.0f});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f, 12.0f});
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 20, 34, 245));
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(94, 201, 255, 255));
	ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(31, 82, 112, 235));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(31, 82, 112, 235));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(31, 82, 112, 235));
	ImGui::PushFont(font);

	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs;
	if (ImGui::Begin("Cemu quick menu", nullptr, flags))
	{
		CenteredText("Cemu Quick Menu");
		ImGui::Separator();

		char controllerRow[128];
		std::snprintf(controllerRow, sizeof(controllerRow), "Controller type     <  %s  >",
		              ControllerTypeName(SwitchInput_GetControllerType()));
		auto& config = GetConfig();
		char fpsRow[96];
		std::snprintf(fpsRow, sizeof(fpsRow), "FPS counter         <  %s  >", config.overlay.fps ? "On" : "Off");
		char positionRow[128];
		std::snprintf(positionRow, sizeof(positionRow), "Overlay position    <  %s  >",
		              PositionName(config.overlay.position));
		char shaderRow[128];
		std::snprintf(shaderRow, sizeof(shaderRow), "Shader notice       <  %s  >",
		              config.notification.shader_compiling ? "On" : "Off");
		char amiiboStatus[96];
		SwitchAmiibo_GetStatus(amiiboStatus, sizeof(amiiboStatus));
		char amiiboRow[160];
		std::snprintf(amiiboRow, sizeof(amiiboRow), "Scan Amiibo         %s", amiiboStatus);
		const int selection = s_selection.load(std::memory_order_relaxed);
		ImGui::Selectable(controllerRow, selection == 0, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Selectable(fpsRow, selection == 1, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Selectable(positionRow, selection == 2, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Selectable(shaderRow, selection == 3, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Selectable(amiiboRow, selection == 4, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Selectable("Exit to launcher", selection == 5, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Selectable("Close quick menu", selection == 6, ImGuiSelectableFlags_None, {0.0f, 52.0f});
		ImGui::Separator();
		CenteredText("D-Pad  Navigate     A  Select     B  Close");
	}
	ImGui::End();

	ImGui::PopFont();
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(3);
}
