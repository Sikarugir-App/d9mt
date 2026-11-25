#include "wsi_platform.hpp"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxmt::wsi {

// All memory we hand to Metal (newBufferWithBytesNoCopy) lives in regions that contain nothing else,
// aligned to 16 KB host pages:
// - blocks >= 64 KB come straight from VirtualAlloc (64 KB aligned, rounded to 16 KB);
// - smaller blocks are carved in 4 KB steps out of 2 MB VirtualAlloc'd arena chunks (with per-size free lists).
// winemetal's unix side then drops EXECUTE on the whole host pages around each wrapped range. With DEP off,
// Wine maps everything rwx and Rosetta trapped every write to one of our hot pages (~100k faults/s); small
// buffers used to share heap pages with unrelated data, so they could not be protected safely.
static constexpr size_t kVirtualAllocThreshold = 64 * 1024;
static constexpr size_t kHostPage = 16 * 1024;
static constexpr size_t kArenaChunk = 2 * 1024 * 1024;
static constexpr size_t kArenaGranule = 4096;

namespace {

struct SmallArena {
  std::mutex mutex;
  char *chunk = nullptr;      // current chunk
  size_t chunk_used = 0;
  std::vector<std::pair<char *, char *>> chunks; // [begin, end) of every chunk
  std::unordered_map<void *, size_t> live;       // ptr -> rounded size
  std::unordered_map<size_t, std::vector<void *>> free_lists;

  bool
  owns(void *p) {
    for (auto &c : chunks)
      if ((char *)p >= c.first && (char *)p < c.second)
        return true;
    return false;
  }

  void *
  alloc(size_t size) {
    size_t rounded = (size + kArenaGranule - 1) & ~(kArenaGranule - 1);
    std::lock_guard<std::mutex> lock(mutex);
    auto &fl = free_lists[rounded];
    if (!fl.empty()) {
      void *p = fl.back();
      fl.pop_back();
      live[p] = rounded;
      return p;
    }
    if (!chunk || chunk_used + rounded > kArenaChunk) {
      chunk = (char *)VirtualAlloc(nullptr, kArenaChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (!chunk)
        return nullptr;
      chunks.emplace_back(chunk, chunk + kArenaChunk);
      chunk_used = 0;
    }
    void *p = chunk + chunk_used;
    chunk_used += rounded;
    live[p] = rounded;
    return p;
  }

  bool
  free(void *p) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = live.find(p);
    if (it == live.end())
      return false;
    free_lists[it->second].push_back(p);
    live.erase(it);
    return true;
  }
};

SmallArena &
arena() {
  static SmallArena a;
  return a;
}

} // namespace

void *
aligned_malloc(size_t size, size_t alignment) {
  void *ptr;
  size_t touched = size;
  if (size >= kVirtualAllocThreshold && alignment <= 64 * 1024) {
    touched = (size + kHostPage - 1) & ~(kHostPage - 1);
    ptr = VirtualAlloc(nullptr, touched, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  } else if (alignment <= kArenaGranule) {
    ptr = arena().alloc(size);
  } else {
    ptr = _aligned_malloc(size, alignment);
  }
  // Pre-fault every page now, before Metal wraps the memory.
  if (ptr)
    std::memset(ptr, 0, touched);
  return ptr;
}

void
aligned_free(void *ptr) {
  if (!ptr)
    return;
  if (arena().free(ptr))
    return;
  // VirtualAlloc'd blocks are region bases; _aligned_malloc pointers never are (heap header precedes them).
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) && mbi.AllocationBase == ptr && mbi.Type == MEM_PRIVATE) {
    VirtualFree(ptr, 0, MEM_RELEASE);
    return;
  }
  _aligned_free(ptr);
}

} // namespace dxmt::wsi
