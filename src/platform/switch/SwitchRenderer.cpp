#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

bool SwitchCreateRenderer(int width, int height)
{
	if (!InitializeGlobalVulkan())
		return false;
	try
	{
		g_renderer = std::make_unique<VulkanRenderer>();
		VulkanRenderer::GetInstance()->InitializeSurface({ width, height }, true);
		return true;
	}
	catch (...)
	{
		return false;
	}
}
