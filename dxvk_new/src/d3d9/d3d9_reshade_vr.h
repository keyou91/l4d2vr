#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace dxvk {

  void D3D9ReShadeVrPrepareConfiguration();
  bool D3D9ReShadeVrInitialize();
  void D3D9ReShadeVrShutdown();
  bool D3D9ReShadeVrIsReady();
  void D3D9ReShadeVrInvalidateBinding();
  void D3D9ReShadeVrPublishDepth(
          VkDevice        device,
          VkImageView     depthView,
          uint32_t        width,
          uint32_t        height);

  void D3D9ReShadeVrClearDepth(
          VkDevice        device);

}

extern "C" bool __cdecl L4D2VR_InitializeReShadeVRBridge();
extern "C" void __cdecl L4D2VR_ShutdownReShadeVRBridge();
