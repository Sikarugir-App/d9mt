
#include "d3d9_buffer.hpp"
#include "d3d9_device.hpp"
#include <cstring>

namespace dxmt {

D3D9VertexBuffer::D3D9VertexBuffer(
    D3D9Device *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    Rc<Buffer> buffer, Rc<BufferAllocation> allocation)
    : device_(device), length_(length), usage_(usage), fvf_(fvf), pool_(pool),
      buffer_(std::move(buffer)), allocation_(std::move(allocation)) {}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexBuffer9) ||
      riid == __uuidof(IDirect3DResource9)) {
    *ppvObj = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::Lock(
    UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  if (OffsetToLock > length_ || (SizeToLock && OffsetToLock + SizeToLock > length_)) {
    static int reported = 0;
    if (reported++ < 20)
      Logger::err(str::format("D3D9: D3D9VertexBuffer::Lock out of range: offset=", OffsetToLock, " size=", SizeToLock, " length=", length_, " flags=", Flags));
  }
#ifdef DXMT_PERF
  auto perf_t0 = clock::now();
#endif
  if (!ppbData) return D3DERR_INVALIDCALL;

  if ((Flags & D3DLOCK_DISCARD) && (usage_ & D3DUSAGE_DYNAMIC)) {
    // Allocate fresh backing store; old allocation stays alive via draw captures
    dxmt::Flags<BufferAllocationFlag> allocFlags;
    allocFlags.set(BufferAllocationFlag::CpuPlaced);
    allocation_ = buffer_->allocate(allocFlags);
    buffer_->rename(Rc(allocation_));
  } else if (!(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
    // Non-DISCARD write: rename allocation so GPU keeps reading old data
    // while CPU writes to a fresh copy. Old allocation stays alive via draw captures.
    dxmt::Flags<BufferAllocationFlag> allocFlags;
    allocFlags.set(BufferAllocationFlag::CpuPlaced);
    auto new_alloc = buffer_->allocate(allocFlags);
    memcpy(new_alloc->mappedMemory(0), allocation_->mappedMemory(0), length_);
    allocation_ = std::move(new_alloc);
    buffer_->rename(Rc(allocation_));
  }

  void *mapped = allocation_->mappedMemory(0);
  if (!mapped) return D3DERR_INVALIDCALL;

  *ppbData = static_cast<uint8_t *>(mapped) + OffsetToLock;
#ifdef DXMT_PERF
  device_->GetQueue().CurrentFrameStatistics().d3d9_lock_time += clock::now() - perf_t0;
#endif
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::Unlock() {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  pDesc->Format = D3DFMT_VERTEXDATA;
  pDesc->Type = D3DRTYPE_VERTEXBUFFER;
  pDesc->Usage = usage_;
  pDesc->Pool = pool_;
  pDesc->Size = length_;
  pDesc->FVF = fvf_;
  return S_OK;
}

// D3D9IndexBuffer

D3D9IndexBuffer::D3D9IndexBuffer(
    D3D9Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    Rc<Buffer> buffer, Rc<BufferAllocation> allocation)
    : device_(device), length_(length), usage_(usage), format_(format), pool_(pool),
      buffer_(std::move(buffer)), allocation_(std::move(allocation)) {}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DIndexBuffer9) ||
      riid == __uuidof(IDirect3DResource9)) {
    *ppvObj = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::Lock(
    UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  if (OffsetToLock > length_ || (SizeToLock && OffsetToLock + SizeToLock > length_)) {
    static int reported = 0;
    if (reported++ < 20)
      Logger::err(str::format("D3D9: D3D9IndexBuffer::Lock out of range: offset=", OffsetToLock, " size=", SizeToLock, " length=", length_, " flags=", Flags));
  }
#ifdef DXMT_PERF
  auto perf_t0 = clock::now();
#endif
  if (!ppbData) return D3DERR_INVALIDCALL;

  if ((Flags & D3DLOCK_DISCARD) && (usage_ & D3DUSAGE_DYNAMIC)) {
    dxmt::Flags<BufferAllocationFlag> allocFlags;
    allocFlags.set(BufferAllocationFlag::CpuPlaced);
    allocation_ = buffer_->allocate(allocFlags);
    buffer_->rename(Rc(allocation_));
  } else if (!(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
    dxmt::Flags<BufferAllocationFlag> allocFlags;
    allocFlags.set(BufferAllocationFlag::CpuPlaced);
    auto new_alloc = buffer_->allocate(allocFlags);
    memcpy(new_alloc->mappedMemory(0), allocation_->mappedMemory(0), length_);
    allocation_ = std::move(new_alloc);
    buffer_->rename(Rc(allocation_));
  }

  void *mapped = allocation_->mappedMemory(0);
  if (!mapped) return D3DERR_INVALIDCALL;

  *ppbData = static_cast<uint8_t *>(mapped) + OffsetToLock;
#ifdef DXMT_PERF
  device_->GetQueue().CurrentFrameStatistics().d3d9_lock_time += clock::now() - perf_t0;
#endif
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::Unlock() {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  pDesc->Format = format_;
  pDesc->Type = D3DRTYPE_INDEXBUFFER;
  pDesc->Usage = usage_;
  pDesc->Pool = pool_;
  pDesc->Size = length_;
  return S_OK;
}

} // namespace dxmt
