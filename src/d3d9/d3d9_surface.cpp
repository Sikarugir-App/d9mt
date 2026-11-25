
#include "d3d9_surface.hpp"
#include "d3d9_device.hpp"
#include "wsi_platform.hpp"

namespace dxmt {

D3D9Surface::D3D9Surface(D3D9Device *device, Rc<Texture> texture, TextureViewKey viewKey,
                         WMTPixelFormat mtlFormat)
    : device_(device), texture_(std::move(texture)), viewKey_(viewKey),
      mtl_format_(mtlFormat != WMTPixelFormatInvalid ? mtlFormat : this->texture_->pixelFormat()),
      pool_(D3DPOOL_DEFAULT), format_(D3DFMT_X8R8G8B8) {}

D3D9Surface::D3D9Surface(D3D9Device *device, UINT width, UINT height, D3DFORMAT format, D3DPOOL pool)
    : device_(device), pool_(pool), format_(format), width_(width), height_(height) {
  pitch_ = width * 4; // BGRA8 = 4 bytes per pixel
  sys_mem_ = std::calloc(height, pitch_);
}

D3D9Surface::~D3D9Surface() {
  if (sys_mem_) {
    std::free(sys_mem_);
    sys_mem_ = nullptr;
  }
  if (readback_mem_) {
    wsi::aligned_free(readback_mem_);
    readback_mem_ = nullptr;
  }
}

HRESULT STDMETHODCALLTYPE D3D9Surface::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj)
    return E_POINTER;

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DSurface9) ||
      riid == __uuidof(IDirect3DResource9)) {
    *ppvObj = ref(this);
    return S_OK;
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::GetDesc(D3DSURFACE_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;

  if (pool_ == D3DPOOL_SYSTEMMEM) {
    pDesc->Format = format_;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = 0;
    pDesc->Pool = D3DPOOL_SYSTEMMEM;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = width_;
    pDesc->Height = height_;
  } else {
    pDesc->Format = format_;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = gpu_usage_;
    pDesc->Pool = D3DPOOL_DEFAULT;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = texture_->width();
    pDesc->Height = texture_->height();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  if (!pLockedRect)
    return D3DERR_INVALIDCALL;
  if (locked_)
    return D3DERR_INVALIDCALL;

  // GPU-backed surface: do a synchronous readback
  if (texture_ && pool_ == D3DPOOL_DEFAULT) {
    device_->FlushDrawBatch();

    UINT width = texture_->width();
    UINT height = texture_->height();
    UINT bytesPerRow = width * 4;
    UINT totalBytes = bytesPerRow * height;
    UINT alignedSize = (totalBytes + 0xFFF) & ~0xFFFu;

    if (!readback_mem_) {
      readback_mem_ = wsi::aligned_malloc(alignedSize, DXMT_PAGE_SIZE);
      if (!readback_mem_) return D3DERR_INVALIDCALL;
      readback_pitch_ = bytesPerRow;
    }

    void *placedMem = readback_mem_;

    WMTBufferInfo bufInfo = {};
    bufInfo.length = alignedSize;
    bufInfo.options = WMTResourceStorageModeShared;
    bufInfo.memory.set(placedMem);
    auto readbackBuf = device_->GetMTLDevice().newBuffer(bufInfo);

    auto &queue = device_->GetQueue();
    auto chunk = queue.CurrentChunk();

    chunk->emitcc([
      srcTex = texture_,
      readbackBufHandle = readbackBuf.handle,
      width, height, bytesPerRow, totalBytes
    ](ArgumentEncodingContext &ctx) mutable {
      ctx.startBlitPass();

      auto srcHandle = ctx.access(srcTex, 0u, 0u, DXMT_ENCODER_RESOURCE_ACESS_READ);
      auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
      blitCmd.type = WMTBlitCommandCopyFromTextureToBuffer;
      blitCmd.src = srcHandle;
      blitCmd.slice = 0;
      blitCmd.level = 0;
      blitCmd.origin = {0, 0, 0};
      blitCmd.size = {width, height, 1};
      blitCmd.dst = readbackBufHandle;
      blitCmd.offset = 0;
      blitCmd.bytes_per_row = bytesPerRow;
      blitCmd.bytes_per_image = totalBytes;

      ctx.endPass();
    });

    queue.CommitCurrentChunk();
    queue.WaitForIdle();

    locked_ = true;

    if (pRect) {
      pLockedRect->pBits = static_cast<uint8_t *>(readback_mem_) + pRect->top * readback_pitch_ + pRect->left * 4;
    } else {
      pLockedRect->pBits = readback_mem_;
    }
    pLockedRect->Pitch = readback_pitch_;
    return S_OK;
  }

  // System memory surface
  if (pool_ != D3DPOOL_SYSTEMMEM || !sys_mem_)
    return D3DERR_INVALIDCALL;

  locked_ = true;

  if (pRect) {
    pLockedRect->pBits = static_cast<uint8_t *>(sys_mem_) + pRect->top * pitch_ + pRect->left * 4;
  } else {
    pLockedRect->pBits = sys_mem_;
  }
  pLockedRect->Pitch = pitch_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::UnlockRect() {
  if (!locked_)
    return D3DERR_INVALIDCALL;
  locked_ = false;
  return S_OK;
}

} // namespace dxmt
