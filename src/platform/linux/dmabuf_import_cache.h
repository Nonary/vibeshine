/**
 * @file src/platform/linux/dmabuf_import_cache.h
 * @brief Identity keys and a small MRU cache for zero-copy DMA-BUF imports.
 */
#pragma once

#include "dmabuf_surface.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace egl {
  struct dmabuf_import_key_t {
    int width {};
    int height {};
    std::uint32_t fourcc {};
    std::uint64_t modifier {};
    std::array<std::uint64_t, 4> dev {};
    std::array<std::uint64_t, 4> ino {};
    std::array<std::uint32_t, 4> pitches {};
    std::array<std::uint32_t, 4> offsets {};
    std::array<bool, 4> present {};

    bool operator==(const dmabuf_import_key_t &) const = default;
  };

  inline std::optional<dmabuf_import_key_t> make_dmabuf_import_key(const surface_descriptor_t &surface) noexcept {
    dmabuf_import_key_t key;
    key.width = surface.width;
    key.height = surface.height;
    key.fourcc = surface.fourcc;
    key.modifier = surface.modifier;

    bool any_fd = false;
    for (std::size_t i = 0; i < 4; ++i) {
      key.pitches[i] = surface.pitches[i];
      key.offsets[i] = surface.offsets[i];

      if (surface.fds[i] < 0) {
        continue;
      }

      struct stat st {};
      if (fstat(surface.fds[i], &st) != 0) {
        return std::nullopt;
      }

      key.dev[i] = static_cast<std::uint64_t>(st.st_dev);
      key.ino[i] = static_cast<std::uint64_t>(st.st_ino);
      key.present[i] = true;
      any_fd = true;
    }

    if (!any_fd) {
      return std::nullopt;
    }

    return key;
  }

  // Pointers returned by find() and insert() stay valid only until the next
  // insert() or clear().
  template<class Value, std::size_t Capacity = 4>
  class dmabuf_import_cache_t {
    static_assert(Capacity > 0, "dmabuf_import_cache_t requires a non-zero capacity");

  public:
    dmabuf_import_cache_t() {
      entries.reserve(Capacity);
    }

    Value *find(const dmabuf_import_key_t &key) {
      for (auto &entry : entries) {
        if (entry.key == key) {
          entry.last_used = ++clock;
          return &entry.value;
        }
      }
      return nullptr;
    }

    Value *insert(const dmabuf_import_key_t &key, Value &&value) {
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->key == key) {
          entries.erase(it);
          break;
        }
      }

      if (entries.size() >= Capacity) {
        auto oldest = entries.begin();
        for (auto it = entries.begin() + 1; it != entries.end(); ++it) {
          if (it->last_used < oldest->last_used) {
            oldest = it;
          }
        }
        entries.erase(oldest);
      }

      entries.push_back(entry_t {key, std::move(value), ++clock});
      return &entries.back().value;
    }

    void clear() noexcept {
      entries.clear();
    }

    std::size_t size() const noexcept {
      return entries.size();
    }

  private:
    struct entry_t {
      dmabuf_import_key_t key;
      Value value;
      std::uint64_t last_used;
    };

    std::vector<entry_t> entries;
    std::uint64_t clock {};
  };
}  // namespace egl
