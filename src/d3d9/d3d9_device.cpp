
#include "d3d9_device.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_format.hpp"
#include "d3d9_query.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_stateblock.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_vertex_declaration.hpp"
#include "airconv_public.h"
#include "config/config.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_shader_cache.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "util_error.hpp"
#include "wsi_platform.hpp"
#include "wsi_monitor.hpp"
#include "wsi_window.hpp"

namespace dxmt {

static D3DMATRIX IdentityMatrix() {
  D3DMATRIX m = {};
  m._11 = m._22 = m._33 = m._44 = 1.0f;
  return m;
}

static D3DMATRIX MultiplyMatrices(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX r = {};
  const float *A = &a._11;
  const float *B = &b._11;
  float *R = &r._11;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++)
        R[i * 4 + j] += A[i * 4 + k] * B[k * 4 + j];
  return r;
}

uint32_t D3D9Device::TransformIndex(D3DTRANSFORMSTATETYPE state) {
  // D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_TEXTURE0-7 = 16-23
  // D3DTS_WORLD = 256 (D3DTS_WORLDMATRIX(0))
  // D3DTS_WORLDMATRIX(n) = 256 + n
  if (state < 512) return (uint32_t)state;
  return 0;
}

D3D9Device::D3D9Device(IDirect3D9 *pD3D9, HWND hFocusWindow, D3DPRESENT_PARAMETERS *pParams)
    : d3d9_(pD3D9), hwnd_(hFocusWindow), present_params_(*pParams),
      hud_(WMT::DeveloperHUDProperties::instance()) {

  // Use the device window if focus window is not set
  if (!hwnd_)
    hwnd_ = pParams->hDeviceWindow;

  Logger::info("D3D9Device: creating Metal device");

  // Create DXMT device (wraps MTLDevice + CommandQueue)
  auto devices = WMT::CopyAllDevices();
  auto mtl_device = devices.object(0);
  dxmt_device_ = CreateDXMTDevice({.device = mtl_device});

  // Preload: compile all cached shaders to MTLLibrary ahead of first use
  ShaderCache::getInstance(dxmt_device_->metalVersion())
      .preload(dxmt_device_->device());

  // Create Metal view + layer from HWND
  native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hwnd_, mtl_device, layer_weak_);
  if (!native_view_) {
    Logger::err("D3D9: Failed to create metal view");
    throw MTLD3DError("Failed to create metal view");
  }

  // Create presenter for swap chain
  presenter_ = Rc(new Presenter(
      mtl_device, layer_weak_,
      dxmt_device_->queue().cmd_library,
      1.0f, 1));

#ifdef DXMT_PERF
  // 15 lines: 4 DXMT shared + 1 D3D9 header + 10 D3D9 specific
  hud_.initialize("------- DXMT -------", 24, "com.github.3shain.dxmt-d3d9");
