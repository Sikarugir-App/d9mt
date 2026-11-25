#pragma once

#include "com/com_object.hpp"
#include "dxmt_texture.hpp"
#include "d3d9_format.hpp"
#include <d3d9.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

namespace dxmt {

class D3D9Device;
class D3D9TextureSurface;

class D3D9Texture2D final : public ComObjectClamp<IDirect3DTexture9> {
public:
  D3D9Texture2D(D3D9Device *device, UINT width, UINT height, UINT levels,
                 D3DFORMAT format, Rc<Texture> texture, TextureViewKey viewKey)
      : device_(device), width_(width), height_(height), format_(format),
        texture_(std::move(texture)), viewKey_(viewKey) {
    if (levels == 0)
      levels = (UINT)std::floor(std::log2((double)std::max(width, height))) + 1;
    levelCount_ = levels;

    // Allocate per-level staging data
    UINT mipW = width, mipH = height;
    for (UINT i = 0; i < levelCount_; i++) {
      MipLevel mip;
      mip.width = mipW;
      mip.height = mipH;
      mip.pitch = D3D9FormatPitch(format, mipW);
      mip.dataSize = D3D9FormatMipSize(format, mipW, mipH);
      mip.data = std::malloc(mip.dataSize);
      std::memset(mip.data, 0, mip.dataSize);
      mip.dirty = false;
      mips_.push_back(mip);
      mipW = std::max(1u, mipW / 2);
      mipH = std::max(1u, mipH / 2);
    }
  }

