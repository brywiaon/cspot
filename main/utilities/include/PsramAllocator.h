#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace bell {
template <typename T>
class PsramAllocator {
 public:
  using value_type = T;

  PsramAllocator() noexcept = default;

  template <typename U>
  PsramAllocator(const PsramAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      return nullptr;
    }

    void* ptr = nullptr;
#if defined(ESP_PLATFORM)
    ptr = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!ptr) {
      ptr = std::malloc(n * sizeof(T));
    }
    return static_cast<T*>(ptr);
  }

  void deallocate(T* ptr, std::size_t) noexcept {
#if defined(ESP_PLATFORM)
    heap_caps_free(ptr);
#else
    std::free(ptr);
#endif
  }

  template <typename U>
  bool operator==(const PsramAllocator<U>&) const noexcept {
    return true;
  }

  template <typename U>
  bool operator!=(const PsramAllocator<U>&) const noexcept {
    return false;
  }
};
}  // namespace bell
