#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_device.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_texture.hpp"
#include "d3d9_fixed_function.hpp"
#include "d3d9_buffer.hpp"
#include "wsi_window.hpp" // full types needed for Com<D3D9VertexBuffer/IndexBuffer> members (GCC)
#include "log/log.hpp"
#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <d3d9.h>

namespace dxmt {

class D3D9Surface;
class D3D9VertexShader;
class D3D9PixelShader;
class D3D9VertexBuffer;
class D3D9IndexBuffer;
class D3D9VertexDeclaration;
class D3D9Texture2D;
class D3D9StateBlock;

class D3D9Device final : public ComObjectWithInitialRef<IDirect3DDevice9> {
  friend class D3D9StateBlock;
public:
  D3D9Device(IDirect3D9 *pD3D9, HWND hFocusWindow, D3DPRESENT_PARAMETERS *pParams);
  ~D3D9Device();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final;

  // IDirect3DDevice9 - essential methods
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() final;
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() final;
  HRESULT STDMETHODCALLTYPE EvictManagedResources() final;
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9) final;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps) final;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) final;

  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) final;
  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) final;
  BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) final;

  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **) final { static bool w=false; if(!w){Logger::warn("D3D9: stub CreateAdditionalSwapChain");w=true;} return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT, IDirect3DSwapChain9 **) final { static bool w=false; if(!w){Logger::warn("D3D9: stub GetSwapChain");w=true;} return D3DERR_INVALIDCALL; }
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() final { return 1; }

  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) final;
  HRESULT STDMETHODCALLTYPE Present(const RECT *, const RECT *, HWND, const RGNDATA *) final;

  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                           IDirect3DSurface9 **ppBackBuffer) final;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS *) final { return D3DERR_INVALIDCALL; }

  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) final { return D3DERR_INVALIDCALL; }
  void STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) final {}
  void STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP *) final {}

  // Texture creation
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                           IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                 IDirect3DVolumeTexture9 **, HANDLE *) final { static bool w=false; if(!w){Logger::warn("D3D9: stub CreateVolumeTexture");w=true;} return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                               IDirect3DCubeTexture9 **, HANDLE *) final { static bool w=false; if(!w){Logger::warn("D3D9: stub CreateCubeTexture");w=true;} return D3DERR_INVALIDCALL; }

  // Buffers
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                                IDirect3DVertexBuffer9 **ppVertexBuffer,
                                                HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                               IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) final;

  // Render targets
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                                D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                                BOOL Lockable, IDirect3DSurface9 **ppSurface,
                                                HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                       D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                                       BOOL Discard, IDirect3DSurface9 **ppSurface,
                                                       HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *, const RECT *,
                                           IDirect3DSurface9 *, const POINT *) final { static bool w=false; if(!w){Logger::warn("D3D9: stub UpdateSurface");w=true;} return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                                           IDirect3DBaseTexture9 *pDestinationTexture) final;
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) final;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT, IDirect3DSurface9 *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
                                         IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
                                         D3DTEXTUREFILTERTYPE Filter) final;
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *, const RECT *, D3DCOLOR) final { static bool w=false; if(!w){Logger::warn("D3D9: stub ColorFill");w=true;} return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                                         IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) final;

  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) final;
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) final;
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) final;
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) final;

  HRESULT STDMETHODCALLTYPE BeginScene() final;
  HRESULT STDMETHODCALLTYPE EndScene() final;

  HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags,
                                   D3DCOLOR Color, float Z, DWORD Stencil) final;

  // Transform/material/light
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) final;
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) final;
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) final;
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *pViewport) final;
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) final;
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *pMaterial) final;
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) final;
  HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9 *pLight) final;
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) final;
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) final;
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable) final;
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD, const float *) final { static bool w=false; if(!w){Logger::warn("D3D9: SetClipPlane not implemented");w=true;} return S_OK; }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD, float *) final { return D3DERR_INVALIDCALL; }

  // Render state
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) final;
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) final;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) final;
  HRESULT STDMETHODCALLTYPE BeginStateBlock() final;
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) final;
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) final;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) final;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) final;
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) final;
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) final;
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) final;
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, const PALETTEENTRY *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *) final { return D3DERR_INVALIDCALL; }

  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *pRect) final {
    if (!pRect) return D3DERR_INVALIDCALL;
    scissor_rect_ = *pRect;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) final {
    if (!pRect) return D3DERR_INVALIDCALL;
    *pRect = scissor_rect_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL) final { return S_OK; }
  BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() final { return FALSE; }
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float) final { return S_OK; }
  float STDMETHODCALLTYPE GetNPatchMode() final { return 0.0f; }

  // Draw calls
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                           UINT PrimitiveCount) final;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                                  UINT MinVertexIndex, UINT NumVertices,
                                                  UINT StartIndex, UINT PrimitiveCount) final;
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                             const void *pVertexStreamZeroData,
                                             UINT VertexStreamZeroStride) final;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                                    UINT NumVertices, UINT PrimitiveCount,
                                                    const void *pIndexData, D3DFORMAT IndexDataFormat,
                                                    const void *pVertexStreamZeroData,
                                                    UINT VertexStreamZeroStride) final;

  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9 *, IDirect3DVertexDeclaration9 *, DWORD) final { static bool w=false; if(!w){Logger::warn("D3D9: stub ProcessVertices");w=true;} return D3DERR_INVALIDCALL; }

  // Vertex declaration & shaders
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements,
                                                     IDirect3DVertexDeclaration9 **ppDecl) final;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) final;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) final;
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) final;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) final;

  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) final;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT, const int *, UINT) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT, int *, UINT) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT, const BOOL *, UINT) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT, BOOL *, UINT) final { return D3DERR_INVALIDCALL; }

  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData,
                                             UINT OffsetInBytes, UINT Stride) final;
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
                                             UINT *pOffsetInBytes, UINT *pStride) final;
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT, UINT) final { static bool w=false; if(!w){Logger::warn("D3D9: SetStreamSourceFreq not implemented");w=true;} return S_OK; }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT, UINT *) final { return D3DERR_INVALIDCALL; }

  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) final;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) final;

  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) final;
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT, const int *, UINT) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT, int *, UINT) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT, const BOOL *, UINT) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT, BOOL *, UINT) final { return D3DERR_INVALIDCALL; }

  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float *, const D3DRECTPATCH_INFO *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float *, const D3DTRIPATCH_INFO *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) final;

  // Internal accessors
  WMT::Device GetMTLDevice() { return dxmt_device_->device(); }
  CommandQueue &GetQueue() { return dxmt_device_->queue(); }

  // Public for use in free-function EmitCommonRenderSetup
  struct ShaderConstants {
    float vs[256][4];
    float ps[256][4];
  };

  struct DrawCapture {
    obj_handle_t pso; // raw handle — lifetime managed by pso_cache_
    WMTPrimitiveType mtlPrimType;
    Rc<Texture> backbuffer;
    TextureViewKey view;
    D3DVIEWPORT9 vp;
    uint32_t slot_mask;
    uint64_t argbuf_size;
    uint64_t vb_region_size;
    struct VBSlotCapture {
      Rc<Buffer> buffer;
      Rc<BufferAllocation> alloc; // retains dynamic VB allocation until GPU finishes
      WMT::Buffer raw_buffer;   // for makeResident on transient/dynamic VBs
      uint64_t gpu_address = 0; // for transient/dynamic VBs
      UINT offset = 0;
      UINT stride = 0;
    };
    std::array<VBSlotCapture, 16> vb_captures;
    ShaderConstants *constants;
    uint64_t vs_const_version;
    uint64_t ps_const_version;
    uint16_t vs_const_count; // max float constant register used by VS (0-256)
    uint16_t ps_const_count; // max float constant register used by PS (0-256)
    uint64_t vs_const_buf_size; // actual VS constant buffer size in bytes (right-sized)
    bool depth_enable;
    bool depth_write;
    WMTCompareFunction depth_func;
    Rc<Texture> depth_tex;
    TextureViewKey depth_view;
    WMTCullMode cull_mode;
    uint32_t stencil_ref;
    obj_handle_t dsso;
    struct TexCapture {
      // Owning reference: the game may unbind and free a texture right after the draw call, before the
      // encoder thread has used this capture (a raw pointer here caused a use-after-free of the MTLTexture
      // that crashed Metal's completion thread = freeze on the first gameplay frame in GTA SA).
      Rc<Texture> texture;
      TextureViewKey viewKey;
      uint32_t stage;
    };
    std::array<TexCapture, 16> texCaptures;
    uint8_t texCaptureCount = 0;
    obj_handle_t samplerHandles[16] = {};
    uint64_t tex_sampler_fingerprint = 0;
    bool scissor_enable;
    RECT scissor_rect;
  };

  DrawCapture BuildDrawCapture(WMTPrimitiveType mtlPrimType);

  // Batched draw for render pass coalescing
  struct BatchedDraw {
    DrawCapture cap;
    enum Type { kNonIndexed, kIndexed } type;
    UINT vertexStart;
    UINT vertexCount;
    UINT indexCount;
    WMTIndexType indexType;
    Rc<Buffer> ibBuffer;
    Rc<BufferAllocation> ibAlloc; // retains dynamic IB allocation until GPU finishes
    WMT::Buffer ibRawBuffer; // for transient/dynamic IBs
    uint64_t ibOffset;
    INT baseVertex;
  };

  void FlushDrawBatch();
  void QueueBatchedDraw(BatchedDraw &&draw);
  void BeginOcclusionQuery(Rc<VisibilityResultQuery> query);
  void EndOcclusionQuery(Rc<VisibilityResultQuery> query);
  void FlushForQuery();
  size_t PendingDrawCount() const { return pending_draws_.size(); }

