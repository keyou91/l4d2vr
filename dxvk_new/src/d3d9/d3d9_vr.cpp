#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"

#include "d3d9_device.h"

#include "L4D2VR/game.h"
#include "L4D2VR/vr.h"

#include <cstring>

namespace dxvk {

  class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {

  public:

    D3D9VR(IDirect3DDevice9* pDevice)
      : m_device(static_cast<D3D9DeviceEx*>(pDevice)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid,
            void** ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDirect3DVR9)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9*   pSurface,
            D3D9_TEXTURE_VR_DESC* pDesc) {
      if (unlikely(pSurface == nullptr || pDesc == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9Surface* surface = static_cast<D3D9Surface*>(pSurface);

      const auto* tex = surface->GetCommonTexture();

      const auto& desc = tex->Desc();
      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr || image->info().sampleCount != VK_SAMPLE_COUNT_1_BIT)) {
        Logger::warn("D3D9VR::GetVRDesc: refusing non-submit-ready multisampled texture");
        return D3DERR_INVALIDCALL;
      }

      const auto& device = tex->Device()->GetDXVKDevice();

      // I don't know why the image randomly is a uint64_t in OpenVR.
      pDesc->Image = uint64_t(image->handle());
      pDesc->Device = device->handle();
      pDesc->PhysicalDevice = device->adapter()->handle();
      pDesc->Instance = device->instance()->handle();
      pDesc->Queue = device->queues().graphics.queueHandle;
      pDesc->QueueFamilyIndex = device->queues().graphics.queueIndex;

      pDesc->Width = desc->Width;
      pDesc->Height = desc->Height;
      pDesc->Format = tex->GetFormatMapping().FormatColor;
      pDesc->SampleCount = uint32_t(image->info().sampleCount);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9* pSurface,
            BOOL               waitResourceIdle) {
      if (unlikely(pSurface == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      auto* tex = static_cast<D3D9Surface*>(pSurface)->GetCommonTexture();
      const auto& image = tex->GetImage();

      VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, image->info().mipLevels,
        0, image->info().numLayers
      };

      m_device->TransformImage(
        tex, &subresources,
        image->info().layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

      // This wait may need to be on all Faces and Mip Levels (2 loops).
      if (waitResourceIdle)
        m_device->WaitForResource(*image, tex->GetMappingBufferSequenceNumber(0u), D3DLOCK_READONLY);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockDevice() {
      m_lock = m_device->LockDeviceExclusive();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice() {
      m_lock = D3D9DeviceLock();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockSubmissionQueue() {
      // Drain DXVK's CPU command stream while no other D3D9 call can append to it,
      // then take the queue's native external-synchronization gate. Once acquired,
      // the device lock can be released: Source may record the next frame, but DXVK's
      // submit thread cannot touch VkQueue until OpenVR has submitted the stereo pair.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->lockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockPreparedSubmissionQueue() {
      // The producer has already flushed and synchronized the D3D9 command stream
      // before publishing this texture generation. Only serialize native VkQueue
      // access here; doing another CS-thread drain on every VR submit defeats queued
      // rendering and puts the render-thread backlog on the Present thread.
      m_device->GetDXVKDevice()->lockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawQueuedEyeSubmitOverlays(VR* vr) {
      if (unlikely(vr == nullptr))
        return D3DERR_INVALIDCALL;

      // The caller owns the VR texture mutex and the recursive D3D9 device lock.
      // Keep these draws in the same copy -> decorate -> transition transaction.
      m_device->DrawQueuedEyeSubmitOverlays(vr);
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
      m_device->GetDXVKDevice()->unlockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitDeviceIdle() {
      // This can be called from the VR/Present path while Source's queued material
      // thread is active. DXVK's D3D9 command chunk is device-owned mutable state, so
      // flushing/synchronizing without the same device lock used by draw calls can race
      // DrawIndexedPrimitive and leave m_csChunk null/moved.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      m_device->Flush();
      // Not clear if we need all here, perhaps...
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->waitForIdle();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData) {
      if (unlikely(backBufferData == nullptr))
        return D3DERR_INVALIDCALL;

      IDirect3DSurface9* backBufferSurface = nullptr;

      HRESULT res = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBufferSurface);
      if (FAILED(res) || backBufferSurface == nullptr)
        return FAILED(res) ? res : D3DERR_INVALIDCALL;

      D3D9_TEXTURE_VR_DESC textureDesc = {};

      res = GetVRDesc(backBufferSurface, &textureDesc);

      if (backBufferSurface != nullptr)
        backBufferSurface->Release();

      if (FAILED(res))
        return res;

      std::memcpy(&backBufferData->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
      backBufferData->m_VRTexture.handle = &backBufferData->m_VulkanData;
      backBufferData->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
      backBufferData->m_VRTexture.eType = vr::TextureType_Vulkan;

      return res;
    }

  private:

    D3D9DeviceEx* m_device;
    D3D9DeviceLock m_lock;

  };

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* pDevice, IDirect3DVR9** pInterface) {
  if (pDevice == nullptr || pInterface == nullptr)
    return D3DERR_INVALIDCALL;

  *pInterface = new dxvk::D3D9VR(pDevice);

  return D3D_OK;
}
