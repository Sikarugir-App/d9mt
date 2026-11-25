
#include "d3d9_interface.hpp"
#include <algorithm>
#include <vector>
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include "wsi_monitor.hpp"

namespace dxmt {

D3D9Interface::D3D9Interface() {
  Logger::info("D3D9Interface created");
}

HRESULT STDMETHODCALLTYPE D3D9Interface::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj)
    return E_POINTER;

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
    *ppvObj = ref(this);
    return S_OK;
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::RegisterSoftwareDevice(void *) {
  return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE D3D9Interface::GetAdapterCount() {
  return 1;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::GetAdapterIdentifier(
    UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  if (!pIdentifier)
    return D3DERR_INVALIDCALL;

  memset(pIdentifier, 0, sizeof(*pIdentifier));
  strcpy(pIdentifier->Driver, "dxmt");

  auto devices = WMT::CopyAllDevices();
  auto device = devices.object(0);
  auto name = device.name().getUTF8String();
  strncpy(pIdentifier->Description, name.c_str(), sizeof(pIdentifier->Description) - 1);

  pIdentifier->VendorId = 0x106B; // Apple
  pIdentifier->DeviceId = (DWORD)(device.registryID() & 0xFFFF);

  char buf[256];
  snprintf(buf, sizeof(buf), "D3D9 Adapter: %s vendor=0x%04lX device=0x%04lX registryID=0x%llX",
           name.c_str(), (unsigned long)pIdentifier->VendorId, (unsigned long)pIdentifier->DeviceId,
           (unsigned long long)device.registryID());
  Logger::info(buf);
  return S_OK;
}

static uint32_t getFormatBpp(D3DFORMAT Format) {
  switch (Format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    return 32;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return 16;
  default:
    return 0;
  }
}

static UINT enumerateMatchingModes(D3DFORMAT Format, UINT targetIndex, D3DDISPLAYMODE *pOut) {
  uint32_t bpp = getFormatBpp(Format);
  if (bpp == 0)
    return 0;

  HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  wsi::WsiMode wsiMode;
  std::vector<D3DDISPLAYMODE> modes;

  for (uint32_t i = 0; wsi::getDisplayMode(monitor, i, &wsiMode); i++) {
    if (wsiMode.bitsPerPixel != bpp)
      continue;
    D3DDISPLAYMODE m = {};
    m.Width = wsiMode.width;
    m.Height = wsiMode.height;
    m.RefreshRate = (wsiMode.refreshRate.denominator != 0)
        ? wsiMode.refreshRate.numerator / wsiMode.refreshRate.denominator
        : 60;
    m.Format = Format;
    bool dup = false;
    for (auto &e : modes)
      dup |= e.Width == m.Width && e.Height == m.Height && e.RefreshRate == m.RefreshRate;
    if (!dup)
      modes.push_back(m);
  }

  // Same order as wined3d (width, height, refresh rate), so games that store a
  // mode *index* (e.g. GTA SA's gta_sa.set) pick the same resolution as before.
  std::sort(modes.begin(), modes.end(), [](const D3DDISPLAYMODE &a, const D3DDISPLAYMODE &b) {
    if (a.Width != b.Width) return a.Width < b.Width;
    if (a.Height != b.Height) return a.Height < b.Height;
    return a.RefreshRate < b.RefreshRate;
  });

  if (pOut && targetIndex < modes.size())
    *pOut = modes[targetIndex];
  return (UINT)modes.size();
}

UINT STDMETHODCALLTYPE D3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  if (Adapter != 0)
    return 0;
  return enumerateMatchingModes(Format, UINT_MAX, nullptr);
}

HRESULT STDMETHODCALLTYPE D3D9Interface::EnumAdapterModes(
    UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  if (Adapter != 0 || !pMode)
    return D3DERR_INVALIDCALL;

  UINT count = enumerateMatchingModes(Format, Mode, pMode);
  if (Mode >= count)
    return D3DERR_INVALIDCALL;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  if (Adapter != 0 || !pMode)
    return D3DERR_INVALIDCALL;

  HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  wsi::WsiMode mode;
  if (wsi::getCurrentDisplayMode(monitor, &mode)) {
    pMode->Width = mode.width;
    pMode->Height = mode.height;
    pMode->RefreshRate = (mode.refreshRate.denominator != 0)
        ? mode.refreshRate.numerator / mode.refreshRate.denominator
        : 60;
  } else {
    pMode->Width = 1920;
    pMode->Height = 1080;
    pMode->RefreshRate = 60;
  }
  pMode->Format = D3DFMT_X8R8G8B8;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) {
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE, D3DFORMAT, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;

  // HACK(wow-launch): reject-list approach — reject unsupported resource types and unmapped formats
  // Proper fix: full format capability table
  if (RType == D3DRTYPE_CUBETEXTURE || RType == D3DRTYPE_VOLUMETEXTURE)
    return D3DERR_NOTAVAILABLE;

  // For depth/stencil usage, accept known depth formats
  if (Usage & D3DUSAGE_DEPTHSTENCIL) {
    switch (CheckFormat) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
      return S_OK;
    default:
      return D3DERR_NOTAVAILABLE;
    }
  }

  // For regular textures/surfaces, check if format is mappable
  if (ConvertD3D9Format(CheckFormat) == WMTPixelFormatInvalid)
    return D3DERR_NOTAVAILABLE;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels) {
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  if (pQualityLevels)
    *pQualityLevels = 1;
  if (MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) {
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceFormatConversion(
    UINT Adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) {
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::GetDeviceCaps(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
  if (Adapter != 0 || !pCaps)
    return D3DERR_INVALIDCALL;

  memset(pCaps, 0, sizeof(*pCaps));
  pCaps->DeviceType = D3DDEVTYPE_HAL;
  pCaps->AdapterOrdinal = 0;

  // -- General device caps --
  pCaps->Caps = 0;
  pCaps->Caps2 = D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
  pCaps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD;
  pCaps->CursorCaps = D3DCURSORCAPS_COLOR;
  pCaps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                   D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                   D3DDEVCAPS_PUREDEVICE;

  // -- Primitive/raster caps --
  pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                             D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP |
                             D3DPMISCCAPS_MASKZ;
  // Vertex fog only (no table fog); no MSAA
  pCaps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGRANGE |
                      D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_SCISSORTEST |
                      D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS;

  // -- Comparison caps (all 8 funcs implemented via ConvertCompareFunc) --
  pCaps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                    D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL |
                    D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  pCaps->AlphaCmpCaps = pCaps->ZCmpCaps;

  // -- Blend caps (only factors mapped in ConvertBlendFactor) --
  pCaps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR |
                        D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA |
                        D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
                        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR |
                        D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT |
                        D3DPBLENDCAPS_BLENDFACTOR;
  pCaps->DestBlendCaps = pCaps->SrcBlendCaps;

  // -- Shade caps (FF VS generates Gouraud interpolation; specular + fog in FF PS) --
  pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;

  // -- Texture caps (no cube maps, no volume textures — CheckDeviceFormat rejects both) --
  pCaps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_POW2 |
                       D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_PERSPECTIVE;

  // -- Texture filter caps (min/mag/mip all mapped in ConvertD3D9SamplerState) --
  pCaps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                             D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFPOINT |
                             D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                             D3DPTFILTERCAPS_MIPFLINEAR;
  // No cube/volume texture support, leave CubeTextureFilterCaps and VolumeTextureFilterCaps at 0

  // -- Texture address caps (MIRRORONCE not mapped in convertAddr, omit it) --
  pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR |
                              D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER |
                              D3DPTADDRESSCAPS_INDEPENDENTUV;

  // -- Stencil caps (all 8 ops mapped via ConvertStencilOp; no two-sided — back=front always) --
  pCaps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE |
                       D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT |
                       D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;

  // -- Texture stage operation caps (all ops listed in FFPSKey handling) --
  pCaps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
                         D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
                         D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
                         D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
                         D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
                         D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                         D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
                         D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA |
                         D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
                         D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
                         D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
                         D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA |
                         D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE |
                         D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD |
                         D3DTEXOPCAPS_LERP;

  // -- Texture stage/sampler limits (8 stages initialized in device, 16 sampler slots bound) --
  pCaps->MaxTextureBlendStages = 8;
  pCaps->MaxSimultaneousTextures = 8;

  // -- Texture dimension limits (Metal supports 16384) --
  pCaps->MaxTextureWidth = 16384;
  pCaps->MaxTextureHeight = 16384;
  pCaps->MaxTextureRepeat = 8192;
  pCaps->MaxTextureAspectRatio = 16384;
  pCaps->MaxAnisotropy = 16;
  pCaps->MaxVertexW = 1e10f;

  // -- Line caps (basic line primitives work with same PSO as triangles) --
  pCaps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND |
                    D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;

  // -- FVF caps (8 texture coordinates supported in vertex declaration) --
  pCaps->FVFCaps = (8 & D3DFVFCAPS_TEXCOORDCOUNTMASK) | D3DFVFCAPS_PSIZE;

  // -- Vertex processing (no texgen, no clip planes — both are stubs) --
  pCaps->VertexProcessingCaps = D3DVTXPCAPS_MATERIALSOURCE7 |
                                D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS |
                                D3DVTXPCAPS_LOCALVIEWER;
  pCaps->MaxActiveLights = 8;
  pCaps->MaxUserClipPlanes = 0;
  pCaps->MaxVertexBlendMatrices = 4;
  pCaps->MaxVertexBlendMatrixIndex = 0;
  pCaps->MaxPointSize = 1.0f; // no point size control implemented
  pCaps->MaxPrimitiveCount = 0x00555555;

  // -- Shader caps --
  pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  pCaps->MaxVertexShaderConst = 256;
  pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  pCaps->PixelShader1xMaxValue = 65504.0f;

  // -- Draw limits --
  pCaps->MaxVertexIndex = 0x00FFFFFF;
  pCaps->MaxStreams = 16;
  pCaps->MaxStreamStride = 508;
  pCaps->NumSimultaneousRTs = 1; // only backbuffer RT functional

  return S_OK;
}

HMONITOR STDMETHODCALLTYPE D3D9Interface::GetAdapterMonitor(UINT Adapter) {
  return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice9 **ppReturnedDeviceInterface) {
  if (Adapter != 0 || !pPresentationParameters || !ppReturnedDeviceInterface)
    return D3DERR_INVALIDCALL;

  *ppReturnedDeviceInterface = nullptr;

  try {
    *ppReturnedDeviceInterface = new D3D9Device(this, hFocusWindow, pPresentationParameters);
    return S_OK;
  } catch (...) {
    Logger::err("D3D9: Failed to create device");
    return D3DERR_INVALIDCALL;
  }
}

} // namespace dxmt