#endif

  // Query display refresh rate
  HMONITOR monitor = wsi::getWindowMonitor(hwnd_);
  wsi::WsiMode current_mode;
  if (wsi::getCurrentDisplayMode(monitor, &current_mode) &&
      current_mode.refreshRate.denominator != 0 &&
      current_mode.refreshRate.numerator != 0) {
    init_refresh_rate_ = (double)current_mode.refreshRate.numerator /
                         (double)current_mode.refreshRate.denominator;
  }

  // Get window size if not specified
  if (present_params_.BackBufferWidth == 0 || present_params_.BackBufferHeight == 0)
    wsi::getWindowSize(hwnd_, &present_params_.BackBufferWidth, &present_params_.BackBufferHeight);

  Logger::info(str::format("D3D9Device: backbuffer ",
      present_params_.BackBufferWidth, "x", present_params_.BackBufferHeight,
      " ", D3D9FormatName(present_params_.BackBufferFormat),
      " depth=", D3D9FormatName(present_params_.AutoDepthStencilFormat),
      " windowed=", present_params_.Windowed,
      " buffers=", present_params_.BackBufferCount,
      " swap=", present_params_.SwapEffect,
      " msaa=", (int)present_params_.MultiSampleType));

  // Configure layer properties for presentation
  ApplyFullscreenWindow();
  presenter_->changeLayerProperties(
      WMTPixelFormatBGRA8Unorm, WMTColorSpaceSRGB,
      present_params_.BackBufferWidth, present_params_.BackBufferHeight, 1);

  // Create backbuffer
  CreateBackbuffer(present_params_.BackBufferWidth, present_params_.BackBufferHeight);

  // Pre-allocate snapshot ring — no per-draw allocation
  snapshot_ring_.reset(new ShaderConstants[kSnapshotRingSize]);

  // Initialize current render target to backbuffer
  current_rt_surface_ = backbuffer_surface_;
  current_rt_ = backbuffer_;
  current_rt_view_ = backbuffer_view_;
  current_rt_format_ = WMTPixelFormatBGRA8Unorm;

  // Auto-create depth stencil if requested
  if (present_params_.EnableAutoDepthStencil) {
    IDirect3DSurface9 *depthSurf = nullptr;
    CreateDepthStencilSurface(
        present_params_.BackBufferWidth, present_params_.BackBufferHeight,
        present_params_.AutoDepthStencilFormat,
        D3DMULTISAMPLE_NONE, 0, TRUE, &depthSurf, nullptr);
    if (depthSurf) {
      SetDepthStencilSurface(depthSurf);
      depthSurf->Release();
    }
  }

  // Initialize default render states
  render_states_[D3DRS_ZENABLE] = present_params_.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
  render_states_[D3DRS_ZWRITEENABLE] = TRUE;
  render_states_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  render_states_[D3DRS_ALPHABLENDENABLE] = FALSE;
  render_states_[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  render_states_[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  render_states_[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  render_states_[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  render_states_[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  render_states_[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  render_states_[D3DRS_CULLMODE] = D3DCULL_CCW;
  render_states_[D3DRS_FILLMODE] = D3DFILL_SOLID;
  render_states_[D3DRS_COLORWRITEENABLE] = 0xF;
  render_states_[D3DRS_LIGHTING] = TRUE;
  render_states_[D3DRS_SPECULARENABLE] = FALSE;
  render_states_[D3DRS_AMBIENT] = 0;
  render_states_[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  render_states_[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  render_states_[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  render_states_[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  render_states_[D3DRS_COLORVERTEX] = TRUE;
  render_states_[D3DRS_NORMALIZENORMALS] = FALSE;
  render_states_[D3DRS_FOGCOLOR] = 0;
  render_states_[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
  render_states_[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
  render_states_[D3DRS_FOGENABLE] = FALSE;
  render_states_[D3DRS_ALPHATESTENABLE] = FALSE;
  render_states_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  render_states_[D3DRS_ALPHAREF] = 0;
  render_states_[D3DRS_STENCILENABLE] = FALSE;
  render_states_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  render_states_[D3DRS_STENCILREF] = 0;
  render_states_[D3DRS_STENCILMASK] = 0xFFFFFFFF;
  render_states_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  render_states_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_SCISSORTESTENABLE] = FALSE;
  render_states_[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;

  // Set default viewport
  viewport_.X = 0;
  viewport_.Y = 0;
  viewport_.Width = present_params_.BackBufferWidth;
  viewport_.Height = present_params_.BackBufferHeight;
  viewport_.MinZ = 0.0f;
  viewport_.MaxZ = 1.0f;

  // Initialize transforms to identity
  D3DMATRIX identity = IdentityMatrix();
  for (auto &t : transforms_)
    t = identity;

  // Initialize default texture stage states
  for (DWORD stage = 0; stage < 8; stage++) {
    texture_stage_states_[stage][D3DTSS_COLOROP] = (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    texture_stage_states_[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    texture_stage_states_[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_ALPHAOP] = (stage == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    texture_stage_states_[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    texture_stage_states_[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_TEXCOORDINDEX] = stage;
    texture_stage_states_[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    texture_stage_states_[stage][D3DTSS_COLORARG0] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_RESULTARG] = D3DTA_CURRENT;
  }

  // Create default 1x1 white texture for unbound sampler fallback
  {
    WMTTextureInfo whiteInfo = {};
    whiteInfo.pixel_format = WMTPixelFormatBGRA8Unorm;
    whiteInfo.width = 1;
    whiteInfo.height = 1;
    whiteInfo.depth = 1;
    whiteInfo.array_length = 1;
    whiteInfo.type = WMTTextureType2D;
    whiteInfo.mipmap_level_count = 1;
    whiteInfo.sample_count = 1;
    whiteInfo.usage = WMTTextureUsageShaderRead;
    whiteInfo.options = WMTResourceStorageModeShared;
    default_white_tex_ = Rc(new Texture(whiteInfo, dxmt_device_->device()));
    default_white_tex_->rename(default_white_tex_->allocate({}));
    TextureViewDescriptor whiteViewDesc = {
        .format = WMTPixelFormatBGRA8Unorm,
        .type = WMTTextureType2D,
        .firstMiplevel = 0,
        .miplevelCount = 1,
        .firstArraySlice = 0,
        .arraySize = 1,
    };
    default_white_view_ = default_white_tex_->createView(whiteViewDesc);
    // Fill with opaque white (BGRA)
    auto whiteTex = default_white_tex_->current()->texture();
    uint32_t whitePixel = 0xFFFFFFFF;
    WMTOrigin origin = {0, 0, 0};
    WMTSize size = {1, 1, 1};
    whiteTex.replaceRegion(origin, size, 0, 0, &whitePixel, 4, 0);
    // Create a default sampler state for unbound slots
    WMTSamplerInfo defaultSampInfo = {};
    defaultSampInfo.min_filter = WMTSamplerMinMagFilterNearest;
    defaultSampInfo.mag_filter = WMTSamplerMinMagFilterNearest;
    defaultSampInfo.mip_filter = WMTSamplerMipFilterNotMipmapped;
    defaultSampInfo.s_address_mode = WMTSamplerAddressModeClampToEdge;
    defaultSampInfo.t_address_mode = WMTSamplerAddressModeClampToEdge;
    defaultSampInfo.r_address_mode = WMTSamplerAddressModeClampToEdge;
    default_sampler_ = MTLDevice_newSamplerState(dxmt_device_->device().handle, &defaultSampInfo);
  }

  // Initialize default material
  material_.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
  material_.Ambient = {0.0f, 0.0f, 0.0f, 0.0f};
  material_.Specular = {0.0f, 0.0f, 0.0f, 0.0f};
  material_.Emissive = {0.0f, 0.0f, 0.0f, 0.0f};
  material_.Power = 0.0f;

  Logger::info("D3D9Device: created successfully");
}

D3D9Device::~D3D9Device() {
  if (cursor_handle_)
    DestroyCursor(cursor_handle_);
  if (native_view_)
    WMT::ReleaseMetalView(native_view_);
  Logger::info("D3D9Device: destroyed");
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) {
  if (!pCursorBitmap)
    return D3DERR_INVALIDCALL;

  D3DSURFACE_DESC desc;
  if (FAILED(pCursorBitmap->GetDesc(&desc)))
    return D3DERR_INVALIDCALL;

  D3DLOCKED_RECT lr;
  if (FAILED(pCursorBitmap->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    return D3DERR_INVALIDCALL;

  UINT w = desc.Width;
  UINT h = desc.Height;

  int scale = Config::getInstance().getOption<int>("d3d9.cursorScale", 1);
  if (scale < 1) scale = 1;
  if (scale > 8) scale = 8;

  UINT sw = w * scale;
  UINT sh = h * scale;

  // Build BGRA pixel buffer at final size
  std::vector<uint32_t> pixels(sw * sh);

  if (scale == 1) {
    auto *src = static_cast<const uint8_t *>(lr.pBits);
    for (UINT y = 0; y < h; y++) {
      memcpy(&pixels[y * sw], src + y * lr.Pitch, w * 4);
    }
  } else {
    auto *src = static_cast<const uint8_t *>(lr.pBits);
    for (UINT y = 0; y < sh; y++) {
      auto *srcRow = reinterpret_cast<const uint32_t *>(src + (y / scale) * lr.Pitch);
      for (UINT x = 0; x < sw; x++) {
        pixels[y * sw + x] = srcRow[x / scale];
      }
    }
  }

  pCursorBitmap->UnlockRect();

  // AND mask: all zeros = entire image is used (no transparency from mask)
  UINT maskSize = ((sw + 31) / 32) * 4 * sh;
  std::vector<uint8_t> andMask(maskSize, 0);

  HBITMAP hColor = CreateBitmap(sw, sh, 1, 32, pixels.data());
  HBITMAP hMask = CreateBitmap(sw, sh, 1, 1, andMask.data());

  ICONINFO ii = {};
  ii.fIcon = FALSE;
  ii.xHotspot = XHotSpot * scale;
  ii.yHotspot = YHotSpot * scale;
  ii.hbmMask = hMask;
  ii.hbmColor = hColor;

  HCURSOR newCursor = (HCURSOR)CreateIconIndirect(&ii);

  DeleteObject(hColor);
  DeleteObject(hMask);

  if (!newCursor)
    return D3DERR_INVALIDCALL;

  HCURSOR old = cursor_handle_;
  cursor_handle_ = newCursor;

  if (cursor_visible_)
    SetCursor(cursor_handle_);

  if (old)
    DestroyCursor(old);

  return S_OK;
}

void STDMETHODCALLTYPE D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE D3D9Device::ShowCursor(BOOL bShow) {
  BOOL prev = cursor_visible_;
  cursor_visible_ = bShow;
  SetCursor(cursor_visible_ ? cursor_handle_ : nullptr);
  return prev;
}

HRESULT D3D9Device::CreateBackbuffer(UINT width, UINT height) {
  WMTTextureInfo info = {};
  info.pixel_format = WMTPixelFormatBGRA8Unorm;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  info.options = WMTResourceStorageModePrivate;

  backbuffer_ = Rc(new Texture(info, dxmt_device_->device()));
  backbuffer_->rename(backbuffer_->allocate({}));

  backbuffer_view_ = backbuffer_->createView({
      .format = WMTPixelFormatBGRA8Unorm,
      .type = WMTTextureType2DArray,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });

  backbuffer_surface_ = new D3D9Surface(this, backbuffer_, backbuffer_view_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
    *ppvObj = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Device::TestCooperativeLevel() { return S_OK; }
UINT STDMETHODCALLTYPE D3D9Device::GetAvailableTextureMem() { return 512 * 1024 * 1024; }
HRESULT STDMETHODCALLTYPE D3D9Device::EvictManagedResources() { return S_OK; }

HRESULT STDMETHODCALLTYPE D3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
  if (!ppD3D9) return D3DERR_INVALIDCALL;
  *ppD3D9 = d3d9_.ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  return d3d9_->GetDeviceCaps(0, D3DDEVTYPE_HAL, pCaps);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDisplayMode(UINT, D3DDISPLAYMODE *pMode) {
  if (!pMode) return D3DERR_INVALIDCALL;
  pMode->Width = present_params_.BackBufferWidth;
  pMode->Height = present_params_.BackBufferHeight;
  pMode->RefreshRate = 60;
  pMode->Format = D3DFMT_X8R8G8B8;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  if (!pParameters) return D3DERR_INVALIDCALL;
  pParameters->AdapterOrdinal = 0;
  pParameters->DeviceType = D3DDEVTYPE_HAL;
  pParameters->hFocusWindow = hwnd_;
  pParameters->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
  return S_OK;
}

// Fullscreen D3D9 apps (e.g. RenderWare/GTA SA) expect the device window's client
// area to match the backbuffer; wined3d and DXGI resize/restyle the window for them.
void D3D9Device::ApplyFullscreenWindow() {
  if (present_params_.Windowed || !hwnd_ || !wsi::isWindow(hwnd_))
    return;
  HMONITOR monitor = wsi::getWindowMonitor(hwnd_);
  // Switch the display mode to the backbuffer size first (Wine clamps windows to the screen)
  wsi::WsiMode mode = {};
  mode.width = present_params_.BackBufferWidth;
  mode.height = present_params_.BackBufferHeight;
  mode.refreshRate = {present_params_.FullScreen_RefreshRateInHz, present_params_.FullScreen_RefreshRateInHz ? 1u : 0u};
  mode.bitsPerPixel = 32;
  mode.interlaced = false;
  bool modeOk = wsi::setWindowMode(monitor, hwnd_, mode);
  Logger::info(str::format("D3D9Device: display mode ", mode.width, "x", mode.height, modeOk ? " set" : " NOT set"));
  if (!fullscreen_window_)
    fullscreen_window_ = wsi::enterFullscreenMode(monitor, hwnd_, &window_state_, false);
  RECT rect = {};
  ::GetWindowRect(hwnd_, &rect);
  ::SetWindowPos(hwnd_, HWND_TOP, rect.left, rect.top,
                 present_params_.BackBufferWidth, present_params_.BackBufferHeight,
                 SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  // A fullscreen device owns the foreground. RenderWare only finishes its startup
  // (calls Reset and starts rendering) once its window is activated, so don't rely on a click.
  if (::GetForegroundWindow() != hwnd_) {
    ::SetForegroundWindow(hwnd_);
    ::SetActiveWindow(hwnd_);
    ::SetFocus(hwnd_);
  }
  Logger::info(str::format("D3D9Device: foreground=", ::GetForegroundWindow() == hwnd_ ? "yes" : "no"));
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  Logger::info(str::format("D3D9Device: fullscreen window client ", client.right - client.left, "x",
                           client.bottom - client.top));
}

HRESULT STDMETHODCALLTYPE D3D9Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  if (!pPresentationParameters) return D3DERR_INVALIDCALL;
  present_params_ = *pPresentationParameters;
  if (present_params_.BackBufferWidth == 0 || present_params_.BackBufferHeight == 0)
    wsi::getWindowSize(hwnd_, &present_params_.BackBufferWidth, &present_params_.BackBufferHeight);

  ApplyFullscreenWindow();
  presenter_->changeLayerProperties(
      WMTPixelFormatBGRA8Unorm, WMTColorSpaceSRGB,
      present_params_.BackBufferWidth, present_params_.BackBufferHeight, 1);

  CreateBackbuffer(present_params_.BackBufferWidth, present_params_.BackBufferHeight);

  // Like real D3D9: Reset recreates the automatic depth/stencil surface and binds it
  // (apps must release their own DEFAULT-pool surfaces before Reset; RenderWare relies on this).
  if (present_params_.EnableAutoDepthStencil) {
    IDirect3DSurface9 *depthSurf = nullptr;
    CreateDepthStencilSurface(
        present_params_.BackBufferWidth, present_params_.BackBufferHeight,
        present_params_.AutoDepthStencilFormat,
        D3DMULTISAMPLE_NONE, 0, TRUE, &depthSurf, nullptr);
    SetDepthStencilSurface(depthSurf);
    if (depthSurf)
      depthSurf->Release();
  } else {
    SetDepthStencilSurface(nullptr);
  }

  // Reset current RT to new backbuffer
  current_rt_surface_ = backbuffer_surface_;
  current_rt_ = backbuffer_;
  current_rt_view_ = backbuffer_view_;
  current_rt_format_ = WMTPixelFormatBGRA8Unorm;

  Logger::info(str::format("D3D9Device: Reset backbuffer ",
      present_params_.BackBufferWidth, "x", present_params_.BackBufferHeight,
      " ", D3D9FormatName(present_params_.BackBufferFormat),
      " depth=", D3D9FormatName(present_params_.AutoDepthStencilFormat),
      " windowed=", present_params_.Windowed,
      " msaa=", (int)present_params_.MultiSampleType));

  viewport_.X = 0;
  viewport_.Y = 0;
  viewport_.Width = present_params_.BackBufferWidth;
  viewport_.Height = present_params_.BackBufferHeight;
  viewport_.MinZ = 0.0f;
  viewport_.MaxZ = 1.0f;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) {
  if (iSwapChain != 0 || iBackBuffer != 0 || !ppBackBuffer)
    return D3DERR_INVALIDCALL;
  *ppBackBuffer = ref(backbuffer_surface_.ptr());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  FlushDrawBatch();
  if (RenderTargetIndex != 0) {
    Logger::warn(str::format("D3D9: SetRenderTarget index ", RenderTargetIndex, " not supported"));
    return D3DERR_INVALIDCALL;
  }
  if (!pRenderTarget) {
    // RT0 cannot be set to null
    return D3DERR_INVALIDCALL;
  }

  // Determine if this is a D3D9Surface or D3D9TextureSurface
  // Try GetContainer to detect texture surface
  IDirect3DTexture9 *containerTex = nullptr;
  bool isTexSurface = SUCCEEDED(pRenderTarget->GetContainer(__uuidof(IDirect3DTexture9), (void **)&containerTex));
  if (containerTex) containerTex->Release();

  Rc<Texture> rtTex;
  TextureViewKey rtView;
  WMTPixelFormat rtFormat;

  if (isTexSurface) {
    auto *texSurf = static_cast<D3D9TextureSurface *>(pRenderTarget);
    rtTex = texSurf->texture();
    rtView = texSurf->viewKey();
    rtFormat = texSurf->mtlFormat();
    current_rt_surface_ = nullptr; // Not a D3D9Surface
  } else {
    auto *surf = static_cast<D3D9Surface *>(pRenderTarget);
    rtTex = surf->texture();
    rtView = surf->viewKey();
    rtFormat = surf->mtlFormat();
    current_rt_surface_ = surf;
  }

  current_rt_ = rtTex;
  current_rt_view_ = rtView;
  current_rt_format_ = rtFormat;


  // Update viewport to match new render target size
  viewport_.X = 0;
  viewport_.Y = 0;
  viewport_.Width = current_rt_->width();
  viewport_.Height = current_rt_->height();
  viewport_.MinZ = 0.0f;
  viewport_.MaxZ = 1.0f;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
  if (RenderTargetIndex != 0 || !ppRenderTarget)
    return D3DERR_INVALIDCALL;
  if (current_rt_surface_) {
    *ppRenderTarget = ref(current_rt_surface_.ptr());
  } else {
    // Fallback to backbuffer when using texture surface RT
    *ppRenderTarget = ref(backbuffer_surface_.ptr());
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::BeginScene() { return S_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::EndScene() { return S_OK; }

HRESULT STDMETHODCALLTYPE D3D9Device::Clear(
    DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  if (!(Flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return S_OK;

  FlushDrawBatch();

  bool clearColor = (Flags & D3DCLEAR_TARGET) != 0;
  bool clearDepth = (Flags & D3DCLEAR_ZBUFFER) != 0 && depth_stencil_;

  float r = ((Color >> 16) & 0xFF) / 255.0f;
  float g = ((Color >> 8) & 0xFF) / 255.0f;
  float b = ((Color >> 0) & 0xFF) / 255.0f;
  float a = ((Color >> 24) & 0xFF) / 255.0f;

  bool clearStencil = (Flags & D3DCLEAR_STENCIL) != 0 && depth_stencil_;

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  // D3D9 clears only the viewport, intersected with the scissor rect when enabled and with each given rect.
  // (GTA SA clears colour+depth+stencil of the backbuffer with a 1024x512 viewport mid-frame; clearing the whole
  // surface there wiped everything drawn so far = one-frame flashes of buildings/ground.)
  {
    UINT rtw = current_rt_ ? current_rt_->width() : 0, rth = current_rt_ ? current_rt_->height() : 0;
    LONG bx0 = (LONG)viewport_.X, by0 = (LONG)viewport_.Y;
    LONG bx1 = bx0 + (LONG)viewport_.Width, by1 = by0 + (LONG)viewport_.Height;
    if (render_states_[D3DRS_SCISSORTESTENABLE]) {
      bx0 = std::max(bx0, scissor_rect_.left);  by0 = std::max(by0, scissor_rect_.top);
      bx1 = std::min(bx1, scissor_rect_.right); by1 = std::min(by1, scissor_rect_.bottom);
    }
    std::vector<RECT> rects;
    if (Count > 0 && pRects) {
      for (DWORD i = 0; i < Count; i++) {
        RECT r = {std::max(bx0, pRects[i].x1), std::max(by0, pRects[i].y1),
                  std::min(bx1, pRects[i].x2), std::min(by1, pRects[i].y2)};
        if (r.right > r.left && r.bottom > r.top) rects.push_back(r);
      }
    } else if (bx1 > bx0 && by1 > by0) {
      rects.push_back({bx0, by0, bx1, by1});
    }
    if (rects.empty()) return S_OK;
    auto covers = [&](UINT w, UINT h) {
      return rects.size() == 1 && rects[0].left <= 0 && rects[0].top <= 0 &&
             rects[0].right >= (LONG)w && rects[0].bottom >= (LONG)h;
    };
    bool colorFull = !clearColor || !current_rt_ || covers(rtw, rth);
    bool dsFull = !(clearDepth || clearStencil) || covers(depth_stencil_->width(), depth_stencil_->height());
    if (!colorFull || !dsFull) {
      unsigned dsFlags = (clearDepth ? 1 : 0) | (clearStencil ? 2 : 0);
      chunk->emitcc([
        rects = std::move(rects), r, g, b, a, Z, Stencil, dsFlags,
        rt = clearColor ? current_rt_ : Rc<Texture>(), rt_view = current_rt_view_,
        depth = dsFlags ? depth_stencil_ : Rc<Texture>(), depth_view = depth_stencil_view_
      ](ArgumentEncodingContext &ctx) mutable {
        auto run = [&](Rc<Texture> &tex, TextureViewKey view, unsigned flags, uint8_t st,
                       const std::array<float, 4> &value) {
          LONG w = (LONG)tex->width(view), h = (LONG)tex->height(view);
          ctx.clear_rt_cmd.begin(tex, view, flags, st);
          for (auto &rc : rects) {
            LONG x0 = std::max(rc.left, 0L), y0 = std::max(rc.top, 0L);
            LONG x1 = std::min(rc.right, w), y1 = std::min(rc.bottom, h);
            if (x1 > x0 && y1 > y0)
              ctx.clear_rt_cmd.clear(x0, y0, x1 - x0, y1 - y0, value);
          }
          ctx.clear_rt_cmd.end();
        };
        if (rt) run(rt, rt_view, 1, 0, {r, g, b, a});
        if (depth) run(depth, depth_view, dsFlags, (uint8_t)Stencil, {Z, 0, 0, 0});
      });
      return S_OK;
    }
  }

  if (clearColor) {
    chunk->emitcc([
      r, g, b, a,
      rt = current_rt_,
      rt_view = current_rt_view_
    ](ArgumentEncodingContext &ctx) mutable {
      ctx.clearColor(std::move(rt), rt_view, 1, {r, g, b, a});
    });
  }

  if ((clearDepth || clearStencil) && depth_stencil_) {
    unsigned dsFlags = (clearDepth ? 1 : 0) | (clearStencil ? 2 : 0);
    chunk->emitcc([
      Z, Stencil, dsFlags,
      depth = depth_stencil_,
      depth_view = depth_stencil_view_
    ](ArgumentEncodingContext &ctx) mutable {
      ctx.clearDepthStencil(std::move(depth), depth_view, 1, dsFlags, Z, (uint8_t)Stencil);
    });
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetViewport(const D3DVIEWPORT9 *pViewport) {
  if (!pViewport) return D3DERR_INVALIDCALL;
  viewport_ = *pViewport;
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyViewport;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetViewport(D3DVIEWPORT9 *pViewport) {
  if (!pViewport) return D3DERR_INVALIDCALL;
  *pViewport = viewport_;
  return S_OK;
}

// Render state
HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_state_change_count++;
#endif
  if (State >= 256) return D3DERR_INVALIDCALL;
  if (render_states_[State] == Value) return S_OK; // no-op: skip dirty tracking
  render_states_[State] = Value;

  // Bump constant versions for render states that get packed into shader constants
  switch (State) {
  case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAFUNC: case D3DRS_FOGENABLE:
    pso_dirty_ = true; // these affect PSO variant key
    [[fallthrough]];
  case D3DRS_ALPHAREF:
  case D3DRS_FOGCOLOR: case D3DRS_FOGSTART: case D3DRS_FOGEND: case D3DRS_FOGDENSITY:
  case D3DRS_TEXTUREFACTOR: // FF PS c[0]; was never re-uploaded (GTA SA car env map used a stale factor)
    ps_const_version_++;
    ff_const_version_++;
    ff_dirty_ |= kFFDirtyFog | kFFDirtyPS;
    break;
  case D3DRS_AMBIENT:
    ff_const_version_++;
    ff_dirty_ |= kFFDirtyAmbient;
    break;
  default:
    break;
  }

  // Mark PSO dirty for blend/depth/write/FF state changes
  switch (State) {
  case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP:
  case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
  case D3DRS_SRGBWRITEENABLE: case D3DRS_COLORWRITEENABLE:
  case D3DRS_FOGTABLEMODE: case D3DRS_FOGVERTEXMODE:
  case D3DRS_LIGHTING: case D3DRS_NORMALIZENORMALS: case D3DRS_SPECULARENABLE:
  case D3DRS_DIFFUSEMATERIALSOURCE: case D3DRS_AMBIENTMATERIALSOURCE:
  case D3DRS_SPECULARMATERIALSOURCE: case D3DRS_EMISSIVEMATERIALSOURCE:
  case D3DRS_COLORVERTEX:
    pso_dirty_ = true;
    break;
  default:
    break;
  }

  // Mark DSSO dirty for depth/stencil state changes
  switch (State) {
  case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC:
  case D3DRS_STENCILENABLE: case D3DRS_STENCILFUNC:
  case D3DRS_STENCILPASS: case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL:
  case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_STENCILREF:
  case D3DRS_CULLMODE:
    dsso_dirty_ = true;
    break;
  default:
    break;
  }

  // Warn once for render states we store but don't act on
  static bool warned[256] = {};
  if (!warned[State] && Value != 0) {
    switch (State) {
    case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_WRAP0: case D3DRS_WRAP1: case D3DRS_WRAP2: case D3DRS_WRAP3:
    case D3DRS_WRAP4: case D3DRS_WRAP5: case D3DRS_WRAP6: case D3DRS_WRAP7:
    case D3DRS_CLIPPING:
    case D3DRS_POINTSIZE:
    case D3DRS_TWOSIDEDSTENCILMODE:
      Logger::warn(str::format("D3D9: unhandled render state ", State, " = ", Value));
      warned[State] = true;
      break;
    default:
      break;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
  if (!pValue || State >= 256) return D3DERR_INVALIDCALL;
  *pValue = render_states_[State];
  return S_OK;
}

// Transform state
HRESULT STDMETHODCALLTYPE D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= 512) return D3DERR_INVALIDCALL;
  transforms_[idx] = *pMatrix;
  ff_const_version_++;
  if (State == D3DTS_WORLD)
    ff_dirty_ |= kFFDirtyWorld;
  else if (State == D3DTS_VIEW)
    ff_dirty_ |= kFFDirtyView | kFFDirtyLights;
  else if (State == D3DTS_PROJECTION)
    ff_dirty_ |= kFFDirtyProj;
  else if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE7)
    ff_dirty_ |= kFFDirtyTexMat;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= 512) return D3DERR_INVALIDCALL;
  *pMatrix = transforms_[idx];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= 512) return D3DERR_INVALIDCALL;
  transforms_[idx] = MultiplyMatrices(transforms_[idx], *pMatrix);
  ff_const_version_++;
  if (State == D3DTS_WORLD)
    ff_dirty_ |= kFFDirtyWorld;
  else if (State == D3DTS_VIEW)
    ff_dirty_ |= kFFDirtyView | kFFDirtyLights;
  else if (State == D3DTS_PROJECTION)
    ff_dirty_ |= kFFDirtyProj;
  else if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE7)
    ff_dirty_ |= kFFDirtyTexMat;
  return S_OK;
}

// Material
HRESULT STDMETHODCALLTYPE D3D9Device::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  if (!pMaterial) return D3DERR_INVALIDCALL;
  material_ = *pMaterial;
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyMaterial;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetMaterial(D3DMATERIAL9 *pMaterial) {
  if (!pMaterial) return D3DERR_INVALIDCALL;
  *pMaterial = material_;
  return S_OK;
}

// Lights
HRESULT STDMETHODCALLTYPE D3D9Device::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
  if (!pLight || Index >= kMaxLights) return D3DERR_INVALIDCALL;
  if (pLight->Type != lights_[Index].Type)
    pso_dirty_ = true; // only light Type affects FF VS key
  lights_[Index] = *pLight;
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyLights;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
  if (!pLight || Index >= kMaxLights) return D3DERR_INVALIDCALL;
  *pLight = lights_[Index];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  if (Index >= kMaxLights) return D3DERR_INVALIDCALL;
  light_enabled_[Index] = Enable;
  pso_dirty_ = true; // light enable affects FF VS key
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyLights;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  if (!pEnable || Index >= kMaxLights) return D3DERR_INVALIDCALL;
  *pEnable = light_enabled_[Index];
  return S_OK;
}

// Texture stage states
HRESULT STDMETHODCALLTYPE D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  if (Stage >= 8 || Type >= 33) return D3DERR_INVALIDCALL;
  if (texture_stage_states_[Stage][Type] == Value) return S_OK;
  texture_stage_states_[Stage][Type] = Value;
  // Only states that affect FF shader keys need to dirty PSO
  switch (Type) {
  case D3DTSS_COLOROP: case D3DTSS_COLORARG1: case D3DTSS_COLORARG2:
  case D3DTSS_ALPHAOP: case D3DTSS_ALPHAARG1: case D3DTSS_ALPHAARG2:
  case D3DTSS_TEXCOORDINDEX: case D3DTSS_TEXTURETRANSFORMFLAGS:
  case D3DTSS_COLORARG0: case D3DTSS_ALPHAARG0: case D3DTSS_RESULTARG:
    pso_dirty_ = true;
    break;
  default:
    break;
  }
  // Only COLOROP affects which stages get white texture fallback
  if (Type == D3DTSS_COLOROP)
    tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  if (Stage >= 8 || Type >= 33 || !pValue) return D3DERR_INVALIDCALL;
  *pValue = texture_stage_states_[Stage][Type];
  return S_OK;
}

// Texture creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) {
  if (!ppTexture) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9Format(Format);
  if (mtlFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("D3D9: CreateTexture unsupported format ", (int)Format));
    return D3DERR_INVALIDCALL;
  }

  // Compute mip levels
  UINT mipLevels = Levels;
  if (mipLevels == 0)
    mipLevels = (UINT)std::floor(std::log2((double)std::max(Width, Height))) + 1;

  bool isRenderTarget = (Usage & D3DUSAGE_RENDERTARGET) != 0;

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = mipLevels;
  info.sample_count = 1;
  if (isRenderTarget) {
    info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
    info.options = WMTResourceStorageModePrivate;
  } else {
    info.usage = WMTTextureUsageShaderRead;
    info.options = WMTResourceStorageModeShared;
  }

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  TextureViewDescriptor viewDesc = {
      .format = mtlFormat,
      .type = WMTTextureType2D,
      .firstMiplevel = 0,
      .miplevelCount = mipLevels,
      .firstArraySlice = 0,
      .arraySize = 1,
  };

  TextureViewKey viewKey;
  if (Format == D3DFMT_A8L8) {
    // A8L8 maps to RG8Unorm. D3D9 expects RGB=luminance, A=alpha.
    // Metal sees R=luminance, G=alpha. Apply .rrrg swizzle.
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleGreen
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else if (Format == D3DFMT_X8R8G8B8 || Format == D3DFMT_X1R5G5B5) {
    // X formats: the unused byte/bit must read as alpha = 1 (Metal returns whatever is stored there).
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleOne
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else if (Format == D3DFMT_L8) {
    // L8 maps to R8Unorm. D3D9 expects luminance replicated to RGB, alpha=1.
    // Metal sees (R, 0, 0, 1). Apply .rrr1 swizzle.
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else {
    viewKey = texture->createView(viewDesc);
  }

  *ppTexture = ref(new D3D9Texture2D(this, Width, Height, Levels, Format, std::move(texture), viewKey));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_state_change_count++;
#endif
  if (Stage >= 16) return D3DERR_INVALIDCALL;
  auto *tex = static_cast<D3D9Texture2D *>(pTexture);
  if (bound_textures_[Stage].ptr() == tex) return S_OK; // no-op
  bound_textures_[Stage] = tex;
  if (tex)
    tex_bound_mask_ |= (1u << Stage);
  else
    tex_bound_mask_ &= ~(1u << Stage);
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  if (Stage >= 16 || !ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = bound_textures_[Stage].ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_state_change_count++;
#endif
  if (Sampler >= 16 || Type >= 14) return D3DERR_INVALIDCALL;
  if (sampler_states_[Sampler][Type] == Value) return S_OK;
  sampler_states_[Sampler][Type] = Value;
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
  if (Sampler >= 16 || Type >= 14 || !pValue) return D3DERR_INVALIDCALL;
  *pValue = sampler_states_[Sampler][Type];
  return S_OK;
}

// Depth/stencil surface
HRESULT STDMETHODCALLTYPE D3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
  FlushDrawBatch();
  if (pNewZStencil) {
    auto *surf = static_cast<D3D9Surface *>(pNewZStencil);
    depth_stencil_ = surf->texture();
    depth_stencil_view_ = surf->viewKey();
    depth_stencil_surface_ = surf;
    depth_stencil_format_ = surf->mtlFormat();
  } else {
    depth_stencil_ = nullptr;
    depth_stencil_view_ = 0;
    depth_stencil_surface_ = nullptr;
    depth_stencil_format_ = WMTPixelFormatInvalid;
  }
  pso_dirty_ = true;
  dsso_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
  if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
  if (!depth_stencil_surface_) return D3DERR_NOTFOUND;
  *ppZStencilSurface = ref(depth_stencil_surface_.ptr());
  return S_OK;
}

static WMTPixelFormat ConvertD3D9DepthFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
    return WMTPixelFormatDepth16Unorm;
  case D3DFMT_D24S8:
  case D3DFMT_D24X4S4:
    return WMTPixelFormatDepth32Float_Stencil8; // Use D32F+S8 for stencil support
  case D3DFMT_D24X8:
    return WMTPixelFormatDepth32Float; // No stencil needed
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
    return WMTPixelFormatDepth32Float;
  case D3DFMT_D24FS8:
    return WMTPixelFormatDepth32Float_Stencil8;
  default:
    return WMTPixelFormatDepth32Float;
  }
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  if (!ppSurface) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9DepthFormat(Format);

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageRenderTarget;
  info.options = WMTResourceStorageModePrivate;

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  auto viewKey = texture->createView({
      .format = mtlFormat,
      .type = WMTTextureType2DArray,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });

  auto *dsSurface = new D3D9Surface(this, texture, viewKey);
  dsSurface->setD3DDesc(Format, D3DUSAGE_DEPTHSTENCIL);
  *ppSurface = ref(dsSurface);
  return S_OK;
}

// Buffer creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) {
  if (!ppVertexBuffer) return D3DERR_INVALIDCALL;

  auto buffer = Rc(new Buffer(Length, dxmt_device_->device()));
  Flags<BufferAllocationFlag> bufFlags;
  bufFlags.set(BufferAllocationFlag::CpuPlaced);
  auto allocation = buffer->allocate(bufFlags);
  memset(allocation->mappedMemory(0), 0, Length);
  buffer->rename(Rc(allocation));

  *ppVertexBuffer = ref(new D3D9VertexBuffer(this, Length, Usage, FVF, Pool,
                                              std::move(buffer), std::move(allocation)));
  return S_OK;
}

// Index buffer creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) {
  if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;

  auto buffer = Rc(new Buffer(Length, dxmt_device_->device()));
  Flags<BufferAllocationFlag> bufFlags;
  bufFlags.set(BufferAllocationFlag::CpuPlaced);
  auto allocation = buffer->allocate(bufFlags);
  memset(allocation->mappedMemory(0), 0, Length);
  buffer->rename(Rc(allocation));

  *ppIndexBuffer = ref(new D3D9IndexBuffer(this, Length, Usage, Format, Pool,
                                            std::move(buffer), std::move(allocation)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  auto *ib = static_cast<D3D9IndexBuffer *>(pIndexData);
  if (current_ib_.ptr() == ib) return S_OK;
  current_ib_ = ib;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  if (!ppIndexData) return D3DERR_INVALIDCALL;
  *ppIndexData = current_ib_.ref();
  return S_OK;
}

// FVF
HRESULT STDMETHODCALLTYPE D3D9Device::SetFVF(DWORD FVF) {
  if (FVF == 0) return S_OK;

  if (current_fvf_ == FVF) return S_OK;
  current_fvf_ = FVF;
  pso_dirty_ = true;

  auto it = fvf_cache_.find(FVF);
  if (it != fvf_cache_.end()) {
    current_vdecl_ = it->second;
    return S_OK;
  }

  auto decl = D3D9VertexDeclaration::CreateFromFVF(FVF);
  fvf_cache_.emplace(FVF, decl);
  current_vdecl_ = std::move(decl);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetFVF(DWORD *pFVF) {
  if (!pFVF) return D3DERR_INVALIDCALL;
  *pFVF = current_fvf_;
  return S_OK;
}

// Vertex declaration
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
  if (!pVertexElements || !ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = ref(new D3D9VertexDeclaration(pVertexElements));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  auto *decl = static_cast<D3D9VertexDeclaration *>(pDecl);
  if (current_vdecl_.ptr() == decl) return S_OK;
  current_vdecl_ = decl;
  current_fvf_ = 0;
  pso_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = current_vdecl_.ref();
  return S_OK;
}

// Compile a DXSO pixel shader to Metal function
static WMT::Reference<WMT::Function> CompileDxsoPixelShader(WMT::Device device, dxso_shader_t shader,
                                                              const Sha1Digest &shaderSha1,
                                                              const char *funcName, WMTMetalVersion metalVersion,
                                                              uint8_t alphaTestFunc = 0,
                                                              uint8_t fogMode = 0) {
  auto start_ns = ShaderCache::nowNs();
  // Cache key: shader SHA1 + variant params
  Sha1HashState keyHash;
  keyHash.update(shaderSha1);
  keyHash.update(alphaTestFunc);
  keyHash.update(fogMode);
  auto cacheKey = keyHash.final();

  auto &cache = ShaderCache::getInstance(metalVersion);

  // Check preloaded libraries (all disk cache entries are preloaded at startup)
  {
    auto library = cache.findPreloadedLibrary(&cacheKey, sizeof(cacheKey));
    if (library) {
      auto function = library.newFunction(funcName);
      if (function) return function;
    }
  }

  sm50_error_t err = nullptr;

  SM50_SHADER_COMMON_DATA common;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = (SM50_SHADER_METAL_VERSION)metalVersion;
  common.next = nullptr;

  SM50_SHADER_DXSO_FOG_DATA fogData;
  fogData.type = SM50_SHADER_DXSO_FOG;
  fogData.fog_mode = fogMode;
  fogData.next = &common;

  SM50_SHADER_DXSO_ALPHA_TEST_DATA alphaTest;
  alphaTest.type = SM50_SHADER_DXSO_ALPHA_TEST;
  alphaTest.alpha_test_func = alphaTestFunc;
  alphaTest.next = fogMode ? (void *)&fogData : (void *)&common;

  SM50_SHADER_COMPILATION_ARGUMENT_DATA *firstArg;
  if (alphaTestFunc != 0) {
    firstArg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&alphaTest;
  } else if (fogMode) {
    firstArg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&fogData;
  } else {
    firstArg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common;
  }

  sm50_bitcode_t bitcode = nullptr;
  if (DXSOCompile(shader, firstArg, funcName, &bitcode, &err)) {
    Logger::err("D3D9: DXSOCompile failed for pixel shader");
    if (err) { SM50FreeError(err); }
    return {};
  }

  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);

  auto lib_data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  WMT::Reference<WMT::Error> mtl_err;
  auto library = device.newLibrary(lib_data, mtl_err);

  SM50DestroyBitcode(bitcode);

  if (mtl_err || !library) {
    Logger::err("D3D9: Failed to create MTLLibrary for pixel shader");
    if (mtl_err) Logger::err(mtl_err.description().getUTF8String());
    return {};
  }

  auto function = library.newFunction(funcName);
  if (!function) {
    Logger::err("D3D9: Failed to find MTLFunction for pixel shader");
    return {};
  }

  // Store in cache
  {
    auto writer = cache.getWriter();
    if (writer) writer->set(cacheKey, lib_data);
  }
  ShaderCache::recordShader(start_ns);

  return function;
}

// Map D3D9 decl type to Metal attribute format
static uint32_t D3D9DeclTypeToAttributeFormat(D3DDECLTYPE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:    return WMTAttributeFormatFloat;
  case D3DDECLTYPE_FLOAT2:    return WMTAttributeFormatFloat2;
  case D3DDECLTYPE_FLOAT3:    return WMTAttributeFormatFloat3;
  case D3DDECLTYPE_FLOAT4:    return WMTAttributeFormatFloat4;
  case D3DDECLTYPE_D3DCOLOR:  return WMTAttributeFormatUChar4Normalized_BGRA;
  case D3DDECLTYPE_UBYTE4:    return WMTAttributeFormatUChar4;
  case D3DDECLTYPE_SHORT2:    return WMTAttributeFormatShort2;
  case D3DDECLTYPE_SHORT4:    return WMTAttributeFormatShort4;
  case D3DDECLTYPE_UBYTE4N:   return WMTAttributeFormatUChar4Normalized;
  case D3DDECLTYPE_SHORT2N:   return WMTAttributeFormatShort2Normalized;
  case D3DDECLTYPE_SHORT4N:   return WMTAttributeFormatShort4Normalized;
  case D3DDECLTYPE_USHORT2N:  return WMTAttributeFormatUShort2Normalized;
  case D3DDECLTYPE_USHORT4N:  return WMTAttributeFormatUShort4Normalized;
  case D3DDECLTYPE_FLOAT16_2: return WMTAttributeFormatHalf2;
  case D3DDECLTYPE_FLOAT16_4: return WMTAttributeFormatHalf4;
  default: return 0;
  }
}

// Compile a DXSO vertex shader with input layout from vertex declaration
static WMT::Reference<WMT::Function> CompileDxsoVertexShaderWithLayout(
    WMT::Device device, D3D9VertexShader *vs, D3D9VertexDeclaration *vdecl,
    const char *funcName, WMTMetalVersion metalVersion) {
  auto start_ns = ShaderCache::nowNs();

  // Query the shader's input declarations to map vertex elements by usage/usageIndex
  uint32_t numInputDecls = DXSOGetInputDeclCount(vs->handle());
  std::vector<DXSO_INPUT_DECL> inputDecls(numInputDecls);
  if (numInputDecls > 0)
    DXSOGetInputDecls(vs->handle(), inputDecls.data());

  // Build SM50_IA_INPUT_ELEMENT array by matching D3D9 vertex elements to DXSO input DCLs
  auto &elements = vdecl->elements();
  std::vector<SM50_IA_INPUT_ELEMENT> ia_elements;
  // The vertex-buffer table is compacted by stream mask. BuildDrawCapture fills it using the
  // declaration's full mask, so the shader must use the same mask (not only the streams it reads),
  // otherwise later streams shift by one entry and the GPU reads the wrong buffer / past the table.
  uint32_t slot_mask = vdecl->slotMask();

  for (auto &elem : elements) {
    if (elem.Stream == 0xFF) break;

    uint32_t format = D3D9DeclTypeToAttributeFormat((D3DDECLTYPE)elem.Type);
    if (!format) continue;

    // Find the shader input DCL that matches this vertex element's usage/usageIndex
    for (auto &dcl : inputDecls) {
      if (dcl.usage == elem.Usage && dcl.usageIndex == elem.UsageIndex) {
        SM50_IA_INPUT_ELEMENT ia_elem = {};
        ia_elem.reg = dcl.reg;
        ia_elem.slot = elem.Stream;
        ia_elem.aligned_byte_offset = elem.Offset;
        ia_elem.format = format;
        ia_elem.step_function = 0;
        ia_elem.step_rate = 1;
        ia_elements.push_back(ia_elem);
        break;
      }
    }
  }

  SM50_SHADER_COMMON_DATA common;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = (SM50_SHADER_METAL_VERSION)metalVersion;
  common.next = nullptr;

  // Cache key: shader SHA1 + layout elements
  Sha1HashState keyHash;
  keyHash.update(vs->sha1());
  keyHash.update(ia_elements.data(), ia_elements.size() * sizeof(SM50_IA_INPUT_ELEMENT));
  keyHash.update(&slot_mask, sizeof(slot_mask));
  auto cacheKey = keyHash.final();

  auto &cache = ShaderCache::getInstance(metalVersion);

  // Check preloaded libraries
  {
    auto library = cache.findPreloadedLibrary(&cacheKey, sizeof(cacheKey));
    if (library) {
      auto function = library.newFunction(funcName);
      if (function) return function;
    }
  }

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout;
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.next = &common;
  ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia_layout.slot_mask = slot_mask;
  ia_layout.num_elements = (uint32_t)ia_elements.size();
  ia_layout.elements = ia_elements.data();

  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t err = nullptr;
  if (DXSOCompile(vs->handle(), (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ia_layout, funcName, &bitcode, &err)) {
    Logger::err("D3D9: DXSOCompile failed for vertex shader with input layout");
    if (err) { SM50FreeError(err); }
    return {};
  }

  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);

  auto lib_data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  WMT::Reference<WMT::Error> mtl_err;
  auto library = device.newLibrary(lib_data, mtl_err);

  SM50DestroyBitcode(bitcode);

  if (mtl_err || !library) {
    Logger::err("D3D9: Failed to create MTLLibrary for vertex shader");
    return {};
  }

  auto function = library.newFunction(funcName);
  if (!function) {
    Logger::err("D3D9: Failed to create MTLFunction for vertex shader");
    return {};
  }

  // Store in cache
  {
    auto writer = cache.getWriter();
    if (writer) writer->set(cacheKey, lib_data);
  }
  ShaderCache::recordShader(start_ns);

  return function;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  if (!pFunction || !ppShader) return D3DERR_INVALIDCALL;

  // Scan for END token to determine DXSO bytecode size
  const uint32_t *p = (const uint32_t *)pFunction;
  while ((*p & 0xffff) != 0xffff) p++;
  p++; // include END token
  size_t bytecodeSize = (size_t)((const uint8_t *)p - (const uint8_t *)pFunction);

  dxso_shader_t shader = nullptr;
  sm50_error_t err = nullptr;

  if (DXSOInitialize(pFunction, bytecodeSize, &shader, &err)) {
    Logger::err("D3D9: DXSOInitialize failed for vertex shader");
    if (err) { SM50FreeError(err); }
    return D3DERR_INVALIDCALL;
  }

  std::vector<uint8_t> bytecodeVec((const uint8_t *)pFunction, (const uint8_t *)pFunction + bytecodeSize);
  *ppShader = ref(new D3D9VertexShader(shader, std::move(bytecodeVec)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  auto *vs = static_cast<D3D9VertexShader *>(pShader);
  if (current_vs_.ptr() == vs) return S_OK;
  current_vs_ = vs;
  pso_dirty_ = true;
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = current_vs_.ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  if (!pFunction || !ppShader) return D3DERR_INVALIDCALL;

  // Scan for END token
  const uint32_t *p = (const uint32_t *)pFunction;
  while ((*p & 0xffff) != 0xffff) p++;
  p++;
  size_t bytecodeSize = (size_t)((const uint8_t *)p - (const uint8_t *)pFunction);

  dxso_shader_t shader = nullptr;
  sm50_error_t err = nullptr;

  if (DXSOInitialize(pFunction, bytecodeSize, &shader, &err)) {
    Logger::err("D3D9: DXSOInitialize failed for pixel shader");
    if (err) { SM50FreeError(err); }
    return D3DERR_INVALIDCALL;
  }

  uint8_t maxTexcoordCount = (uint8_t)DXSOGetPSMaxTexcoordCount(shader);
  auto sha1 = Sha1HashState::compute(pFunction, bytecodeSize);

  auto function = CompileDxsoPixelShader(dxmt_device_->device(), shader, sha1,
                                          "shader_main", dxmt_device_->metalVersion());
  if (!function) {
    DXSODestroy(shader);
    return D3DERR_INVALIDCALL;
  }

  *ppShader = ref(new D3D9PixelShader(shader, std::move(function), maxTexcoordCount, sha1));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  auto *ps = static_cast<D3D9PixelShader *>(pShader);
  if (current_ps_.ptr() == ps) return S_OK;
  current_ps_ = ps;
  pso_dirty_ = true;
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = current_ps_.ref();
  return S_OK;
}

// Shader constants
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantF(
    UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(&vsConstants_[StartRegister], pConstantData, Vector4fCount * 4 * sizeof(float));
  vs_const_version_++;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantF(
    UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(&psConstants_[StartRegister], pConstantData, Vector4fCount * 4 * sizeof(float));
  ps_const_version_++;
  return S_OK;
}

// Stream source
HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) {
  if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
  auto *vb = static_cast<D3D9VertexBuffer *>(pStreamData);
  if (stream_sources_[StreamNumber].ptr() == vb &&
      stream_offsets_[StreamNumber] == OffsetInBytes &&
      stream_strides_[StreamNumber] == Stride)
    return S_OK;
  stream_sources_[StreamNumber] = vb;
  stream_offsets_[StreamNumber] = OffsetInBytes;
  stream_strides_[StreamNumber] = Stride;
  vb_dirty_ = true;
  return S_OK;
}

// Queries
HRESULT STDMETHODCALLTYPE D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
  if (Type != D3DQUERYTYPE_EVENT && Type != D3DQUERYTYPE_OCCLUSION)
    return D3DERR_NOTAVAILABLE;
  if (!ppQuery) return S_OK; // Just checking support
  *ppQuery = ref(new D3D9Query(Type, this));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Query::Issue(DWORD dwIssueFlags) {
  if (type_ == D3DQUERYTYPE_OCCLUSION) {
    if (dwIssueFlags & D3DISSUE_BEGIN) {
      query_ = new VisibilityResultQuery();
      device_->BeginOcclusionQuery(query_);
      building_ = true;
      issued_ = false;
    }
    if (dwIssueFlags & D3DISSUE_END) {
      if (building_) {
        device_->EndOcclusionQuery(query_);
        building_ = false;
        issued_ = true;
      }
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  if (type_ == D3DQUERYTYPE_EVENT) {
    if (pData) {
      BOOL result = TRUE;
      memcpy(pData, &result, std::min(dwSize, (DWORD)sizeof(BOOL)));
    }
    return S_OK;
  }

  if (type_ == D3DQUERYTYPE_OCCLUSION) {
    if (!issued_) {
      // Never issued — return immediately with dummy result (same as old stub)
      if (pData) {
        DWORD result = 0;
        memcpy(pData, &result, std::min(dwSize, (DWORD)sizeof(DWORD)));
      }
      return S_OK;
    }
    uint64_t value;
    if (query_->getValue(&value)) {
      if (pData) {
        DWORD result = (DWORD)std::min(value, (uint64_t)UINT32_MAX);
        memcpy(pData, &result, std::min(dwSize, (DWORD)sizeof(DWORD)));
      }
      return S_OK;
    }
    if (dwGetDataFlags & D3DGETDATA_FLUSH)
      device_->FlushForQuery();
    return S_FALSE;
  }

  return S_OK;
}

// D3D9 blend factor → Metal blend factor
static WMTBlendFactor ConvertBlendFactor(DWORD d3dBlend) {
  switch (d3dBlend) {
  case D3DBLEND_ZERO:          return WMTBlendFactorZero;
  case D3DBLEND_ONE:           return WMTBlendFactorOne;
  case D3DBLEND_SRCCOLOR:      return WMTBlendFactorSourceColor;
  case D3DBLEND_INVSRCCOLOR:   return WMTBlendFactorOneMinusSourceColor;
  case D3DBLEND_SRCALPHA:      return WMTBlendFactorSourceAlpha;
  case D3DBLEND_INVSRCALPHA:   return WMTBlendFactorOneMinusSourceAlpha;
  case D3DBLEND_DESTALPHA:     return WMTBlendFactorDestinationAlpha;
  case D3DBLEND_INVDESTALPHA:  return WMTBlendFactorOneMinusDestinationAlpha;
  case D3DBLEND_DESTCOLOR:     return WMTBlendFactorDestinationColor;
  case D3DBLEND_INVDESTCOLOR:  return WMTBlendFactorOneMinusDestinationColor;
  case D3DBLEND_SRCALPHASAT:   return WMTBlendFactorSourceAlphaSaturated;
  case D3DBLEND_BLENDFACTOR:   return WMTBlendFactorBlendColor;
  case D3DBLEND_INVBLENDFACTOR:return WMTBlendFactorOneMinusBlendColor;
  default:                     return WMTBlendFactorOne;
  }
}

static WMTBlendOperation ConvertBlendOp(DWORD d3dOp) {
  switch (d3dOp) {
  case D3DBLENDOP_ADD:         return WMTBlendOperationAdd;
  case D3DBLENDOP_SUBTRACT:    return WMTBlendOperationSubtract;
  case D3DBLENDOP_REVSUBTRACT: return WMTBlendOperationReverseSubtract;
  case D3DBLENDOP_MIN:         return WMTBlendOperationMin;
  case D3DBLENDOP_MAX:         return WMTBlendOperationMax;
  default:                     return WMTBlendOperationAdd;
  }
}

// PSO creation — compiles vertex shader with current vertex declaration's input layout
obj_handle_t D3D9Device::CreatePSO() {
  if (!current_vdecl_) return 0;

  // Fast path: reuse cached PSO if no relevant state changed
  if (!pso_dirty_ && cached_pso_)
    return cached_pso_;


  // Lazy-evaluated FF key builders (avoid redundant BuildFFVSKey/BuildFFPSKey calls)
  FFVSKey ffvskey_val;
  FFPSKey ffpskey_val;
  bool have_ffvs = false, have_ffps = false;
  auto getFFVSKey = [&]() -> FFVSKey& {
    if (!have_ffvs) { ffvskey_val = BuildFFVSKey(); have_ffvs = true; }
    return ffvskey_val;
  };
  auto getFFPSKey = [&]() -> FFPSKey& {
    if (!have_ffps) { ffpskey_val = BuildFFPSKey(); have_ffps = true; }
    return ffpskey_val;
  };

  // Resolve VS function handle (avoid WMT::Reference retain/release on hot path)
  obj_handle_t vs_handle = 0;
  if (current_vs_) {
    auto existing = current_vs_->getLayoutVariant(current_vdecl_.ptr());
    if (existing) {
      vs_handle = existing.handle;
    } else {
      auto vs_func = CompileDxsoVertexShaderWithLayout(
          dxmt_device_->device(), current_vs_.ptr(), current_vdecl_.ptr(),
          "shader_main", dxmt_device_->metalVersion());
      if (vs_func)
        current_vs_->addLayoutVariant(current_vdecl_.ptr(), vs_func);
      vs_handle = vs_func.handle;
    }
  } else {
    auto &ffkey = getFFVSKey();
    if (current_ps_) {
      // Custom PS: bump VS texcoord count to satisfy PS inputs
      if (current_ps_->maxTexcoordCount() > ffkey.tex_coord_count)
        ffkey.tex_coord_count = current_ps_->maxTexcoordCount();
    } else {
      // FF PS: bump VS texcoord count if any PS stage uses a higher texcoord_index
      auto &ffpskey = getFFPSKey();
      for (int i = 0; i < 8; i++) {
        if (ffpskey.stages[i].color_op == D3DTOP_DISABLE)
          break;
        if (ffpskey.stages[i].has_texture) {
          uint8_t need = ffpskey.stages[i].texcoord_index + 1;
          if (need > ffkey.tex_coord_count)
            ffkey.tex_coord_count = need;
        }
      }
    }
    // (Re-)populate TCI/TTF modes for the final tex_coord_count: a bumped count (e.g. GTA SA vehicle env map
    // on stage 1 with a single UV set) otherwise left stage 1 as passthru of a non-existent coordinate.
    FillFFVSTexcoordModes(ffkey);
    auto ff_vs = GetOrCreateFFVS(ffkey, current_vdecl_.ptr());
    vs_handle = ff_vs.handle;
    if (!vs_handle) {
      Logger::err("D3D9: FF VS compilation failed");
    }
  }
  if (!vs_handle) return 0;

  // Determine alpha test and fog state for custom PS
  uint8_t alphaFunc = 0;
  if (current_ps_ && render_states_[D3DRS_ALPHATESTENABLE] &&
      render_states_[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS) {
    alphaFunc = (uint8_t)render_states_[D3DRS_ALPHAFUNC];
  }
  // Determine fog mode for custom PS:
  // 0=none, 1=vertex (FOG0 from VS), 2=table EXP, 3=table EXP2, 4=table LINEAR
  uint8_t psFogMode = 0;
  if (current_ps_ && render_states_[D3DRS_FOGENABLE]) {
    if (current_vs_) {
      // Custom VS: use vertex fog if VS outputs FOG0, else table fog from depth
      if (DXSOGetVSHasFogOutput(current_vs_->handle())) {
        psFogMode = 1; // vertex fog
      } else {
        // Fall back to table fog computed in PS from fragment depth
        DWORD tableFog = render_states_[D3DRS_FOGTABLEMODE];
        switch (tableFog) {
        case D3DFOG_EXP:    psFogMode = 2; break;
        case D3DFOG_EXP2:   psFogMode = 3; break;
        case D3DFOG_LINEAR: psFogMode = 4; break;
        }
      }
    } else {
      // FF VS: outputs FOG0 when effective fog mode != NONE
      DWORD effFog = render_states_[D3DRS_FOGVERTEXMODE];
      if (effFog == D3DFOG_NONE)
        effFog = render_states_[D3DRS_FOGTABLEMODE];
      if (effFog != D3DFOG_NONE)
        psFogMode = 1; // vertex fog (FF VS computes it)
    }
  }

  // Resolve PS function handle (avoid WMT::Reference retain/release on hot path)
  obj_handle_t ps_handle = 0;
  if (current_ps_) {
    uint8_t variantKey = D3D9PixelShader::makeVariantKey(alphaFunc, psFogMode);
    auto existing = current_ps_->getVariant(variantKey);
    if (existing) {
      ps_handle = existing.handle;
    } else {
      // Compile new variant with alpha test and fog baked in
      char funcName[64];
      snprintf(funcName, sizeof(funcName), "shader_main_v%u", variantKey);
      auto ps_func = CompileDxsoPixelShader(
          dxmt_device_->device(), current_ps_->handle(), current_ps_->sha1(),
          funcName, dxmt_device_->metalVersion(), alphaFunc, psFogMode);
      if (ps_func)
        current_ps_->addVariant(variantKey, ps_func);
      ps_handle = ps_func.handle;
    }
    if (!ps_handle) Logger::err("D3D9: PS function is null");
  } else {
    auto &ffkey = getFFPSKey();
    // Sync PS fog_enable with VS fog output to prevent stage-in mismatch
    if (current_vs_ && ffkey.fog_enable) {
      if (!DXSOGetVSHasFogOutput(current_vs_->handle()))
        ffkey.fog_enable = 0;
    }
    // SM < 3 vertex shader + vertex fog (no table fog): D3D9 treats oFog as a fog coordinate and applies the
    // FOGVERTEXMODE formula (GTA SA's RW skin shader writes (depth-start)/(end-start); used raw as a factor it
    // fully fogged every ped).
    ffkey.fog_coord_mode = 0;
    if (current_vs_ && ffkey.fog_enable && render_states_[D3DRS_FOGTABLEMODE] == D3DFOG_NONE &&
        render_states_[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE) {
      uint32_t ver = current_vs_->bytecodeSize() >= 4 ? *(const uint32_t *)current_vs_->bytecode() : 0;
      if (((ver >> 8) & 0xFF) < 3)
        ffkey.fog_coord_mode = (uint8_t)render_states_[D3DRS_FOGVERTEXMODE];
    }
    // Compute effective texcoord count: max of vdecl count and PS stage needs.
    // This ensures the PS declares the same TEXCOORD inputs the VS outputs.
    {
      auto &vsKey = getFFVSKey();
      // FF VS only outputs FOG0 when fog_mode != 0; sync PS to match
      if (!current_vs_ && vsKey.fog_mode == 0)
        ffkey.fog_enable = 0;
      uint8_t effective = vsKey.tex_coord_count;
      for (int i = 0; i < 8; i++) {
        if (ffkey.stages[i].color_op == D3DTOP_DISABLE)
          break;
        if (ffkey.stages[i].has_texture) {
          uint8_t need = ffkey.stages[i].texcoord_index + 1;
          if (need > effective) effective = need;
        }
      }
      ffkey.tex_coord_count = effective;
    }
    auto ff_ps = GetOrCreateFFPS(ffkey);
    ps_handle = ff_ps.handle;
    if (!ps_handle) {
      Logger::err("D3D9: FF PS compilation failed");
    }
  }
  if (!ps_handle) return 0;

  DWORD blendEnable = render_states_[D3DRS_ALPHABLENDENABLE];
  DWORD srcBlend = render_states_[D3DRS_SRCBLEND];
  DWORD destBlend = render_states_[D3DRS_DESTBLEND];
  DWORD blendOp = render_states_[D3DRS_BLENDOP];
  DWORD srcBlendAlpha = render_states_[D3DRS_SRCBLENDALPHA];
  DWORD destBlendAlpha = render_states_[D3DRS_DESTBLENDALPHA];
  DWORD blendOpAlpha = render_states_[D3DRS_BLENDOPALPHA];
  DWORD depthFmt = depth_stencil_ ? (DWORD)depth_stencil_format_ : 0;
  bool srgbWrite = render_states_[D3DRS_SRGBWRITEENABLE] != 0;
  uint8_t colorWriteMask = (uint8_t)(render_states_[D3DRS_COLORWRITEENABLE] & 0xF);

  PSOKey key = {vs_handle, ps_handle, current_vdecl_.ptr(),
                blendEnable, srcBlend, destBlend, blendOp, srcBlendAlpha,
                destBlendAlpha, blendOpAlpha, depthFmt, alphaFunc,
                srgbWrite, colorWriteMask};
  if (key == cached_pso_key_) {
    pso_dirty_ = false;
    return cached_pso_;
  }
  auto it = pso_cache_.find(key);
  if (it != pso_cache_.end()) {
    cached_pso_ = it->second.handle;
    cached_pso_key_ = key;
    pso_dirty_ = false;
    return cached_pso_;
  }

  WMTRenderPipelineInfo pipeline_info;
  WMT::InitializeRenderPipelineInfo(pipeline_info);

  pipeline_info.vertex_function = vs_handle;
  pipeline_info.fragment_function = ps_handle;
  // Use sRGB format variant when sRGB write is enabled and RT is BGRA8
  if (srgbWrite && current_rt_format_ == WMTPixelFormatBGRA8Unorm)
    pipeline_info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm_sRGB;
  else
    pipeline_info.colors[0].pixel_format = current_rt_format_;

  // Color write mask: D3D9 (R=1,G=2,B=4,A=8) → Metal (R=8,G=4,B=2,A=1)
  {
    uint8_t mtlMask = 0;
    if (colorWriteMask & 0x1) mtlMask |= WMTColorWriteMaskRed;
    if (colorWriteMask & 0x2) mtlMask |= WMTColorWriteMaskGreen;
    if (colorWriteMask & 0x4) mtlMask |= WMTColorWriteMaskBlue;
    if (colorWriteMask & 0x8) mtlMask |= WMTColorWriteMaskAlpha;
    pipeline_info.colors[0].write_mask = (WMTColorWriteMask)mtlMask;
  }

  pipeline_info.rasterization_enabled = true;
  pipeline_info.raster_sample_count = 1;
  // Leave input_primitive_topology Unspecified: the same PSO is used for lines/points too.

  // Blend state
  if (blendEnable) {
    pipeline_info.colors[0].blending_enabled = true;
    pipeline_info.colors[0].src_rgb_blend_factor = ConvertBlendFactor(srcBlend);
    pipeline_info.colors[0].dst_rgb_blend_factor = ConvertBlendFactor(destBlend);
    pipeline_info.colors[0].rgb_blend_operation = ConvertBlendOp(render_states_[D3DRS_BLENDOP]);
    pipeline_info.colors[0].src_alpha_blend_factor = ConvertBlendFactor(render_states_[D3DRS_SRCBLENDALPHA]);
    pipeline_info.colors[0].dst_alpha_blend_factor = ConvertBlendFactor(render_states_[D3DRS_DESTBLENDALPHA]);
    pipeline_info.colors[0].alpha_blend_operation = ConvertBlendOp(render_states_[D3DRS_BLENDOPALPHA]);
  }

  // Depth format
  if (depthFmt) {
    pipeline_info.depth_pixel_format = (WMTPixelFormat)depthFmt;
    // Set stencil format for combined depth+stencil formats
    if (depthFmt == (DWORD)WMTPixelFormatDepth32Float_Stencil8)
      pipeline_info.stencil_pixel_format = WMTPixelFormatDepth32Float_Stencil8;
  }

  WMT::Reference<WMT::Error> err;
  auto pso = dxmt_device_->device().newRenderPipelineState(pipeline_info, err);

  if (err || !pso) {
    Logger::err("D3D9: Failed to create render pipeline state");
    if (err) {
      auto desc = err.description();
      if (desc) Logger::err(desc.getUTF8String());
    }
    return {};
  }

#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_pso_miss_count++;
#endif
  pso_cache_.emplace(key, pso);
  cached_pso_ = pso.handle;
  pso_dirty_ = false;
  return pso.handle;
}

// D3D compare func → Metal compare func
static WMTCompareFunction ConvertCompareFunc(DWORD d3dFunc) {
  switch (d3dFunc) {
  case D3DCMP_NEVER:        return WMTCompareFunctionNever;
  case D3DCMP_LESS:         return WMTCompareFunctionLess;
  case D3DCMP_EQUAL:        return WMTCompareFunctionEqual;
  case D3DCMP_LESSEQUAL:    return WMTCompareFunctionLessEqual;
  case D3DCMP_GREATER:      return WMTCompareFunctionGreater;
  case D3DCMP_NOTEQUAL:     return WMTCompareFunctionNotEqual;
  case D3DCMP_GREATEREQUAL: return WMTCompareFunctionGreaterEqual;
  case D3DCMP_ALWAYS:       return WMTCompareFunctionAlways;
  default:                  return WMTCompareFunctionLessEqual;
  }
}

// D3D9 sampler state → WMTSamplerInfo
static WMTSamplerInfo ConvertD3D9SamplerState(const DWORD state[14]) {
  WMTSamplerInfo info = {};

  // Filter
  DWORD minFilter = state[D3DSAMP_MINFILTER];
  DWORD magFilter = state[D3DSAMP_MAGFILTER];
  DWORD mipFilter = state[D3DSAMP_MIPFILTER];
  info.min_filter = (minFilter == D3DTEXF_POINT) ? WMTSamplerMinMagFilterNearest : WMTSamplerMinMagFilterLinear;
  info.mag_filter = (magFilter == D3DTEXF_POINT) ? WMTSamplerMinMagFilterNearest : WMTSamplerMinMagFilterLinear;

  // Mip filter
  switch (mipFilter) {
  case D3DTEXF_NONE:
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
    break;
  case D3DTEXF_POINT:
    info.mip_filter = WMTSamplerMipFilterNearest;
    break;
  case D3DTEXF_LINEAR:
    info.mip_filter = WMTSamplerMipFilterLinear;
    break;
  default:
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
    break;
  }

  // Address modes
  auto convertAddr = [](DWORD mode) -> WMTSamplerAddressMode {
    switch (mode) {
    case D3DTADDRESS_WRAP:   return WMTSamplerAddressModeRepeat;
    case D3DTADDRESS_MIRROR: return WMTSamplerAddressModeMirrorRepeat;
    case D3DTADDRESS_CLAMP:  return WMTSamplerAddressModeClampToEdge;
    case D3DTADDRESS_BORDER: return WMTSamplerAddressModeClampToZero;
    default:                 return WMTSamplerAddressModeRepeat;
    }
  };
  info.s_address_mode = convertAddr(state[D3DSAMP_ADDRESSU]);
  info.t_address_mode = convertAddr(state[D3DSAMP_ADDRESSV]);
  info.r_address_mode = convertAddr(state[D3DSAMP_ADDRESSU]); // W not commonly used for 2D

  info.normalized_coords = true;
  info.lod_min_clamp = 0.0f;
  info.lod_max_clamp = 1000.0f;
  DWORD maxAniso = state[D3DSAMP_MAXANISOTROPY];
  info.max_anisotroy = (maxAniso > 1) ? maxAniso : 1;
  info.support_argument_buffers = false;
  return info;
}

// D3D stencil op → Metal stencil op
static WMTStencilOperation ConvertStencilOp(DWORD d3dOp) {
  switch (d3dOp) {
  case D3DSTENCILOP_KEEP:    return WMTStencilOperationKeep;
  case D3DSTENCILOP_ZERO:    return WMTStencilOperationZero;
  case D3DSTENCILOP_REPLACE: return WMTStencilOperationReplace;
  case D3DSTENCILOP_INCRSAT: return WMTStencilOperationIncrementClamp;
  case D3DSTENCILOP_DECRSAT: return WMTStencilOperationDecrementClamp;
  case D3DSTENCILOP_INVERT:  return WMTStencilOperationInvert;
  case D3DSTENCILOP_INCR:    return WMTStencilOperationIncrementWrap;
  case D3DSTENCILOP_DECR:    return WMTStencilOperationDecrementWrap;
  default:                   return WMTStencilOperationKeep;
  }
}

// D3D cull mode → Metal cull mode
static WMTCullMode ConvertCullMode(DWORD d3dCull) {
  switch (d3dCull) {
  case D3DCULL_NONE: return WMTCullModeNone;
  case D3DCULL_CW:   return WMTCullModeFront; // D3D CW cull = Metal front cull (D3D is left-handed)
  case D3DCULL_CCW:  return WMTCullModeBack;
  default:           return WMTCullModeBack;
  }
}

// Emit common render pass setup (shared between Draw and DrawIndexed)
// When prev is non-null, redundant state commands are skipped.
// prev_vs_const_off / prev_ps_const_off are the argbuf offsets of the previous draw's
// constant regions — used to reuse them when constant versions match.
using DrawCapture = D3D9Device::DrawCapture;
struct EmitState {
  uint64_t vs_const_off = 0;
  uint64_t ps_const_off = 0;
};
static void EmitCommonRenderSetup(ArgumentEncodingContext &ctx, DrawCapture &cap,
                                   DrawCapture *prev, EmitState &state,
                                   bool start_pass = true, uint64_t argbuf_size_override = 0,
                                   uint64_t argbuf_base = 0) {
  if (start_pass) {
    uint64_t alloc_size = argbuf_size_override ? argbuf_size_override : cap.argbuf_size;
    // Attach the bound depth buffer even when ZENABLE is off: the PSO is built with its format, the
    // DSSO disables the test, and later draws in the same batch may enable depth.
    uint8_t dsv_flags = cap.depth_tex
        ? DepthStencilPlanarFlags(cap.depth_tex->pixelFormat()) : 0;
    auto &pass_info = *ctx.startRenderPass(dsv_flags, 0, 1, alloc_size);

    auto &color = pass_info.colors[0];
    color.attachment = ctx.access(cap.backbuffer, cap.view, DXMT_ENCODER_RESOURCE_ACESS_READWRITE);
    color.depth_plane = 0;
    color.load_action = WMTLoadActionLoad;
    color.store_action = WMTStoreActionStore;

    if (cap.depth_tex) {
      pass_info.depth.attachment = ctx.access(cap.depth_tex, cap.depth_view, DXMT_ENCODER_RESOURCE_ACESS_READWRITE);
      pass_info.depth.load_action = WMTLoadActionLoad;
      pass_info.depth.store_action = WMTStoreActionStore;
      if (dsv_flags & 2) {
        pass_info.stencil.attachment = pass_info.depth.attachment;
        pass_info.stencil.load_action = WMTLoadActionLoad;
        pass_info.stencil.store_action = WMTStoreActionStore;
      }
    }

    pass_info.render_target_width = cap.backbuffer->width();
    pass_info.render_target_height = cap.backbuffer->height();
    pass_info.render_target_array_length = 1;
    pass_info.default_raster_sample_count = 1;

    ctx.bumpVisibilityResultOffset();
  }

  // Set PSO — skip if identical to previous draw in batch
  if (!prev || cap.pso != prev->pso) {
    auto &setpso = ctx.encodeRenderCommand<wmtcmd_render_setpso>();
    setpso.type = WMTRenderCommandSetPSO;
    setpso.pso = cap.pso;
  }

  // Depth stencil state — skip if identical to previous draw in batch
  if (!prev || cap.dsso != prev->dsso || cap.stencil_ref != prev->stencil_ref) {
    auto &setdsso = ctx.encodeRenderCommand<wmtcmd_render_setdsso>();
    setdsso.type = WMTRenderCommandSetDSSO;
    setdsso.dsso = cap.dsso;
    setdsso.stencil_ref = cap.stencil_ref;
  }

  // Rasterizer state — skip if cull mode identical to previous draw
  if (!prev || cap.cull_mode != prev->cull_mode) {
    auto &setrast = ctx.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
    setrast.type = WMTRenderCommandSetRasterizerState;
    setrast.fill_mode = WMTTriangleFillModeFill;
    setrast.cull_mode = cap.cull_mode;
    setrast.depth_clip_mode = WMTDepthClipModeClip;
    setrast.winding = WMTWindingClockwise; // D3D default winding
    setrast.depth_bias = 0.0f;
    setrast.scole_scale = 0.0f;
    setrast.depth_bias_clamp = 0.0f;
  }

  // Scissor rect — skip if identical to previous draw
  if (cap.scissor_enable) {
    if (!prev || !prev->scissor_enable || memcmp(&cap.scissor_rect, &prev->scissor_rect, sizeof(RECT)) != 0) {
      auto &setscr = ctx.encodeRenderCommand<wmtcmd_render_setscissorrect>();
      setscr.type = WMTRenderCommandSetScissorRect;
      LONG left = cap.scissor_rect.left < 0 ? 0 : cap.scissor_rect.left;
      LONG top = cap.scissor_rect.top < 0 ? 0 : cap.scissor_rect.top;
      LONG right = cap.scissor_rect.right;
      LONG bottom = cap.scissor_rect.bottom;
      setscr.scissor_rect = {(uint64_t)left, (uint64_t)top,
                             (uint64_t)(right > left ? right - left : 0),
                             (uint64_t)(bottom > top ? bottom - top : 0)};
    }
  }

  // Set viewport — skip if identical to previous draw
  if (!prev || memcmp(&cap.vp, &prev->vp, sizeof(D3DVIEWPORT9)) != 0) {
    auto &setvp = ctx.encodeRenderCommand<wmtcmd_render_setviewport>();
    setvp.type = WMTRenderCommandSetViewport;
    setvp.viewport = {(double)cap.vp.X, (double)cap.vp.Y, (double)cap.vp.Width, (double)cap.vp.Height,
                      (double)cap.vp.MinZ, (double)cap.vp.MaxZ};
  }

  // Write vertex buffer entries into the argument buffer
  if (cap.slot_mask) {
    struct VERTEX_BUFFER_ENTRY {
      uint64_t buffer_handle;
      uint32_t stride;
      uint32_t length;
    };
    auto *entries = ctx.getMappedArgumentBuffer<VERTEX_BUFFER_ENTRY>(argbuf_base);
    uint32_t max_slot = 32 - __builtin_clz(cap.slot_mask);

    for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
      if (!(cap.slot_mask & (1 << slot))) continue;
      auto &vc = cap.vb_captures[slot];
      if (vc.gpu_address) {
        // Transient VB from ring allocator
        entries[index].buffer_handle = vc.gpu_address + vc.offset;
        entries[index].stride = vc.stride;
        entries[index].length = vc.stride ? 0xFFFFFFFF : 0;
        ctx.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(
            vc.raw_buffer, DXMT_RESOURCE_RESIDENCY_VERTEX_READ);
      } else if (vc.buffer) {
        auto valid_length = vc.buffer->length() > vc.offset ? vc.buffer->length() - vc.offset : 0;
        auto [alloc, sub_offset] = ctx.access<true>(vc.buffer, vc.offset, valid_length, DXMT_ENCODER_RESOURCE_ACESS_READ);
        entries[index].buffer_handle = alloc->gpuAddress() + sub_offset + vc.offset;
        entries[index].stride = vc.stride;
        entries[index].length = valid_length;
        ctx.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(vc.buffer.ptr());
      } else {
        entries[index].buffer_handle = 0;
        entries[index].stride = 0;
        entries[index].length = 0;
      }
      index++;
    }

    auto &setoff = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    setoff.type = WMTRenderCommandSetVertexBufferOffset;
    setoff.offset = ctx.getFinalArgumentBufferOffset(argbuf_base);
    setoff.index = 16;
  }

  // Write VS/PS constants — skip copy and reuse previous offset when version matches
  constexpr uint64_t PS_CONST_BUF_SIZE = 256 * 4 * sizeof(float);

  // Reuse only if the previous upload covered every register this draw reads (same constants, but a shader using
  // more registers than the previous one would otherwise read stale argbuf data).
  bool reuse_vs = prev && cap.vs_const_version == prev->vs_const_version &&
                  cap.vs_const_count <= prev->vs_const_count;
  bool reuse_ps = prev && cap.ps_const_version == prev->ps_const_version &&
                  cap.ps_const_count <= prev->ps_const_count;

  if (!reuse_vs) {
    auto *vs_const_dst = ctx.getMappedArgumentBuffer<char>(argbuf_base + cap.vb_region_size);
    uint64_t vs_copy_size = cap.vs_const_count * 4 * sizeof(float);
    memcpy(vs_const_dst, cap.constants->vs, vs_copy_size);
    state.vs_const_off = ctx.getFinalArgumentBufferOffset(argbuf_base + cap.vb_region_size);
  }
  if (!reuse_vs || !prev) {
    auto &setoff_vs = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    setoff_vs.type = WMTRenderCommandSetVertexBufferOffset;
    setoff_vs.offset = state.vs_const_off;
    setoff_vs.index = 17;
  }

  if (!reuse_ps) {
    // PS buffer starts after the right-sized VS buffer
    auto *ps_const_dst = ctx.getMappedArgumentBuffer<char>(argbuf_base + cap.vb_region_size + cap.vs_const_buf_size);
    uint64_t ps_copy_size = cap.ps_const_count * 4 * sizeof(float);
    if (cap.ps_const_count < 254) {
      // Copy shader's own constants, then special registers 254-255 (alpha ref, fog)
      memcpy(ps_const_dst, cap.constants->ps, ps_copy_size);
      memcpy(ps_const_dst + 254 * 4 * sizeof(float),
             cap.constants->ps[254], 2 * 4 * sizeof(float));
    } else {
      memcpy(ps_const_dst, cap.constants->ps, PS_CONST_BUF_SIZE);
    }
    state.ps_const_off = ctx.getFinalArgumentBufferOffset(argbuf_base + cap.vb_region_size + cap.vs_const_buf_size);
  }
  if (!reuse_ps || !prev) {
    auto &setoff_ps = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    setoff_ps.type = WMTRenderCommandSetFragmentBufferOffset;
    setoff_ps.offset = state.ps_const_off;
    setoff_ps.index = 18;
  }

  // Bind textures and samplers — skip commands if identical to previous draw
  bool tex_changed = !prev || cap.tex_sampler_fingerprint != prev->tex_sampler_fingerprint;
  for (uint8_t i = 0; i < cap.texCaptureCount; i++) {
    auto &tc = cap.texCaptures[i];
    auto &view = ctx.access(tc.texture, tc.viewKey, DXMT_ENCODER_RESOURCE_ACESS_READ);
    if (tex_changed) {
      auto &settex = ctx.encodeRenderCommand<wmtcmd_render_settexture>();
      settex.type = WMTRenderCommandSetFragmentTexture;
      settex.texture = view.texture;
      settex.index = 19 + tc.stage * 2;

      auto &setsamp = ctx.encodeRenderCommand<wmtcmd_render_setsamplerstate>();
      setsamp.type = WMTRenderCommandSetFragmentSamplerState;
      setsamp.sampler_state = cap.samplerHandles[tc.stage];
      setsamp.index = tc.stage; // must match shader [[sampler(stage)]]; Metal allows 0..15
    }
    ctx.makeResident<PipelineStage::Pixel, PipelineKind::Ordinary>(tc.texture.ptr(), tc.viewKey);
  }
}

// Encode per-draw state into an already-open render pass (no startRenderPass/endPass)
using BatchedDraw = D3D9Device::BatchedDraw;
// Emit a draw command (non-indexed or indexed)
static void EmitDrawCommand(ArgumentEncodingContext &ctx, BatchedDraw &bd) {
  if (bd.type == BatchedDraw::kNonIndexed) {
    auto &draw = ctx.encodeRenderCommand<wmtcmd_render_draw>();
    draw.type = WMTRenderCommandDraw;
    draw.primitive_type = bd.cap.mtlPrimType;
    draw.vertex_start = bd.vertexStart;
    draw.vertex_count = bd.vertexCount;
    draw.instance_count = 1;
    draw.base_instance = 0;
  } else if (bd.ibRawBuffer != nullptr) {
    // Transient IB from ring allocator
    ctx.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(
        bd.ibRawBuffer, DXMT_RESOURCE_RESIDENCY_VERTEX_READ);
    auto &draw = ctx.encodeRenderCommand<wmtcmd_render_draw_indexed>();
    draw.type = WMTRenderCommandDrawIndexed;
    draw.primitive_type = bd.cap.mtlPrimType;
    draw.index_count = bd.indexCount;
    draw.index_type = bd.indexType;
    draw.index_buffer = bd.ibRawBuffer;
    draw.index_buffer_offset = bd.ibOffset;
    draw.instance_count = 1;
    draw.base_vertex = bd.baseVertex;
    draw.base_instance = 0;
  } else {
    auto [ib_alloc, ib_sub_offset] = ctx.access<true>(bd.ibBuffer, 0, bd.ibBuffer->length(), DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto &draw = ctx.encodeRenderCommand<wmtcmd_render_draw_indexed>();
    draw.type = WMTRenderCommandDrawIndexed;
    draw.primitive_type = bd.cap.mtlPrimType;
    draw.index_count = bd.indexCount;
    draw.index_type = bd.indexType;
    draw.index_buffer = ib_alloc->buffer();
    draw.index_buffer_offset = ib_sub_offset + bd.ibOffset;
    draw.instance_count = 1;
    draw.base_vertex = bd.baseVertex;
    draw.base_instance = 0;
  }
}

void D3D9Device::FlushDrawBatch() {
  if (pending_draws_.empty()) return;

#ifdef DXMT_PERF
  auto perf_t0 = clock::now();
  auto &perf_stats = dxmt_device_->queue().CurrentFrameStatistics();
  perf_stats.d3d9_batch_flush_count++;
#endif

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  chunk->emitcc([draws = std::move(pending_draws_)](ArgumentEncodingContext &ctx) mutable {
    // Compute total argbuf size for the single shared render pass
    uint64_t total_argbuf = 0;
    for (auto &bd : draws)
      total_argbuf += bd.cap.argbuf_size;

    EmitState emit_state;
    uint64_t argbuf_offset = 0;
    for (size_t i = 0; i < draws.size(); i++) {
      auto &bd = draws[i];
      bool first = (i == 0);
      DrawCapture *prev = first ? nullptr : &draws[i - 1].cap;
      EmitCommonRenderSetup(ctx, bd.cap, prev, emit_state, first, total_argbuf, argbuf_offset);
      EmitDrawCommand(ctx, bd);
      argbuf_offset += bd.cap.argbuf_size;
    }
    ctx.endPass();
    // NOTE: do not clear `draws` here: the captured allocations (dynamic VBs/IBs) must stay alive
    // until the chunk completes on the GPU; the lambda is destroyed in chunk.reset() after that.
  });

  pending_draws_.clear();
  batch_rt_ = nullptr;
  batch_depth_ = nullptr;
  batch_argbuf_size_ = 0;

#ifdef DXMT_PERF
  perf_stats.d3d9_flush_batch_time += clock::now() - perf_t0;
#endif
}

void D3D9Device::BeginOcclusionQuery(Rc<VisibilityResultQuery> query) {
  FlushDrawBatch();
  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();
  chunk->emitcc([q = std::move(query)](ArgumentEncodingContext &ctx) mutable {
    ctx.beginVisibilityResultQuery(std::move(q));
  });
}

void D3D9Device::EndOcclusionQuery(Rc<VisibilityResultQuery> query) {
  FlushDrawBatch();
  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();
  chunk->emitcc([q = std::move(query)](ArgumentEncodingContext &ctx) mutable {
    ctx.endVisibilityResultQuery(std::move(q));
  });
  needs_query_flush_ = true;
}

void D3D9Device::FlushForQuery() {
  if (needs_query_flush_) {
    needs_query_flush_ = false;
    auto &queue = dxmt_device_->queue();
    queue.CommitCurrentChunk();
  }
}

void D3D9Device::QueueBatchedDraw(BatchedDraw &&draw) {
  Texture *rt = draw.cap.backbuffer.ptr();
  Texture *depth = draw.cap.depth_tex.ptr();

  // Flush if RT or depth target changed
  if (!pending_draws_.empty() && (rt != batch_rt_ || depth != batch_depth_))
    FlushDrawBatch();

  // Flush if argbuf would exceed allocator block size
  if (batch_argbuf_size_ + draw.cap.argbuf_size > kCommandChunkGPUHeapSize)
    FlushDrawBatch();

  batch_rt_ = rt;
  batch_depth_ = depth;
  batch_argbuf_size_ += draw.cap.argbuf_size;
  pending_draws_.push_back(std::move(draw));
}

// Compute primitive count → vertex/index count and Metal primitive type
static bool GetPrimitiveInfo(D3DPRIMITIVETYPE type, UINT primCount,
                             UINT &outCount, WMTPrimitiveType &outMtl) {
  switch (type) {
  case D3DPT_TRIANGLELIST:  outCount = primCount * 3;  outMtl = WMTPrimitiveTypeTriangle;      return true;
  case D3DPT_TRIANGLESTRIP: outCount = primCount + 2;  outMtl = WMTPrimitiveTypeTriangleStrip;  return true;
  case D3DPT_LINELIST:      outCount = primCount * 2;  outMtl = WMTPrimitiveTypeLine;           return true;
  case D3DPT_LINESTRIP:     outCount = primCount + 1;  outMtl = WMTPrimitiveTypeLineStrip;      return true;
  case D3DPT_POINTLIST:     outCount = primCount;       outMtl = WMTPrimitiveTypePoint;          return true;
  case D3DPT_TRIANGLEFAN:   outCount = primCount * 3;  outMtl = WMTPrimitiveTypeTriangle;       return true;
  default: return false;
  }
}

// Generate triangle fan → triangle list index buffer (transient ring allocation)
static CommandQueue::TransientAllocation GenerateFanIndices(CommandQueue &queue, UINT primCount, UINT startVertex) {
  UINT indexCount = primCount * 3;
  UINT size = indexCount * sizeof(uint16_t);
  auto alloc = queue.AllocateTransientBuffer(size, 2);
  auto *indices = static_cast<uint16_t*>(alloc.cpu_ptr);
  for (UINT i = 0; i < primCount; i++) {
    // Relative indices; the caller passes StartVertex as baseVertex. (Adding startVertex here wrapped
    // around 16 bits for draws late in a large vertex buffer and produced stray triangles.)
    (void)startVertex;
    indices[i * 3 + 0] = 0;
    indices[i * 3 + 1] = (uint16_t)(i + 1);
    indices[i * 3 + 2] = (uint16_t)(i + 2);
  }
  return alloc;
}

// Build a DrawCapture from current device state
DrawCapture D3D9Device::BuildDrawCapture(WMTPrimitiveType mtlPrimType) {
#ifdef DXMT_PERF
  auto &perf_stats = dxmt_device_->queue().CurrentFrameStatistics();
  auto perf_t0 = clock::now();
  auto perf_t = perf_t0;
#endif

  auto pso = CreatePSO();
#ifdef DXMT_PERF
  auto perf_t1 = clock::now();
  perf_stats.d3d9_create_pso_time += perf_t1 - perf_t;
  perf_t = perf_t1;
#endif
  if (!pso) {
    Logger::warn(str::format("D3D9: BuildDrawCapture PSO creation failed, vs=",
        current_vs_ ? "custom" : "FF", " ps=", current_ps_ ? "custom" : "FF",
        " vdecl=", current_vdecl_ ? "yes" : "null",
        " rt_fmt=", (int)current_rt_format_,
        " depth_fmt=", (int)depth_stencil_format_));
  }

  uint32_t slot_mask = current_vdecl_ ? current_vdecl_->slotMask() : 0;
  uint32_t num_vb_slots = __builtin_popcount(slot_mask);

  constexpr uint64_t VB_ENTRY_SIZE = 16;
  constexpr uint64_t PS_CONST_BUF_SIZE = 256 * 4 * sizeof(float); // PS needs full 256 regs (special regs at 254-255)
  // VS constant count is known from shader — right-size to avoid wasting argbuf
  uint16_t vs_const_count = current_vs_ ? (uint16_t)std::min(current_vs_->maxConstantReg(), 256u) : (uint16_t)kFFVSConstCount;
  uint64_t vs_const_buf_size = vs_const_count * 4 * sizeof(float);
  uint64_t vb_region_size = num_vb_slots * VB_ENTRY_SIZE;
  uint64_t argbuf_size = vb_region_size + vs_const_buf_size + PS_CONST_BUF_SIZE;

  DrawCapture cap;
  cap.pso = pso;
  cap.mtlPrimType = mtlPrimType;
  cap.backbuffer = current_rt_;
  // When sRGB write is enabled, use an sRGB view so the render pass format matches the PSO
  if (render_states_[D3DRS_SRGBWRITEENABLE] && current_rt_format_ == WMTPixelFormatBGRA8Unorm) {
    cap.view = current_rt_->checkViewUseFormat(current_rt_view_, WMTPixelFormatBGRA8Unorm_sRGB);
  } else {
    cap.view = current_rt_view_;
  }
  cap.vp = viewport_;
  cap.slot_mask = slot_mask;
  cap.argbuf_size = argbuf_size;
  cap.vb_region_size = vb_region_size;
  cap.vs_const_buf_size = vs_const_buf_size;

  // VB captures — always re-capture dynamic VBs (Lock DISCARD renames allocation),
  // only re-capture static VBs when stream sources changed
  for (uint32_t mask = slot_mask; mask; mask &= mask - 1) {
    unsigned i = __builtin_ctz(mask);
    if (!stream_sources_[i]) continue;
    auto *vb = stream_sources_[i].ptr();
    if (vb->isDynamic()) {
      // Always re-capture dynamic — allocation may have changed via Lock(DISCARD)
      auto *alloc = vb->allocation();
      cap.vb_captures[i].alloc = Rc(alloc);
      cap.vb_captures[i].raw_buffer = alloc->buffer();
      cap.vb_captures[i].gpu_address = alloc->gpuAddress();
    } else {
      // Always re-capture: Lock rename changes the allocation, but SetStreamSource
      // won't set vb_dirty_ (same VB pointer), so shadow_cap_ would go stale.
      auto *alloc = vb->allocation();
      cap.vb_captures[i].buffer = {};
      cap.vb_captures[i].alloc = Rc(alloc);
      cap.vb_captures[i].raw_buffer = alloc->buffer();
      cap.vb_captures[i].gpu_address = alloc->gpuAddress();
    }
    cap.vb_captures[i].offset = stream_offsets_[i];
    cap.vb_captures[i].stride = stream_strides_[i];
  }
  vb_dirty_ = false;
#ifdef DXMT_PERF
  perf_t1 = clock::now();
  perf_stats.d3d9_vb_capture_time += perf_t1 - perf_t;
  perf_t = perf_t1;
#endif

  // Snapshot constants — reuse previous snapshot if versions match
  // Tag FF versions with high bit to prevent collisions with custom shader versions
  static constexpr uint64_t kFFVersionTag = 1ULL << 63;
  cap.vs_const_version = current_vs_ ? vs_const_version_ : (ff_const_version_ | kFFVersionTag);
  cap.ps_const_version = current_ps_ ? ps_const_version_ : (ff_const_version_ | kFFVersionTag);
  cap.vs_const_count = vs_const_count; // already computed for argbuf sizing
  cap.ps_const_count = current_ps_ ? (uint16_t)std::min(current_ps_->maxConstantReg(), 256u) : 3;

  // A snapshot only holds the registers that were copied into it: a shader switch to one reading more registers
  // (same constant version) needs a fresh snapshot too.
  bool need_new_snapshot = (cap.vs_const_version != snapshot_vs_version_) ||
                           (cap.ps_const_version != snapshot_ps_version_) ||
                           cap.vs_const_count > snapshot_vs_count_ || cap.ps_const_count > snapshot_ps_count_;
  if (need_new_snapshot) {
    auto *snap = &snapshot_ring_[snapshot_ring_head_++ % kSnapshotRingSize];
    if (current_vs_) {
      memcpy(snap->vs, vsConstants_, cap.vs_const_count * 4 * sizeof(float));
    } else {
      UpdateFFConstants();
      memcpy(snap->vs, cached_ff_vs_, sizeof(cached_ff_vs_));
    }
    if (current_ps_) {
      memcpy(snap->ps, psConstants_, cap.ps_const_count * 4 * sizeof(float));
      if (render_states_[D3DRS_ALPHATESTENABLE] && render_states_[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS) {
        float alphaRef = (float)(render_states_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
        snap->ps[255][0] = alphaRef;
      }
      if (render_states_[D3DRS_FOGENABLE]) {
        DWORD fc = render_states_[D3DRS_FOGCOLOR];
        snap->ps[255][1] = (float)((fc >> 16) & 0xFF) / 255.0f;
        snap->ps[255][2] = (float)((fc >>  8) & 0xFF) / 255.0f;
        snap->ps[255][3] = (float)((fc >>  0) & 0xFF) / 255.0f;
        float fogStart, fogEnd, fogDensity;
        memcpy(&fogStart, &render_states_[D3DRS_FOGSTART], sizeof(float));
        memcpy(&fogEnd, &render_states_[D3DRS_FOGEND], sizeof(float));
        memcpy(&fogDensity, &render_states_[D3DRS_FOGDENSITY], sizeof(float));
        snap->ps[254][0] = fogStart;
        snap->ps[254][1] = fogEnd;
        snap->ps[254][2] = fogDensity;
      }
    } else {
      UpdateFFConstants();
      memcpy(snap->ps, cached_ff_ps_, 3 * 4 * sizeof(float));
    }
    snapshot_vs_version_ = cap.vs_const_version;
    snapshot_ps_version_ = cap.ps_const_version;
    snapshot_vs_count_ = current_vs_ ? cap.vs_const_count : (uint16_t)kFFVSConstCount;
    snapshot_ps_count_ = current_ps_ ? cap.ps_const_count : (uint16_t)3;
    current_const_snapshot_ = snap;
  }
  cap.constants = current_const_snapshot_;
#ifdef DXMT_PERF
  perf_t1 = clock::now();
  perf_stats.d3d9_constant_snapshot_time += perf_t1 - perf_t;
  perf_t = perf_t1;
#endif

  // Depth state
  bool depthEnabled = render_states_[D3DRS_ZENABLE] != D3DZB_FALSE && depth_stencil_;
  cap.depth_enable = depthEnabled;
  cap.depth_write = render_states_[D3DRS_ZWRITEENABLE] != FALSE;
  cap.depth_func = ConvertCompareFunc(render_states_[D3DRS_ZFUNC]);
  cap.depth_tex = depth_stencil_;
  cap.depth_view = depth_stencil_view_;
  cap.cull_mode = ConvertCullMode(render_states_[D3DRS_CULLMODE]);

  // Create depth stencil state object (cached) — always set to prevent state leaking
  cap.stencil_ref = render_states_[D3DRS_STENCILREF];
  bool stencilEnabled = render_states_[D3DRS_STENCILENABLE] && depth_stencil_;
  if (!dsso_dirty_ && cached_dsso_) {
    cap.dsso = cached_dsso_;
  } else {
    DSKey dsKey = {};
    dsKey.depth_func = depthEnabled ? cap.depth_func : WMTCompareFunctionAlways;
    dsKey.depth_write = depthEnabled ? cap.depth_write : false;
    dsKey.stencil_enabled = stencilEnabled;
    if (stencilEnabled) {
      dsKey.stencil_func = ConvertCompareFunc(render_states_[D3DRS_STENCILFUNC]);
      dsKey.stencil_pass = ConvertStencilOp(render_states_[D3DRS_STENCILPASS]);
      dsKey.stencil_fail = ConvertStencilOp(render_states_[D3DRS_STENCILFAIL]);
      dsKey.depth_fail = ConvertStencilOp(render_states_[D3DRS_STENCILZFAIL]);
      dsKey.stencil_read_mask = (uint8_t)render_states_[D3DRS_STENCILMASK];
      dsKey.stencil_write_mask = (uint8_t)render_states_[D3DRS_STENCILWRITEMASK];
    }
    auto dsIt = ds_cache_.find(dsKey);
    if (dsIt != ds_cache_.end()) {
      cap.dsso = dsIt->second;
    } else {
      WMTDepthStencilInfo dsInfo = {};
      dsInfo.depth_compare_function = dsKey.depth_func;
      dsInfo.depth_write_enabled = dsKey.depth_write;
      if (stencilEnabled) {
        dsInfo.front_stencil.enabled = true;
        dsInfo.front_stencil.stencil_compare_function = dsKey.stencil_func;
        dsInfo.front_stencil.depth_stencil_pass_op = dsKey.stencil_pass;
        dsInfo.front_stencil.stencil_fail_op = dsKey.stencil_fail;
        dsInfo.front_stencil.depth_fail_op = dsKey.depth_fail;
        dsInfo.front_stencil.read_mask = dsKey.stencil_read_mask;
        dsInfo.front_stencil.write_mask = dsKey.stencil_write_mask;
        dsInfo.back_stencil = dsInfo.front_stencil;
      }
      cap.dsso = MTLDevice_newDepthStencilState(dxmt_device_->device().handle, &dsInfo);
      ds_cache_[dsKey] = cap.dsso;
    }
    cached_dsso_ = cap.dsso;
    dsso_dirty_ = false;
  }

  // Scissor rect
  cap.scissor_enable = render_states_[D3DRS_SCISSORTESTENABLE] != FALSE;
  cap.scissor_rect = scissor_rect_;

#ifdef DXMT_PERF
  perf_t1 = clock::now();
  perf_stats.d3d9_depth_state_time += perf_t1 - perf_t;
  perf_t = perf_t1;
#endif
  // Upload dirty texture data via staging buffer + blit command to avoid
  // replaceRegion race (CPU overwrites shared texture while GPU still reads it)
  for (uint32_t mask = tex_bound_mask_; mask; mask &= mask - 1) {
    uint32_t stage = __builtin_ctz(mask);
    auto *tex = bound_textures_[stage].ptr();
    if (tex->isAnyDirty()) {
      auto &queue = dxmt_device_->queue();
      tex->uploadDirtyLevelsStaged(tex->texture(), queue);
    }
  }
#ifdef DXMT_PERF
  perf_t1 = clock::now();
  perf_stats.d3d9_texture_upload_time += perf_t1 - perf_t;
  perf_t = perf_t1;
#endif
  // Texture/sampler capture — only rebuild when bindings or shaders changed
  if (tex_dirty_ || !shadow_valid_) {
    shadow_cap_.texCaptureCount = 0;
    memset(shadow_cap_.samplerHandles, 0, sizeof(shadow_cap_.samplerHandles));

    for (uint32_t mask = tex_bound_mask_; mask; mask &= mask - 1) {
      uint32_t stage = __builtin_ctz(mask);
      auto *tex = bound_textures_[stage].ptr();
      shadow_cap_.texCaptures[shadow_cap_.texCaptureCount++] = {tex->texture(), tex->viewKey(), stage};

      SamplerKey samplerKey;
      memcpy(samplerKey.state, sampler_states_[stage], sizeof(samplerKey.state));
      auto sampIt = sampler_cache_.find(samplerKey);
      if (sampIt != sampler_cache_.end()) {
        shadow_cap_.samplerHandles[stage] = sampIt->second;
      } else {
        auto samplerInfo = ConvertD3D9SamplerState(sampler_states_[stage]);
        shadow_cap_.samplerHandles[stage] = MTLDevice_newSamplerState(dxmt_device_->device().handle, &samplerInfo);
        sampler_cache_[samplerKey] = shadow_cap_.samplerHandles[stage];
      }
    }

    // White texture fallback for unbound sampler slots
    if (current_ps_ && default_white_tex_) {
      uint32_t samplerCount = DXSOGetSamplerDeclCount(current_ps_->handle());
      if (samplerCount > 0) {
        DXSO_SAMPLER_DECL samplerDecls[16];
        DXSOGetSamplerDecls(current_ps_->handle(), samplerDecls);
        for (uint32_t i = 0; i < samplerCount; i++) {
          uint32_t reg = samplerDecls[i].reg;
          if (reg >= 16) continue;
          if (!bound_textures_[reg]) {
            shadow_cap_.texCaptures[shadow_cap_.texCaptureCount++] = {
                default_white_tex_, default_white_view_, reg};
            shadow_cap_.samplerHandles[reg] = default_sampler_;
          }
        }
      }
    }
    if (!current_ps_) {
      for (uint32_t stage = 0; stage < 8; stage++) {
        if (texture_stage_states_[stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
          break;
        if (!bound_textures_[stage]) {
          shadow_cap_.texCaptures[shadow_cap_.texCaptureCount++] = {
              default_white_tex_, default_white_view_, stage};
          shadow_cap_.samplerHandles[stage] = default_sampler_;
        }
      }
    }
    tex_dirty_ = false;

    // Compute fingerprint while shadow is freshly built
    uint64_t h = shadow_cap_.texCaptureCount;
    for (uint8_t i = 0; i < shadow_cap_.texCaptureCount; i++) {
      h ^= reinterpret_cast<uint64_t>(shadow_cap_.texCaptures[i].texture.ptr()) * 0x9E3779B97F4A7C15ULL;
      h ^= (uint64_t)shadow_cap_.texCaptures[i].viewKey * 0x517CC1B727220A95ULL;
      h ^= (uint64_t)shadow_cap_.texCaptures[i].stage << 48;
      h ^= (uint64_t)shadow_cap_.samplerHandles[shadow_cap_.texCaptures[i].stage] * 0x6C62272E07BB0142ULL;
    }
    shadow_cap_.tex_sampler_fingerprint = h;
  }
  // Copy from shadow (Rc<Texture> keeps each texture alive until this capture has been encoded/executed)
  for (uint8_t i = 0; i < shadow_cap_.texCaptureCount; i++)
    cap.texCaptures[i] = shadow_cap_.texCaptures[i];
  cap.texCaptureCount = shadow_cap_.texCaptureCount;
  memcpy(cap.samplerHandles, shadow_cap_.samplerHandles, sizeof(cap.samplerHandles));
  cap.tex_sampler_fingerprint = shadow_cap_.tex_sampler_fingerprint;

  shadow_valid_ = true;

#ifdef DXMT_PERF
  perf_t1 = clock::now();
  perf_stats.d3d9_texture_bind_time += perf_t1 - perf_t;
  perf_stats.d3d9_build_capture_time += perf_t1 - perf_t0;
#endif
  return cap;
}


// DrawPrimitive
HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_count++;
#endif
  if (!current_vdecl_) return D3DERR_INVALIDCALL;

  UINT vertexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, vertexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  // Triangle fan: generate synthetic index buffer
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto &queue = dxmt_device_->queue();
    auto fanIB = GenerateFanIndices(queue, PrimitiveCount, StartVertex);
    UINT indexCount = PrimitiveCount * 3;
    auto cap = BuildDrawCapture(mtlPrimType);
    if (!cap.pso) return D3DERR_INVALIDCALL;

    BatchedDraw bd;
    bd.cap = std::move(cap);
    bd.type = BatchedDraw::kIndexed;
    bd.indexCount = indexCount;
    bd.indexType = WMTIndexTypeUInt16;
    bd.ibRawBuffer = fanIB.buffer;
    bd.ibOffset = fanIB.offset;
    bd.baseVertex = StartVertex;
    QueueBatchedDraw(std::move(bd));
    return S_OK;
  }

  auto cap = BuildDrawCapture(mtlPrimType);
  if (!cap.pso) return D3DERR_INVALIDCALL;

  BatchedDraw bd;
  bd.cap = std::move(cap);
  bd.type = BatchedDraw::kNonIndexed;
  bd.vertexStart = StartVertex;
  bd.vertexCount = vertexCount;
  QueueBatchedDraw(std::move(bd));

  return S_OK;
}

// DrawIndexedPrimitive
HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
    UINT MinVertexIndex, UINT NumVertices,
    UINT StartIndex, UINT PrimitiveCount) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_count++;
#endif
  if (!current_vdecl_ || !current_ib_) return D3DERR_INVALIDCALL;

  UINT indexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, indexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  // Triangle fan with index buffer: expand fan indices to triangle list
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    bool is32 = (current_ib_->format() == D3DFMT_INDEX32);
    UINT srcStride = is32 ? 4 : 2;
    auto *srcData = (const uint8_t *)current_ib_->allocation()->mappedMemory(0)
                    + StartIndex * srcStride;

    // Expand to a triangle list, keeping the source index width (32-bit indices were truncated to 16 bits).
    auto &queue = dxmt_device_->queue();
    UINT ibSize = indexCount * srcStride;
    auto fanIB = queue.AllocateTransientBuffer(ibSize, srcStride);
    for (UINT i = 0; i < PrimitiveCount; i++) {
      if (is32) {
        auto *src = (const uint32_t *)srcData;
        auto *dst = (uint32_t *)fanIB.cpu_ptr;
        dst[i * 3 + 0] = src[0]; dst[i * 3 + 1] = src[i + 1]; dst[i * 3 + 2] = src[i + 2];
      } else {
        auto *src = (const uint16_t *)srcData;
        auto *dst = (uint16_t *)fanIB.cpu_ptr;
        dst[i * 3 + 0] = src[0]; dst[i * 3 + 1] = src[i + 1]; dst[i * 3 + 2] = src[i + 2];
      }
    }

    auto cap = BuildDrawCapture(mtlPrimType);
    if (!cap.pso) return D3DERR_INVALIDCALL;

    BatchedDraw bd;
    bd.cap = std::move(cap);
    bd.type = BatchedDraw::kIndexed;
    bd.indexCount = indexCount;
    bd.indexType = is32 ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    bd.ibRawBuffer = fanIB.buffer;
    bd.ibOffset = fanIB.offset;
    bd.baseVertex = BaseVertexIndex;
    QueueBatchedDraw(std::move(bd));
    return S_OK;
  }

  auto cap = BuildDrawCapture(mtlPrimType);
  if (!cap.pso) return D3DERR_INVALIDCALL;

  WMTIndexType indexType = (current_ib_->format() == D3DFMT_INDEX32)
      ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
  UINT indexStride = (indexType == WMTIndexTypeUInt32) ? 4 : 2;
  uint64_t indexBufferOffset = StartIndex * indexStride;

  BatchedDraw bd;
  bd.cap = std::move(cap);
  bd.type = BatchedDraw::kIndexed;
  bd.indexCount = indexCount;
  bd.indexType = indexType;
  {
    // Always capture current allocation directly — kept alive by Rc until GPU finishes.
    // This ensures Lock-triggered renames don't affect already-captured draws.
    auto *alloc = current_ib_->allocation();
    bd.ibAlloc = Rc(alloc);
    bd.ibRawBuffer = alloc->buffer();
    bd.ibOffset = indexBufferOffset;
  }
  bd.baseVertex = BaseVertexIndex;
  QueueBatchedDraw(std::move(bd));

  return S_OK;
}

// DrawPrimitiveUP
HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_up_count++;
#endif
  if (!current_vdecl_ || !pVertexStreamZeroData)
    return D3DERR_INVALIDCALL;

  UINT vertexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, vertexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  auto &queue = dxmt_device_->queue();

  // For fan, we need all fan vertices (primCount + 2), not the expanded count
  UINT sourceVertexCount = (PrimitiveType == D3DPT_TRIANGLEFAN)
    ? PrimitiveCount + 2 : vertexCount;
  UINT dataSize = sourceVertexCount * VertexStreamZeroStride;
  auto transientVB = queue.AllocateTransientBuffer(dataSize, 16);
  memcpy(transientVB.cpu_ptr, pVertexStreamZeroData, dataSize);

  // Save and set stream source
  auto savedVB = stream_sources_[0];
  auto savedOffset = stream_offsets_[0];
  auto savedStride = stream_strides_[0];

  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = VertexStreamZeroStride;

  auto cap = BuildDrawCapture(mtlPrimType);
  // Override VB capture with transient ring allocation
  cap.vb_captures[0].buffer = nullptr;
  cap.vb_captures[0].raw_buffer = transientVB.buffer;
  cap.vb_captures[0].gpu_address = transientVB.gpu_address;
  cap.vb_captures[0].offset = 0;
  cap.vb_captures[0].stride = VertexStreamZeroStride;
  cap.slot_mask |= 1;

  // Restore stream source
  stream_sources_[0] = savedVB;
  stream_offsets_[0] = savedOffset;
  stream_strides_[0] = savedStride;

  if (!cap.pso) return D3DERR_INVALIDCALL;

  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto fanIB = GenerateFanIndices(queue, PrimitiveCount, 0);
    UINT fanIndexCount = PrimitiveCount * 3;
    BatchedDraw bd;
    bd.cap = std::move(cap);
    bd.type = BatchedDraw::kIndexed;
    bd.indexCount = fanIndexCount;
    bd.indexType = WMTIndexTypeUInt16;
    bd.ibRawBuffer = fanIB.buffer;
    bd.ibOffset = fanIB.offset;
    bd.baseVertex = 0;
    QueueBatchedDraw(std::move(bd));
  } else {
    BatchedDraw bd;
    bd.cap = std::move(cap);
    bd.type = BatchedDraw::kNonIndexed;
    bd.vertexStart = 0;
    bd.vertexCount = vertexCount;
    QueueBatchedDraw(std::move(bd));
  }

  // Per D3D9 spec: clear stream source and index buffer after UP draws
  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = 0;
  current_ib_ = nullptr;

  return S_OK;
}

// DrawIndexedPrimitiveUP
HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
    UINT NumVertices, UINT PrimitiveCount,
    const void *pIndexData, D3DFORMAT IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_up_count++;
#endif
  if (!current_vdecl_ || !pIndexData || !pVertexStreamZeroData)
    return D3DERR_INVALIDCALL;

  UINT indexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, indexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  auto &queue = dxmt_device_->queue();

  // Create transient vertex buffer from ring
  UINT vbDataSize = (MinVertexIndex + NumVertices) * VertexStreamZeroStride;
  auto transientVB = queue.AllocateTransientBuffer(vbDataSize, 16);
  memcpy(transientVB.cpu_ptr, pVertexStreamZeroData, vbDataSize);

  // Create transient index buffer — expand fan if needed
  CommandQueue::TransientAllocation transientIB{};
  UINT indexStride = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    // Expand fan → triangle list
    bool is32 = (IndexDataFormat == D3DFMT_INDEX32);
    const uint8_t *srcData = (const uint8_t *)pIndexData;
    UINT outStride = is32 ? 4 : 2;
    transientIB = queue.AllocateTransientBuffer(indexCount * outStride, outStride);
    for (UINT i = 0; i < PrimitiveCount; i++) {
      if (is32) {
        auto *src = (const uint32_t *)srcData;
        auto *dst = static_cast<uint32_t *>(transientIB.cpu_ptr);
        dst[i * 3 + 0] = src[0]; dst[i * 3 + 1] = src[i + 1]; dst[i * 3 + 2] = src[i + 2];
      } else {
        auto *src = (const uint16_t *)srcData;
        auto *dst = static_cast<uint16_t *>(transientIB.cpu_ptr);
        dst[i * 3 + 0] = src[0]; dst[i * 3 + 1] = src[i + 1]; dst[i * 3 + 2] = src[i + 2];
      }
    }
    indexStride = outStride;
  } else {
    UINT ibDataSize = indexCount * indexStride;
    transientIB = queue.AllocateTransientBuffer(ibDataSize, indexStride);
    memcpy(transientIB.cpu_ptr, pIndexData, ibDataSize);
  }

  WMTIndexType indexType = (IndexDataFormat == D3DFMT_INDEX32) ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;

  // Save and set stream source
  auto savedVB = stream_sources_[0];
  auto savedOffset = stream_offsets_[0];
  auto savedStride = stream_strides_[0];

  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = VertexStreamZeroStride;

  auto cap = BuildDrawCapture(mtlPrimType);
  cap.vb_captures[0].buffer = nullptr;
  cap.vb_captures[0].raw_buffer = transientVB.buffer;
  cap.vb_captures[0].gpu_address = transientVB.gpu_address;
  cap.vb_captures[0].offset = 0;
  cap.vb_captures[0].stride = VertexStreamZeroStride;
  cap.slot_mask |= 1;

  // Restore stream source
  stream_sources_[0] = savedVB;
  stream_offsets_[0] = savedOffset;
  stream_strides_[0] = savedStride;

  if (!cap.pso) return D3DERR_INVALIDCALL;

  {
    BatchedDraw bd;
    bd.cap = std::move(cap);
    bd.type = BatchedDraw::kIndexed;
    bd.indexCount = indexCount;
    bd.indexType = indexType;
    bd.ibRawBuffer = transientIB.buffer;
    bd.ibOffset = transientIB.offset;
    bd.baseVertex = 0;
    QueueBatchedDraw(std::move(bd));
  }

  // Per D3D9 spec: clear stream source and index buffer after UP draws
  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = 0;
  current_ib_ = nullptr;

  return S_OK;
}

// Present
HRESULT STDMETHODCALLTYPE D3D9Device::Present(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *) {
  if (pSourceRect || pDestRect || hDestWindowOverride) {
    static bool warned = false;
    if (!warned) {
      Logger::warn("D3D9: Present with source/dest rects or window override not implemented");
      warned = true;
    }
  }

#ifdef DXMT_PERF
  auto &perf_stats = dxmt_device_->queue().CurrentFrameStatistics();
  perf_stats.d3d9_frame_time = clock::now() - frame_start_;
  frame_start_ = clock::now();
#endif

  FlushDrawBatch();

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  chunk->emitcc([
    this,
    state = presenter_->synchronizeLayerProperties(),
    backbuffer = backbuffer_
  ](ArgumentEncodingContext &ctx) mutable {
    double vsync = (present_params_.PresentationInterval & D3DPRESENT_INTERVAL_IMMEDIATE)
        ? 0.0 : 1.0 / init_refresh_rate_;
    ctx.present(backbuffer, presenter_, vsync, state.metadata);
    UpdateStatistics(ctx.queue().statistics, ctx.currentFrameId());
  });

  chunk->signal_frame_latency_fence_ = queue.CurrentFrameSeq();
  queue.CommitCurrentChunk();
  queue.PresentBoundary();

  return S_OK;
}

void D3D9Device::UpdateStatistics(const FrameStatisticsContainer &statistics, uint64_t frame_id) {
#ifdef DXMT_PERF
  if (frame_id % 15 != 0) return;

  hud_.beginUpdate();
  auto &avg = statistics.average();
  auto ms = [](clock::duration d) -> double { return d.count() / 1000000.0; };
  char buf[128];
  int line = 0;

  double f = ms(avg.d3d9_frame_time);
  double cap = ms(avg.d3d9_build_capture_time);
  double lock = ms(avg.d3d9_lock_time);
  double flush = ms(avg.d3d9_flush_batch_time);
  double pso = ms(avg.d3d9_create_pso_time);
  double cnst = ms(avg.d3d9_constant_snapshot_time);
  double tex_up = ms(avg.d3d9_texture_upload_time);
  double vb = ms(avg.d3d9_vb_capture_time);
  double depth = ms(avg.d3d9_depth_state_time);
  double texbnd = ms(avg.d3d9_texture_bind_time);
  double d3d9_pct = f > 0 ? (cap + lock + flush) / f * 100.0 : 0;

  // Encoder thread
  snprintf(buf, sizeof(buf), "-- Encoder --");
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Prepare  %.1fms", ms(avg.encode_prepare_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Flush    %.1fms", ms(avg.encode_flush_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Commit   %.1fms", ms(avg.commit_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  CmdBuf   %u", avg.command_buffer_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Render   %u(%umerged)",
      avg.render_pass_count, avg.render_pass_optimized);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Clear    %u(%umerged)",
      avg.clear_pass_count, avg.clear_pass_optimized);
  hud_.updateLine(line++, buf);

  // API thread
  snprintf(buf, sizeof(buf), "-- API (%.0f%% d3d9) --", d3d9_pct);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Frame    %.1fms", f);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Latency  %.1fms", ms(avg.present_lantency_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Draws    %u(+%uUP)",
      avg.d3d9_draw_count, avg.d3d9_draw_up_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Batches  %u", avg.d3d9_batch_flush_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  States   %u", avg.d3d9_state_change_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Capture  %.1fms", cap);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    PSO    %.1f(%umiss)",
      pso, avg.d3d9_pso_miss_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    Const  %.1f", cnst);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    TexUp  %.1f", tex_up);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    VB     %.1f", vb);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    Depth  %.1f", depth);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    TexBnd %.1f", texbnd);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "    Other  %.1f", cap - pso - cnst - tex_up - vb - depth - texbnd);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Lock     %.1fms", lock);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Flush    %.1fms", flush);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Other    %.1fms", f - cap - lock - flush);
  hud_.updateLine(line++, buf);

  hud_.endUpdate();
#endif
}

// CreateOffscreenPlainSurface
HRESULT STDMETHODCALLTYPE D3D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  if (Pool != D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;

  *ppSurface = ref(new D3D9Surface(this, Width, Height, Format, Pool));
  return S_OK;
}

// GetRenderTargetData - copy render target to system memory surface
HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTargetData(
    IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
  FlushDrawBatch();
  if (!pRenderTarget || !pDestSurface) return D3DERR_INVALIDCALL;

  auto *dst = static_cast<D3D9Surface *>(pDestSurface);
  if (dst->pool() != D3DPOOL_SYSTEMMEM || !dst->sysMemData())
    return D3DERR_INVALIDCALL;

  auto *src = static_cast<D3D9Surface *>(pRenderTarget);
  if (!src->texture()) return D3DERR_INVALIDCALL;

  UINT width = src->texture()->width();
  UINT height = src->texture()->height();
  UINT bytesPerRow = width * 4;
  UINT totalBytes = bytesPerRow * height;
  UINT alignedSize = (totalBytes + 0xFFF) & ~0xFFFu; // page-align

  // Create a shared buffer for readback
  void *placedMem = wsi::aligned_malloc(alignedSize, DXMT_PAGE_SIZE);
  if (!placedMem) return D3DERR_INVALIDCALL;

  WMTBufferInfo bufInfo = {};
  bufInfo.length = alignedSize;
  bufInfo.options = WMTResourceStorageModeShared;
  bufInfo.memory.set(placedMem);
  auto readbackBuf = dxmt_device_->device().newBuffer(bufInfo);

  // Emit blit through the standard command queue
  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  chunk->emitcc([
    srcTex = src->texture(),
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

  // Copy from shared buffer to system memory surface
  std::memcpy(dst->sysMemData(), placedMem, totalBytes);

  wsi::aligned_free(placedMem);
  return S_OK;
}

// Fixed-function pipeline helpers

FFVSKey D3D9Device::BuildFFVSKey() {
  FFVSKey key = {};
  if (!current_vdecl_) return key;

  for (auto &elem : current_vdecl_->elements()) {
    if (elem.Stream == 0xFF) break;
    switch (elem.Usage) {
    case D3DDECLUSAGE_POSITIONT:
      key.has_position_t = 1;
      break;
    case D3DDECLUSAGE_NORMAL:
      key.has_normal = 1;
      break;
    case D3DDECLUSAGE_COLOR:
      if (elem.UsageIndex == 0) key.has_color0 = 1;
      else if (elem.UsageIndex == 1) key.has_color1 = 1;
      break;
    case D3DDECLUSAGE_TEXCOORD:
      if (elem.UsageIndex + 1 > key.tex_coord_count)
        key.tex_coord_count = elem.UsageIndex + 1;
      break;
    default:
      break;
    }
  }

  FillFFVSTexcoordModes(key);

  // Fog mode: prefer vertex fog, fall back to table fog mode
  // (table fog is normally per-pixel, but we approximate per-vertex in the FF VS)
  if (render_states_[D3DRS_FOGENABLE]) {
    DWORD fogMode = render_states_[D3DRS_FOGVERTEXMODE];
    if (fogMode == D3DFOG_NONE)
      fogMode = render_states_[D3DRS_FOGTABLEMODE];
    switch (fogMode) {
    case D3DFOG_EXP:    key.fog_mode = 1; break;
    case D3DFOG_EXP2:   key.fog_mode = 2; break;
    case D3DFOG_LINEAR: key.fog_mode = 3; break;
    default:            key.fog_mode = 0; break;
    }
  }

  // Lighting (only for non-POSITIONT vertices)
  if (render_states_[D3DRS_LIGHTING] && !key.has_position_t) {
    key.lighting_enabled = 1;
    key.normalize_normals = render_states_[D3DRS_NORMALIZENORMALS] ? 1 : 0;
    key.color_vertex = render_states_[D3DRS_COLORVERTEX] ? 1 : 0;
    key.diffuse_source = (uint8_t)render_states_[D3DRS_DIFFUSEMATERIALSOURCE];
    key.ambient_source = (uint8_t)render_states_[D3DRS_AMBIENTMATERIALSOURCE];
    key.specular_source = (uint8_t)render_states_[D3DRS_SPECULARMATERIALSOURCE];
    key.emissive_source = (uint8_t)render_states_[D3DRS_EMISSIVEMATERIALSOURCE];

    uint8_t count = 0;
    for (DWORD i = 0; i < kMaxLights && count < 8; i++) {
      if (light_enabled_[i]) {
        key.light_types[count] = (uint8_t)lights_[i].Type;
        count++;
      }
    }
    key.num_active_lights = count;
  }

  return key;
}

void D3D9Device::FillFFVSTexcoordModes(FFVSKey &key) {
  // TCI generation modes and texture transform flags per texcoord output (output i = stage i)
  for (uint8_t i = 0; i < 8; i++) {
    if (i < key.tex_coord_count) {
      DWORD tci = texture_stage_states_[i][D3DTSS_TEXCOORDINDEX];
      key.tci_modes[i] = (uint8_t)((tci >> 16) & 0x3); // 0=passthru, 1=normal, 2=position, 3=reflection
      key.tci_coord_indices[i] = (uint8_t)(tci & 0x7);
      key.ttf_modes[i] = (uint8_t)(texture_stage_states_[i][D3DTSS_TEXTURETRANSFORMFLAGS] & 0x7);
    } else {
      key.tci_modes[i] = key.tci_coord_indices[i] = key.ttf_modes[i] = 0;
    }
  }
}

FFPSKey D3D9Device::BuildFFPSKey() {
  FFPSKey key = {};
  for (uint32_t i = 0; i < 8; i++) {
    auto &stage = key.stages[i];
    stage.color_op   = (uint8_t)texture_stage_states_[i][D3DTSS_COLOROP];
    stage.color_arg1 = (uint8_t)texture_stage_states_[i][D3DTSS_COLORARG1];
    stage.color_arg2 = (uint8_t)texture_stage_states_[i][D3DTSS_COLORARG2];
    stage.alpha_op   = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAOP];
    stage.alpha_arg1 = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAARG1];
    stage.alpha_arg2 = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAARG2];
    stage.has_texture = bound_textures_[i] ? 1 : 0;
    stage.texcoord_index = (uint8_t)(texture_stage_states_[i][D3DTSS_TEXCOORDINDEX] & 0x7);
    // With the FF VS, output TEXCOORD<i> already holds stage i's routed/generated/transformed coordinates
    // (TEXCOORDINDEX low bits + TCI mode + D3DTS_TEXTUREi), so stage i must sample TEXCOORD<i>.
    if (!current_vs_) stage.texcoord_index = (uint8_t)i;
    stage.color_arg0 = (uint8_t)texture_stage_states_[i][D3DTSS_COLORARG0];
    stage.alpha_arg0 = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAARG0];
    stage.result_arg = (uint8_t)texture_stage_states_[i][D3DTSS_RESULTARG];
    if (stage.color_op == D3DTOP_DISABLE) break;
  }
  key.specular_enable = render_states_[D3DRS_SPECULARENABLE] ? 1 : 0;
  key.alpha_test_enable = render_states_[D3DRS_ALPHATESTENABLE] ? 1 : 0;
  key.alpha_test_func = (uint8_t)render_states_[D3DRS_ALPHAFUNC];
  key.fog_enable = render_states_[D3DRS_FOGENABLE] ? 1 : 0;
  return key;
}

WMT::Reference<WMT::Function> D3D9Device::GetOrCreateFFVS(
    const FFVSKey &key, D3D9VertexDeclaration *vdecl) {
  auto &elements = vdecl->elements();
  uint32_t slotMask = vdecl->slotMask();
  uint32_t numElements = elements.size() > 0 ? (uint32_t)elements.size() - 1 : 0; // exclude end sentinel

  // Cache key must include the layout: a shader compiled for one layout read the wrong streams/offsets
  // for another layout with the same FFVSKey (GTA SA: save pickup collapsed to a point, random stray triangles).
  std::string cacheKey((const char *)&key, sizeof(key));
  cacheKey.append((const char *)elements.data(), numElements * sizeof(D3DVERTEXELEMENT9));
  cacheKey.append((const char *)&slotMask, sizeof(slotMask));
  auto it = ff_vs_cache_.find(cacheKey);
  if (it != ff_vs_cache_.end())
    return it->second;

  auto func = GenerateFFVertexShader(
    dxmt_device_->device(), key, elements.data(), numElements, slotMask,
    dxmt_device_->metalVersion());
  if (func)
    ff_vs_cache_.emplace(std::move(cacheKey), func);
  return func;
}

WMT::Reference<WMT::Function> D3D9Device::GetOrCreateFFPS(const FFPSKey &key) {
  auto it = ff_ps_cache_.find(key);
  if (it != ff_ps_cache_.end())
    return it->second;

  auto func = GenerateFFPixelShader(
    dxmt_device_->device(), key, key.tex_coord_count,
    dxmt_device_->metalVersion());
  if (func)
    ff_ps_cache_.emplace(key, func);
  return func;
}

static void TransformDirectionByMatrix(const D3DMATRIX &m, float inX, float inY, float inZ,
                                        float &outX, float &outY, float &outZ) {
  outX = inX * m._11 + inY * m._21 + inZ * m._31;
  outY = inX * m._12 + inY * m._22 + inZ * m._32;
  outZ = inX * m._13 + inY * m._23 + inZ * m._33;
}

static void TransformPointByMatrix(const D3DMATRIX &m, float inX, float inY, float inZ,
                                   float &outX, float &outY, float &outZ) {
  outX = inX * m._11 + inY * m._21 + inZ * m._31 + m._41;
  outY = inX * m._12 + inY * m._22 + inZ * m._32 + m._42;
  outZ = inX * m._13 + inY * m._23 + inZ * m._33 + m._43;
}

void D3D9Device::UpdateFFConstants() {
  auto *vs = cached_ff_vs_;
  uint32_t dirty = ff_dirty_;
  if (!dirty) return;

  auto &world = transforms_[TransformIndex(D3DTS_WORLD)];
  auto &view = transforms_[TransformIndex(D3DTS_VIEW)];
  auto &proj = transforms_[TransformIndex(D3DTS_PROJECTION)];

  // Transforms: world/view/proj always needed for WorldView/WVP computation
  if (dirty & (kFFDirtyWorld | kFFDirtyView | kFFDirtyProj)) {
    if (dirty & kFFDirtyWorld) memcpy(&vs[0], &world, sizeof(D3DMATRIX));
    if (dirty & kFFDirtyView) memcpy(&vs[4], &view, sizeof(D3DMATRIX));
    if (dirty & kFFDirtyProj) memcpy(&vs[8], &proj, sizeof(D3DMATRIX));
    // WorldView and WVP must be recomputed when any transform changes
    auto wv = MultiplyMatrices(world, view);
    memcpy(&vs[12], &wv, sizeof(D3DMATRIX));
    auto wvp = MultiplyMatrices(wv, proj);
    memcpy(&vs[16], &wvp, sizeof(D3DMATRIX));
  }

  if (dirty & kFFDirtyViewport) {
    vs[20][0] = (float)viewport_.Width;
    vs[20][1] = (float)viewport_.Height;
    vs[20][2] = 0.0f;
    vs[20][3] = 0.0f;
  }

  if (dirty & kFFDirtyFog) {
    float fogStart, fogEnd, fogDensity;
    memcpy(&fogStart, &render_states_[D3DRS_FOGSTART], sizeof(float));
    memcpy(&fogEnd, &render_states_[D3DRS_FOGEND], sizeof(float));
    memcpy(&fogDensity, &render_states_[D3DRS_FOGDENSITY], sizeof(float));
    vs[21][0] = fogStart;
    vs[21][1] = fogEnd;
    vs[21][2] = fogDensity;
    vs[21][3] = 0.0f;
  }

  if (dirty & kFFDirtyAmbient) {
    DWORD amb = render_states_[D3DRS_AMBIENT];
    vs[22][0] = (float)((amb >> 16) & 0xFF) / 255.0f;
    vs[22][1] = (float)((amb >>  8) & 0xFF) / 255.0f;
    vs[22][2] = (float)((amb >>  0) & 0xFF) / 255.0f;
    vs[22][3] = (float)((amb >> 24) & 0xFF) / 255.0f;
  }

  if (dirty & kFFDirtyMaterial) {
    vs[23][0] = material_.Diffuse.r;  vs[23][1] = material_.Diffuse.g;
    vs[23][2] = material_.Diffuse.b;  vs[23][3] = material_.Diffuse.a;
    vs[24][0] = material_.Ambient.r;  vs[24][1] = material_.Ambient.g;
    vs[24][2] = material_.Ambient.b;  vs[24][3] = material_.Ambient.a;
    vs[25][0] = material_.Specular.r; vs[25][1] = material_.Specular.g;
    vs[25][2] = material_.Specular.b; vs[25][3] = material_.Specular.a;
    vs[26][0] = material_.Emissive.r; vs[26][1] = material_.Emissive.g;
    vs[26][2] = material_.Emissive.b; vs[26][3] = material_.Emissive.a;
    vs[27][0] = material_.Power;
    vs[27][1] = 0.0f; vs[27][2] = 0.0f; vs[27][3] = 0.0f;
  }

  if (dirty & kFFDirtyLights) {
    memset(&vs[28], 0, 40 * 4 * sizeof(float));
    memset(&vs[100], 0, 8 * 4 * sizeof(float));
    uint32_t li = 0;
    for (DWORD i = 0; i < kMaxLights && li < 8; i++) {
      if (!light_enabled_[i]) continue;
      auto &light = lights_[i];
      uint32_t base = 28 + li * 5;
      vs[100 + li][0] = light.Specular.r; vs[100 + li][1] = light.Specular.g;
      vs[100 + li][2] = light.Specular.b; vs[100 + li][3] = light.Specular.a;

      float px, py, pz;
      TransformPointByMatrix(view, light.Position.x, light.Position.y, light.Position.z, px, py, pz);
      vs[base][0] = px; vs[base][1] = py; vs[base][2] = pz;
      vs[base][3] = (float)light.Type;

      float dx, dy, dz;
      TransformDirectionByMatrix(view, light.Direction.x, light.Direction.y, light.Direction.z, dx, dy, dz);
      float dlen = sqrtf(dx*dx + dy*dy + dz*dz);
      if (dlen > 0.0f) { dx /= dlen; dy /= dlen; dz /= dlen; }
      vs[base+1][0] = dx; vs[base+1][1] = dy; vs[base+1][2] = dz;
      vs[base+1][3] = light.Falloff;

      vs[base+2][0] = light.Diffuse.r;  vs[base+2][1] = light.Diffuse.g;
      vs[base+2][2] = light.Diffuse.b;  vs[base+2][3] = light.Diffuse.a;
      vs[base+3][0] = light.Ambient.r;  vs[base+3][1] = light.Ambient.g;
      vs[base+3][2] = light.Ambient.b;  vs[base+3][3] = light.Ambient.a;
      vs[base+4][0] = light.Attenuation0; vs[base+4][1] = light.Attenuation1;
      vs[base+4][2] = light.Attenuation2; vs[base+4][3] = light.Range;

      li++;
    }
  }

  if (dirty & kFFDirtyTexMat) {
    for (uint32_t i = 0; i < 8; i++) {
      auto &texMat = transforms_[TransformIndex((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i))];
      memcpy(&vs[68 + i * 4], &texMat, sizeof(D3DMATRIX));
    }
  }

  // PS constants
  if (dirty & (kFFDirtyPS | kFFDirtyFog)) {
    auto *ps = cached_ff_ps_;
    DWORD tf = render_states_[D3DRS_TEXTUREFACTOR];
    ps[0][0] = (float)((tf >> 16) & 0xFF) / 255.0f;
    ps[0][1] = (float)((tf >>  8) & 0xFF) / 255.0f;
    ps[0][2] = (float)((tf >>  0) & 0xFF) / 255.0f;
    ps[0][3] = (float)((tf >> 24) & 0xFF) / 255.0f;

    DWORD fc = render_states_[D3DRS_FOGCOLOR];
    ps[1][0] = (float)((fc >> 16) & 0xFF) / 255.0f;
    ps[1][1] = (float)((fc >>  8) & 0xFF) / 255.0f;
    ps[1][2] = (float)((fc >>  0) & 0xFF) / 255.0f;
    ps[1][3] = (float)(render_states_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;

    float fogStart, fogEnd, fogDensity;
    memcpy(&fogStart, &render_states_[D3DRS_FOGSTART], sizeof(float));
    memcpy(&fogEnd, &render_states_[D3DRS_FOGEND], sizeof(float));
    memcpy(&fogDensity, &render_states_[D3DRS_FOGDENSITY], sizeof(float));
    ps[2][0] = fogStart;
    ps[2][1] = fogEnd;
    ps[2][2] = fogDensity;
    ps[2][3] = 0.0f;
  }

  ff_dirty_ = 0;
}

// CreateRenderTarget
HRESULT STDMETHODCALLTYPE D3D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  if (!ppSurface) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9Format(Format);
  if (mtlFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("D3D9: CreateRenderTarget unsupported format ", (int)Format));
    return D3DERR_INVALIDCALL;
  }

  Logger::info(str::format("D3D9: CreateRenderTarget ", Width, "x", Height,
      " ", D3D9FormatName(Format), " msaa=", (int)MultiSample, " msaaQ=", MultisampleQuality,
      " lockable=", Lockable ? 1 : 0));

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  info.options = WMTResourceStorageModePrivate;

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  auto viewKey = texture->createView({
      .format = mtlFormat,
      .type = WMTTextureType2DArray,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });

  *ppSurface = ref(new D3D9Surface(this, texture, viewKey, mtlFormat));
  return S_OK;
}

// StretchRect - basic blit copy
HRESULT STDMETHODCALLTYPE D3D9Device::StretchRect(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
    IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter) {
  FlushDrawBatch();
  if (!pSourceSurface || !pDestSurface) return D3DERR_INVALIDCALL;

  // Extract GPU textures from either D3D9Surface or D3D9TextureSurface
  auto getTexture = [](IDirect3DSurface9 *surf) -> Rc<Texture> {
    IDirect3DTexture9 *container = nullptr;
    if (SUCCEEDED(surf->GetContainer(__uuidof(IDirect3DTexture9), (void **)&container))) {
      auto *texSurf = static_cast<D3D9TextureSurface *>(surf);
      container->Release();
      return texSurf->texture();
    }
    return static_cast<D3D9Surface *>(surf)->texture();
  };

  auto srcTex = getTexture(pSourceSurface);
  auto dstTex = getTexture(pDestSurface);

  if (!srcTex || !dstTex) {
    Logger::warn("D3D9: StretchRect requires GPU-backed surfaces");
    return D3DERR_INVALIDCALL;
  }

  // Determine source/dest regions
  UINT sx = 0, sy = 0;
  UINT sw = srcTex->width();
  UINT sh = srcTex->height();
  UINT dx = 0, dy = 0;
  if (pSourceRect) {
    sx = pSourceRect->left; sy = pSourceRect->top;
    sw = pSourceRect->right - pSourceRect->left;
    sh = pSourceRect->bottom - pSourceRect->top;
  }
  if (pDestRect) {
    dx = pDestRect->left; dy = pDestRect->top;
    UINT dw = pDestRect->right - pDestRect->left;
    UINT dh = pDestRect->bottom - pDestRect->top;
    if (dw != sw || dh != sh) {
      static bool warned = false;
      if (!warned) {
        Logger::warn("D3D9: StretchRect scaling not implemented, using source size");
        warned = true;
      }
    }
  }

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  chunk->emitcc([
    srcTex = std::move(srcTex), dstTex = std::move(dstTex),
    sx, sy, sw, sh, dx, dy
  ](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();

    auto srcHandle = ctx.access(srcTex, 0u, 0u, DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto dstHandle = ctx.access(dstTex, 0u, 0u, DXMT_ENCODER_RESOURCE_ACESS_WRITE);

    auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
    blitCmd.type = WMTBlitCommandCopyFromTextureToTexture;
    blitCmd.src = srcHandle;
    blitCmd.src_slice = 0;
    blitCmd.src_level = 0;
    blitCmd.src_origin = {sx, sy, 0};
    blitCmd.src_size = {sw, sh, 1};
    blitCmd.dst = dstHandle;
    blitCmd.dst_slice = 0;
    blitCmd.dst_level = 0;
    blitCmd.dst_origin = {dx, dy, 0};

    ctx.endPass();
  });

  return S_OK;
}

// UpdateTexture - copy mip data from src to dst texture
HRESULT STDMETHODCALLTYPE D3D9Device::UpdateTexture(
    IDirect3DBaseTexture9 *pSourceTexture,
    IDirect3DBaseTexture9 *pDestinationTexture) {
  if (!pSourceTexture || !pDestinationTexture) return D3DERR_INVALIDCALL;

  // Both must be 2D textures
  IDirect3DTexture9 *srcTex2D = nullptr;
  IDirect3DTexture9 *dstTex2D = nullptr;
  if (FAILED(pSourceTexture->QueryInterface(__uuidof(IDirect3DTexture9), (void **)&srcTex2D)) ||
      FAILED(pDestinationTexture->QueryInterface(__uuidof(IDirect3DTexture9), (void **)&dstTex2D))) {
    Logger::warn("D3D9: UpdateTexture only supports 2D textures");
    if (srcTex2D) srcTex2D->Release();
    if (dstTex2D) dstTex2D->Release();
    return D3DERR_INVALIDCALL;
  }

  auto *src = static_cast<D3D9Texture2D *>(srcTex2D);
  auto *dst = static_cast<D3D9Texture2D *>(dstTex2D);

  // Copy mip data from source staging to dest staging, then mark dest dirty
  UINT levels = std::min(src->levelCount(), dst->levelCount());
  for (UINT i = 0; i < levels; i++) {
    D3DLOCKED_RECT srcLR, dstLR;
    src->LockRect(i, &srcLR, nullptr, D3DLOCK_READONLY);
    dst->LockRect(i, &dstLR, nullptr, 0);

    D3DSURFACE_DESC desc;
    src->GetLevelDesc(i, &desc);
    UINT rowSize = D3D9FormatPitch(src->format(), desc.Width);
    UINT rows = IsCompressedFormat(src->format()) ? (desc.Height + 3) / 4 : desc.Height;

    for (UINT row = 0; row < rows; row++) {
      memcpy((uint8_t *)dstLR.pBits + row * dstLR.Pitch,
             (const uint8_t *)srcLR.pBits + row * srcLR.Pitch,
             rowSize);
    }

    src->UnlockRect(i);
    dst->UnlockRect(i);
  }

  srcTex2D->Release();
  dstTex2D->Release();
  return S_OK;
}

// GetVertexShaderConstantF
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantF(
    UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &vsConstants_[StartRegister], Vector4fCount * 4 * sizeof(float));
  return S_OK;
}

// GetPixelShaderConstantF
HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantF(
    UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &psConstants_[StartRegister], Vector4fCount * 4 * sizeof(float));
  return S_OK;
}

// GetStreamSource
HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
    UINT *pOffsetInBytes, UINT *pStride) {
  if (StreamNumber >= 16 || !ppStreamData) return D3DERR_INVALIDCALL;
  *ppStreamData = stream_sources_[StreamNumber].ref();
  if (pOffsetInBytes) *pOffsetInBytes = stream_offsets_[StreamNumber];
  if (pStride) *pStride = stream_strides_[StreamNumber];
  return S_OK;
}

// State blocks
HRESULT STDMETHODCALLTYPE D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  if (!ppSB) return D3DERR_INVALIDCALL;

  Logger::debug(str::format("D3D9: CreateStateBlock type=", (int)Type));

  auto *sb = new D3D9StateBlock(this);

  // Capture current state
  memcpy(sb->render_states, render_states_, sizeof(render_states_));
  memcpy(sb->texture_stage_states, texture_stage_states_, sizeof(texture_stage_states_));
  memcpy(sb->sampler_states, sampler_states_, sizeof(sampler_states_));
  memcpy(sb->transforms, transforms_, sizeof(transforms_));
  sb->viewport = viewport_;
  sb->scissor_rect = scissor_rect_;
  sb->material = material_;
  memcpy(sb->lights, lights_, sizeof(lights_));
  memcpy(sb->light_enabled, light_enabled_, sizeof(light_enabled_));
  sb->fvf = current_fvf_;
  memcpy(sb->vs_constants, vsConstants_, sizeof(vsConstants_));
  memcpy(sb->ps_constants, psConstants_, sizeof(psConstants_));
  sb->vs = current_vs_;
  sb->ps = current_ps_;
  sb->vdecl = current_vdecl_;
  for (int i = 0; i < 16; i++) {
    sb->stream_sources[i] = stream_sources_[i];
    sb->stream_offsets[i] = stream_offsets_[i];
    sb->stream_strides[i] = stream_strides_[i];
    sb->textures[i] = bound_textures_[i];
  }
  sb->ib = current_ib_;

  *ppSB = ref(sb);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::BeginStateBlock() {
  recording_state_block_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  if (!ppSB) return D3DERR_INVALIDCALL;
  recording_state_block_ = false;

  // Create a state block capturing current state (same as CreateStateBlock ALL)
  return CreateStateBlock(D3DSBT_ALL, ppSB);
}

// D3D9StateBlock::Capture
HRESULT STDMETHODCALLTYPE D3D9StateBlock::Capture() {
  memcpy(render_states, device_->render_states_, sizeof(render_states));
  memcpy(texture_stage_states, device_->texture_stage_states_, sizeof(texture_stage_states));
  memcpy(sampler_states, device_->sampler_states_, sizeof(sampler_states));
  memcpy(transforms, device_->transforms_, sizeof(transforms));
  viewport = device_->viewport_;
  scissor_rect = device_->scissor_rect_;
  material = device_->material_;
  memcpy(lights, device_->lights_, sizeof(lights));
  memcpy(light_enabled, device_->light_enabled_, sizeof(light_enabled));
  fvf = device_->current_fvf_;
  memcpy(vs_constants, device_->vsConstants_, sizeof(vs_constants));
  memcpy(ps_constants, device_->psConstants_, sizeof(ps_constants));
  vs = device_->current_vs_;
  ps = device_->current_ps_;
  vdecl = device_->current_vdecl_;
  for (int i = 0; i < 16; i++) {
    stream_sources[i] = device_->stream_sources_[i];
    stream_offsets[i] = device_->stream_offsets_[i];
    stream_strides[i] = device_->stream_strides_[i];
    textures[i] = device_->bound_textures_[i];
  }
  ib = device_->current_ib_;
  return S_OK;
}

// D3D9StateBlock::Apply
HRESULT STDMETHODCALLTYPE D3D9StateBlock::Apply() {
  memcpy(device_->render_states_, render_states, sizeof(render_states));
  memcpy(device_->texture_stage_states_, texture_stage_states, sizeof(texture_stage_states));
  memcpy(device_->sampler_states_, sampler_states, sizeof(sampler_states));
  memcpy(device_->transforms_, transforms, sizeof(transforms));
  device_->viewport_ = viewport;
  device_->scissor_rect_ = scissor_rect;
  device_->material_ = material;
  memcpy(device_->lights_, lights, sizeof(lights));
  memcpy(device_->light_enabled_, light_enabled, sizeof(light_enabled));
  if (fvf) device_->SetFVF(fvf);
  memcpy(device_->vsConstants_, vs_constants, sizeof(vs_constants));
  memcpy(device_->psConstants_, ps_constants, sizeof(ps_constants));
  device_->current_vs_ = vs;
  device_->current_ps_ = ps;
  device_->current_vdecl_ = vdecl;
  for (int i = 0; i < 16; i++) {
    device_->stream_sources_[i] = stream_sources[i];
    device_->stream_offsets_[i] = stream_offsets[i];
    device_->stream_strides_[i] = stream_strides[i];
    device_->bound_textures_[i] = textures[i];
  }
  device_->current_ib_ = ib;
  // Bump all dirty flags since state was restored by memcpy (bypassing setters)
  device_->pso_dirty_ = true;
  device_->dsso_dirty_ = true;
  device_->vb_dirty_ = true;
  device_->tex_dirty_ = true;
  device_->shadow_valid_ = false;
  device_->vs_const_version_++;
  device_->ps_const_version_++;
  device_->ff_const_version_++;
  device_->ff_dirty_ |= D3D9Device::kFFDirtyAll;
  return S_OK;
}

} // namespace dxmt
