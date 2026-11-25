#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9_device.hpp"
#include <d3d9.h>
#include <cstring>

namespace dxmt {

class D3D9VertexShader;
class D3D9PixelShader;
class D3D9VertexDeclaration;
class D3D9VertexBuffer;
class D3D9IndexBuffer;
class D3D9Texture2D;

class D3D9StateBlock final : public ComObjectClamp<IDirect3DStateBlock9> {
public:
  D3D9StateBlock(D3D9Device *device) : device_(device) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Capture() final;
  HRESULT STDMETHODCALLTYPE Apply() final;

private:
  friend class D3D9Device;
  D3D9Device *device_;

  // Captured state
  DWORD render_states[256] = {};
  DWORD texture_stage_states[8][33] = {};
  DWORD sampler_states[16][14] = {};
  D3DMATRIX transforms[512] = {};
  D3DVIEWPORT9 viewport = {};
  RECT scissor_rect = {};
  D3DMATERIAL9 material = {};
  D3DLIGHT9 lights[32] = {};       // must match D3D9Device::kMaxLights
  BOOL light_enabled[32] = {};
  DWORD fvf = 0;
  float vs_constants[256][4] = {};
  float ps_constants[256][4] = {};
  Com<D3D9VertexShader> vs;
  Com<D3D9PixelShader> ps;
  Com<D3D9VertexDeclaration> vdecl;
  Com<D3D9VertexBuffer> stream_sources[16] = {};
  UINT stream_offsets[16] = {};
  UINT stream_strides[16] = {};
  Com<D3D9IndexBuffer> ib;
  Com<D3D9Texture2D> textures[16] = {};
};

} // namespace dxmt
