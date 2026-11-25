#pragma once

// Recycling pool for CPU-placed Metal buffers (i386 / wow64 path).
//
// On i386, every CPU-visible buffer is CPU memory from wsi::aligned_malloc wrapped by Metal with
// newBufferWithBytesNoCopy. Measured on macOS 26 / M3 (GTA SA): after such an MTLBuffer is released and
// the memory freed, the C runtime soon hands the same pages out again, and the first CPU write to them
// stalls for 1-6 s (sometimes ~20 s) while the kernel is still tearing down the GPU mapping. The stall is
// per-page (other threads keep running) and does not depend on memory pressure or rosettax87_jit.
//
// So never free memory that Metal has wrapped while the game runs: keep the (memory, MTLBuffer) pair
// and hand it out again for the next allocation with the same size and options.

#include "Metal.hpp"
#include "thread.hpp"
#include "wsi_platform.hpp"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxmt::placed_pool {

struct Entry {
  void *memory = nullptr;
  size_t size = 0;
  WMTResourceOptions options{};
  WMT::Reference<WMT::Buffer> buffer;
  uint64_t gpu_address = 0;
};

// Upper bound for idle pooled memory. Over the cap an entry is really freed (the old, stall-prone path).
constexpr size_t kMaxPooledBytes = 256ull << 20;

struct State {
  dxmt::mutex mutex;
  std::unordered_map<uint64_t, std::vector<Entry>> free;
  size_t pooled_bytes = 0;
  uint64_t hits = 0, misses = 0, overflow_frees = 0;
};

inline State &
state() {
  static State s;
  return s;
}

inline uint64_t
key(size_t size, WMTResourceOptions options) {
  return (uint64_t(options) << 40) ^ uint64_t(size);
}

// Returns true and fills `out` when a pooled pair of exactly this size and these options exists.
inline bool
acquire(size_t size, WMTResourceOptions options, Entry &out) {
  auto &s = state();
  std::lock_guard<dxmt::mutex> lock(s.mutex);
  auto it = s.free.find(key(size, options));
  if (it == s.free.end() || it->second.empty()) {
    s.misses++;
    return false;
  }
  out = std::move(it->second.back());
  it->second.pop_back();
  s.pooled_bytes -= out.size;
  s.hits++;
  return true;
}

// Takes ownership of the memory and the buffer. Only call once the GPU no longer uses them.
inline void
release(Entry &&e) {
  if (!e.memory)
    return;
  auto &s = state();
  {
    std::lock_guard<dxmt::mutex> lock(s.mutex);
    if (s.pooled_bytes + e.size <= kMaxPooledBytes) {
      s.pooled_bytes += e.size;
      s.free[key(e.size, e.options)].push_back(std::move(e));
      return;
    }
    s.overflow_frees++;
  }
  // over the cap: drop the MTLBuffer first, then free the memory
  e.buffer = WMT::Reference<WMT::Buffer>();
  wsi::aligned_free(e.memory);
  e.memory = nullptr;
}

} // namespace dxmt::placed_pool