private:
  HRESULT CreateBackbuffer(UINT width, UINT height);
  obj_handle_t CreatePSO();
  void UpdateStatistics(const FrameStatisticsContainer &statistics, uint64_t frame_id);

  static uint32_t TransformIndex(D3DTRANSFORMSTATETYPE state);

  Com<IDirect3D9> d3d9_;
  HWND hwnd_;
  D3DPRESENT_PARAMETERS present_params_;
  // Fullscreen: window restyled/resized to the backbuffer (like wined3d/DXGI do)
  wsi::DXMTWindowState window_state_ = {};
  bool fullscreen_window_ = false;
  void ApplyFullscreenWindow();

  std::unique_ptr<Device> dxmt_device_;
  WMT::Object native_view_;
  WMT::MetalLayer layer_weak_;
  Rc<Presenter> presenter_;
  HUDState hud_;

  Rc<Texture> backbuffer_;
  TextureViewKey backbuffer_view_ = 0;
  Com<D3D9Surface> backbuffer_surface_;

  D3DVIEWPORT9 viewport_ = {};
  RECT scissor_rect_ = {};
  double init_refresh_rate_ = 60.0;

  // Pipeline state
  Com<D3D9VertexShader> current_vs_;
  Com<D3D9PixelShader> current_ps_;
  Com<D3D9VertexDeclaration> current_vdecl_;
  DWORD current_fvf_ = 0;
  Com<D3D9VertexBuffer> stream_sources_[16] = {};
  UINT stream_offsets_[16] = {};
  UINT stream_strides_[16] = {};
  Com<D3D9IndexBuffer> current_ib_;

  // FVF → vertex declaration cache
  std::unordered_map<DWORD, Com<D3D9VertexDeclaration>> fvf_cache_;

  // Render states
  DWORD render_states_[256] = {};

  // Texture bindings
  Com<D3D9Texture2D> bound_textures_[16];
  uint16_t tex_bound_mask_ = 0; // bitmask of stages with bound textures
  DWORD sampler_states_[16][14] = {};

  // Default 1x1 white texture for unbound sampler slots (D3D9 spec: sample returns white)
  Rc<Texture> default_white_tex_;
  TextureViewKey default_white_view_ = 0;
  obj_handle_t default_sampler_ = 0;

  // Texture stage states (8 stages, 33 state types)
  DWORD texture_stage_states_[8][33] = {};

  // Transform matrices (indexed via TransformIndex)
  D3DMATRIX transforms_[512] = {};

  // Material
  D3DMATERIAL9 material_ = {};

  // Lights
  static constexpr DWORD kMaxLights = 32; // D3D9 allows any light index; only 8 may be active (consumers cap at 8)
  D3DLIGHT9 lights_[kMaxLights] = {};
  BOOL light_enabled_[kMaxLights] = {};

  // Current render target (defaults to backbuffer)
  Com<D3D9Surface> current_rt_surface_;
  Rc<Texture> current_rt_;
  TextureViewKey current_rt_view_ = 0;
  WMTPixelFormat current_rt_format_ = WMTPixelFormatBGRA8Unorm;

  // Depth/stencil
  Rc<Texture> depth_stencil_;
  TextureViewKey depth_stencil_view_ = 0;
  WMTPixelFormat depth_stencil_format_ = WMTPixelFormatInvalid;

  // Shader constant registers + dirty version counters
  float vsConstants_[256][4] = {};
  float psConstants_[256][4] = {};
  uint64_t vs_const_version_ = 1;
  uint64_t ps_const_version_ = 1;
  uint64_t ff_const_version_ = 1; // bumped by SetTransform, SetLight, SetMaterial, etc.

  // Dirty flags — set by D3D9 state setters, cleared in BuildDrawCapture
  bool pso_dirty_ = true;
  obj_handle_t cached_pso_ = 0;
  bool vb_dirty_ = true;    // set by SetStreamSource
  bool tex_dirty_ = true;   // set by SetTexture, SetSamplerState, SetPixelShader, SetVertexShader
  bool vp_dirty_ = true;    // set by SetViewport
  bool scissor_dirty_ = true; // set by SetScissorRect

  // Shadow draw state — reused for non-dirty categories across draws
  DrawCapture shadow_cap_ = {};
  bool shadow_valid_ = false;

  // Depth/stencil surface (GPU-backed, set via SetDepthStencilSurface or auto-created)
  Com<D3D9Surface> depth_stencil_surface_;

  // PSO cache (keyed by VS/PS function handles + vertex declaration + blend state + depth format)
  struct PSOKey {
    obj_handle_t vs_func;  // MTLFunction handle (DXSO or FF)
    obj_handle_t ps_func;
    D3D9VertexDeclaration *vdecl;
    DWORD blend_enable;
    DWORD src_blend;
    DWORD dest_blend;
    DWORD blend_op;
    DWORD src_blend_alpha;
    DWORD dest_blend_alpha;
    DWORD blend_op_alpha;
    DWORD depth_format; // 0 = no depth, else pixel format
    uint8_t alpha_func; // 0 = disabled, 1-8 = D3DCMP_*
    bool srgb_write;
    uint8_t color_write_mask; // D3DRS_COLORWRITEENABLE (4 bits)
    bool operator==(const PSOKey &) const = default;
  };
  struct PSOKeyHash {
    size_t operator()(const PSOKey &k) const {
      size_t h = std::hash<uintptr_t>{}((uintptr_t)k.vs_func);
      h ^= std::hash<uintptr_t>{}((uintptr_t)k.ps_func) * 2654435761u;
      h ^= std::hash<uintptr_t>{}((uintptr_t)k.vdecl) * 40499u;
      h ^= std::hash<uint64_t>{}(
        ((uint64_t)k.blend_enable) | ((uint64_t)k.src_blend << 8) |
        ((uint64_t)k.dest_blend << 16) | ((uint64_t)k.depth_format << 24) |
        ((uint64_t)k.alpha_func << 56) | ((uint64_t)k.srgb_write << 57) |
        ((uint64_t)k.color_write_mask << 58)) * 2246822519u;
      h ^= std::hash<uint64_t>{}(
        ((uint64_t)k.blend_op) | ((uint64_t)k.src_blend_alpha << 8) |
        ((uint64_t)k.dest_blend_alpha << 16) | ((uint64_t)k.blend_op_alpha << 24)) * 2654435769u;
      return h;
    }
  };
  std::unordered_map<PSOKey, WMT::Reference<WMT::RenderPipelineState>, PSOKeyHash> pso_cache_;
  PSOKey cached_pso_key_ = {};

  // Constant snapshot ring — no per-draw allocation
  // Worst case: 32 chunks × 512 draws/chunk (4MB argbuf / 8KB per draw) = 16384
  static constexpr uint32_t kSnapshotRingSize = 16384;
  std::unique_ptr<ShaderConstants[]> snapshot_ring_;
  uint32_t snapshot_ring_head_ = 0;
  ShaderConstants *current_const_snapshot_ = nullptr;
  uint64_t snapshot_vs_version_ = ~0ULL;
  uint64_t snapshot_ps_version_ = ~0ULL;
  uint16_t snapshot_vs_count_ = 0, snapshot_ps_count_ = 0;

  // Persistent FF VS constant buffer — only changed sections updated per draw
  // FF VS constants; layout shared with airconv d3d9_ff_shader.cpp (c[100+i] = light i specular)
  static constexpr uint32_t kFFVSConstCount = 108;
  float cached_ff_vs_[kFFVSConstCount][4] = {};
  float cached_ff_ps_[3][4] = {};
  enum FFDirty : uint32_t {
    kFFDirtyWorld      = 1 << 0, // c[0..3], c[12..15], c[16..19]
    kFFDirtyView       = 1 << 1, // c[4..7], c[12..15], c[16..19], lights
    kFFDirtyProj       = 1 << 2, // c[8..11], c[16..19]
    kFFDirtyViewport   = 1 << 3, // c[20]
    kFFDirtyFog        = 1 << 4, // c[21] (VS), c[0..2] (PS)
    kFFDirtyAmbient    = 1 << 5, // c[22]
    kFFDirtyMaterial   = 1 << 6, // c[23..27]
    kFFDirtyLights     = 1 << 7, // c[28..67]
    kFFDirtyTexMat     = 1 << 8, // c[68..99]
    kFFDirtyPS         = 1 << 9, // FF PS constants (texfactor, fog, alpha)
    kFFDirtyAll        = 0x3FF,
  };
  uint32_t ff_dirty_ = kFFDirtyAll;

  // Draw batching — consecutive draws sharing the same RT/depth are batched into one render pass
  std::vector<BatchedDraw> pending_draws_;
  Texture *batch_rt_ = nullptr;
  Texture *batch_depth_ = nullptr;
  uint64_t batch_argbuf_size_ = 0;
  bool needs_query_flush_ = false;

  clock::time_point frame_start_{clock::now()};

  // State block recording
  bool recording_state_block_ = false;

  // Depth-stencil state cache (keyed by packed DS state)
  struct DSKey {
    WMTCompareFunction depth_func;
    bool depth_write;
    bool stencil_enabled;
    WMTCompareFunction stencil_func;
    WMTStencilOperation stencil_pass;
    WMTStencilOperation stencil_fail;
    WMTStencilOperation depth_fail;
    uint8_t stencil_read_mask;
    uint8_t stencil_write_mask;
    bool operator==(const DSKey &) const = default;
  };
  struct DSKeyHash {
    size_t operator()(const DSKey &k) const {
      uint64_t v = ((uint64_t)k.depth_func) | ((uint64_t)k.depth_write << 4) |
                   ((uint64_t)k.stencil_enabled << 5) | ((uint64_t)k.stencil_func << 6) |
                   ((uint64_t)k.stencil_pass << 10) | ((uint64_t)k.stencil_fail << 14) |
                   ((uint64_t)k.depth_fail << 18) | ((uint64_t)k.stencil_read_mask << 22) |
                   ((uint64_t)k.stencil_write_mask << 30);
      return std::hash<uint64_t>{}(v);
    }
  };
  std::unordered_map<DSKey, obj_handle_t, DSKeyHash> ds_cache_;
  bool dsso_dirty_ = true;
  obj_handle_t cached_dsso_ = 0;
  uint32_t cached_stencil_ref_ = 0;

  // Sampler state cache (keyed by the 14-DWORD sampler state array)
  struct SamplerKey {
    DWORD state[14];
    bool operator==(const SamplerKey &) const = default;
  };
  struct SamplerKeyHash {
    size_t operator()(const SamplerKey &k) const {
      size_t h = 0;
      for (int i = 0; i < 14; i++)
        h ^= std::hash<DWORD>{}(k.state[i]) * (2654435761u + i * 40499u);
      return h;
    }
  };
  std::unordered_map<SamplerKey, obj_handle_t, SamplerKeyHash> sampler_cache_;

  // Cursor state
  HCURSOR cursor_handle_ = nullptr;
  BOOL cursor_visible_ = TRUE;

  // Fixed-function shader caches
  // Keyed by FFVSKey bytes + vertex layout (elements + stream mask): the generated FF VS hard-codes element
  // streams/offsets/types, so two layouts with the same FFVSKey must not share a shader.
  std::map<std::string, WMT::Reference<WMT::Function>> ff_vs_cache_;
  std::map<FFPSKey, WMT::Reference<WMT::Function>> ff_ps_cache_;

  FFVSKey BuildFFVSKey();
  FFPSKey BuildFFPSKey();
  void FillFFVSTexcoordModes(FFVSKey &key);
  WMT::Reference<WMT::Function> GetOrCreateFFVS(const FFVSKey &key, D3D9VertexDeclaration *vdecl);
  WMT::Reference<WMT::Function> GetOrCreateFFPS(const FFPSKey &key);
  void UpdateFFConstants(); // incremental update of cached_ff_vs_/ps_ using ff_dirty_
};

} // namespace dxmt