  ~D3D9Texture2D() {
    for (auto &mip : mips_) {
      if (mip.data) std::free(mip.data);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DTexture9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_TEXTURE; }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetLOD() final { return 0; }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return levelCount_; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) final { return S_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final { return D3DTEXF_NONE; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() final {}

  // IDirect3DTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final {
    if (Level >= levelCount_ || !pDesc) return D3DERR_INVALIDCALL;
    pDesc->Format = format_;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = 0;
    pDesc->Pool = D3DPOOL_MANAGED;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = mips_[Level].width;
    pDesc->Height = mips_[Level].height;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) final;

  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final {
    if (Level >= levelCount_ || !pLockedRect) return D3DERR_INVALIDCALL;
    auto &mip = mips_[Level];
    pLockedRect->Pitch = mip.pitch;
    if (pRect) {
      // Return pointer to the sub-rect origin within the mip buffer
      if (IsCompressedFormat(format_)) {
        // pitch is per row of 4x4 blocks
        pLockedRect->pBits = static_cast<uint8_t *>(mip.data)
          + (pRect->top / 4) * mip.pitch + (pRect->left / 4) * D3D9FormatBlockSize(format_);
      } else {
        UINT bytesPerPixel = D3D9FormatBytesPerPixel(format_);
        pLockedRect->pBits = static_cast<uint8_t *>(mip.data)
          + pRect->top * mip.pitch + pRect->left * bytesPerPixel;
      }
    } else {
      pLockedRect->pBits = mip.data;
    }
    if (!(Flags & D3DLOCK_NO_DIRTY_UPDATE))
      mip.dirty = true;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) final {
    if (Level >= levelCount_) return D3DERR_INVALIDCALL;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) final { return S_OK; }

  // Internal accessors
  Rc<Texture> &texture() { return texture_; }
  TextureViewKey viewKey() const { return viewKey_; }
  UINT width() const { return width_; }
  UINT height() const { return height_; }
  D3DFORMAT format() const { return format_; }
  UINT levelCount() const { return levelCount_; }

  bool isAnyDirty() const {
    for (auto &mip : mips_)
      if (mip.dirty) return true;
    return false;
  }

  void uploadDirtyLevels(WMT::Texture mtlTexture) {
    bool compressed = IsCompressedFormat(format_);
    for (UINT i = 0; i < levelCount_; i++) {
      auto &mip = mips_[i];
      if (!mip.dirty) continue;

      WMTOrigin origin = {0, 0, 0};
      WMTSize size = {mip.width, mip.height, 1};
      if (compressed) {
        uint32_t rows = (mip.height + 3) / 4;
        mtlTexture.replaceRegion(origin, size, i, 0, mip.data, mip.pitch, mip.pitch * rows);
      } else if (format_ == D3DFMT_A4R4G4B4) {
        // Convert A4R4G4B4 (2bpp) → BGRA8 (4bpp) for upload
        uint32_t gpuPitch = mip.width * 4;
        std::vector<uint32_t> converted(mip.width * mip.height);
        auto *src16 = (const uint16_t *)mip.data;
        for (UINT row = 0; row < mip.height; row++) {
          for (UINT col = 0; col < mip.width; col++) {
            uint16_t px = src16[row * (mip.pitch / 2) + col];
            // D3D9 A4R4G4B4: A(15-12) R(11-8) G(7-4) B(3-0)
            uint8_t a4 = (px >> 12) & 0xF;
            uint8_t r4 = (px >> 8) & 0xF;
            uint8_t g4 = (px >> 4) & 0xF;
            uint8_t b4 = px & 0xF;
            // Expand 4-bit to 8-bit: val * 17 = val * 255 / 15
            // Pack as BGRA8 (little-endian: bytes B, G, R, A)
            converted[row * mip.width + col] =
              (uint32_t)(b4 * 17) |
              ((uint32_t)(g4 * 17) << 8) |
              ((uint32_t)(r4 * 17) << 16) |
              ((uint32_t)(a4 * 17) << 24);
          }
        }
        mtlTexture.replaceRegion(origin, size, i, 0, converted.data(), gpuPitch, 0);
      } else {
        mtlTexture.replaceRegion(origin, size, i, 0, mip.data, mip.pitch, 0);
      }
      mip.dirty = false;
    }
  }

  // Staged upload: copies mip data to a staging buffer and emits a blit command
  // into the current command chunk. This avoids the replaceRegion race where
  // the CPU overwrites shared texture data while the GPU still reads it.
  void uploadDirtyLevelsStaged(Rc<Texture> &gpuTex, dxmt::CommandQueue &queue) {
    bool compressed = IsCompressedFormat(format_);
    for (UINT i = 0; i < levelCount_; i++) {
      auto &mip = mips_[i];
      if (!mip.dirty) continue;

      const void *srcData = mip.data;
      UINT srcPitch = mip.pitch;
      UINT uploadHeight = mip.height;
      UINT uploadPitch = srcPitch;

      // Handle format conversions
      std::vector<uint32_t> convertedBuf;
      if (format_ == D3DFMT_A4R4G4B4) {
        uploadPitch = mip.width * 4;
        convertedBuf.resize(mip.width * mip.height);
        auto *src16 = (const uint16_t *)mip.data;
        for (UINT row = 0; row < mip.height; row++) {
          for (UINT col = 0; col < mip.width; col++) {
            uint16_t px = src16[row * (srcPitch / 2) + col];
            uint8_t a4 = (px >> 12) & 0xF;
            uint8_t r4 = (px >> 8) & 0xF;
            uint8_t g4 = (px >> 4) & 0xF;
            uint8_t b4 = px & 0xF;
            convertedBuf[row * mip.width + col] =
              (uint32_t)(b4 * 17) | ((uint32_t)(g4 * 17) << 8) |
              ((uint32_t)(r4 * 17) << 16) | ((uint32_t)(a4 * 17) << 24);
          }
        }
        srcData = convertedBuf.data();
        srcPitch = uploadPitch;
      }

      if (compressed)
        uploadHeight = (mip.height + 3) / 4;

      size_t uploadSize = (size_t)uploadPitch * uploadHeight;
      auto staging = queue.AllocateTransientBuffer(uploadSize, 16);
      std::memcpy(staging.cpu_ptr, srcData, uploadSize);

      auto chunk = queue.CurrentChunk();
      chunk->emitcc([
        tex = gpuTex,
        stagingBuf = staging.buffer.handle,
        stagingOff = (uint64_t)staging.offset,
        w = mip.width, h = mip.height,
        pitch = uploadPitch, level = i
      ](ArgumentEncodingContext &ctx) mutable {
        ctx.startBlitPass();
        auto dstHandle = ctx.access(tex, (unsigned)level, 0u, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
        auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        blitCmd.type = WMTBlitCommandCopyFromBufferToTexture;
        blitCmd.src = stagingBuf;
        blitCmd.src_offset = stagingOff;
        blitCmd.bytes_per_row = pitch;
        blitCmd.bytes_per_image = 0;
        blitCmd.dst = dstHandle;
        blitCmd.level = level;
        blitCmd.slice = 0;
        blitCmd.origin = {0, 0, 0};
        blitCmd.size = {w, h, 1};
        ctx.endPass();
      });

      mip.dirty = false;
    }
  }

  // Legacy accessors for backward compat with single-level path
  bool isDirty() const { return isAnyDirty(); }
  void clearDirty() {
    for (auto &mip : mips_) mip.dirty = false;
  }
  const void *data() const { return mips_[0].data; }
  UINT pitch() const { return mips_[0].pitch; }

private:
  struct MipLevel {
    UINT width;
    UINT height;
    UINT pitch;
    size_t dataSize;
    void *data = nullptr;
    bool dirty = false;
  };

  D3D9Device *device_;
  UINT width_;
  UINT height_;
  D3DFORMAT format_;
  UINT levelCount_;
  std::vector<MipLevel> mips_;

  Rc<Texture> texture_;
  TextureViewKey viewKey_;
};

// Lightweight surface wrapper for GetSurfaceLevel — delegates Lock/Unlock to parent texture mip
class D3D9TextureSurface final : public ComObjectClamp<IDirect3DSurface9> {
public:
  D3D9TextureSurface(D3D9Texture2D *parent, UINT level)
      : parent_(parent), level_(level) {
    parent_->AddRef();
  }
  ~D3D9TextureSurface() {
    if (parent_) parent_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DSurface9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_SURFACE; }

  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (!ppContainer) return E_POINTER;
    return parent_->QueryInterface(riid, ppContainer);
  }

  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final {
    return parent_->GetLevelDesc(level_, pDesc);
  }

  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final {
    return parent_->LockRect(level_, pLockedRect, pRect, Flags);
  }

  HRESULT STDMETHODCALLTYPE UnlockRect() final {
    return parent_->UnlockRect(level_);
  }

  HRESULT STDMETHODCALLTYPE GetDC(HDC *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) final { return D3DERR_INVALIDCALL; }

  // Internal accessors for render target usage
  Rc<Texture> &texture() { return parent_->texture(); }
  TextureViewKey viewKey() const { return parent_->viewKey(); }
  WMTPixelFormat mtlFormat() const { return parent_->texture()->pixelFormat(); }
  D3D9Texture2D *parentTexture() const { return parent_; }

private:
  D3D9Texture2D *parent_;
  UINT level_;
};

// Deferred implementation — needs D3D9TextureSurface to be complete
inline HRESULT STDMETHODCALLTYPE D3D9Texture2D::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  if (Level >= levelCount_ || !ppSurfaceLevel) return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = ref(new D3D9TextureSurface(this, Level));
  return S_OK;
}

} // namespace dxmt
