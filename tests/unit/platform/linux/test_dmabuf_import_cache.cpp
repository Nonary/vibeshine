/**
 * @file tests/unit/platform/linux/test_dmabuf_import_cache.cpp
 * @brief DMA-BUF identity keys and MRU import-cache behaviour using real file descriptors.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/dmabuf_import_cache.h>

#include <sys/mman.h>
#include <unistd.h>

namespace {
  egl::surface_descriptor_t make_surface(int fd, std::uint32_t pitch = 256) {
    egl::surface_descriptor_t surface;
    surface.width = 64;
    surface.height = 64;
    surface.fourcc = 0x34325258;
    surface.modifier = 0;
    surface.fds[0] = fd;
    surface.pitches[0] = pitch;
    surface.offsets[0] = 0;
    return surface;
  }

  struct memfd_t {
    explicit memfd_t(const char *name):
        fd(memfd_create(name, MFD_CLOEXEC)) {
    }

    ~memfd_t() {
      if (fd >= 0) {
        close(fd);
      }
    }

    memfd_t(const memfd_t &) = delete;
    memfd_t &operator=(const memfd_t &) = delete;

    int fd;
  };

  TEST(DmabufImportCache, KeyIsEqualForDuplicatedFdOfSameBuffer) {
    memfd_t memfd("x");
    ASSERT_GE(memfd.fd, 0);
    const int duplicate = dup(memfd.fd);
    ASSERT_GE(duplicate, 0);

    const auto first = egl::make_dmabuf_import_key(make_surface(memfd.fd));
    const auto second = egl::make_dmabuf_import_key(make_surface(duplicate));

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);

    close(duplicate);
  }

  TEST(DmabufImportCache, KeyDiffersForDifferentBuffers) {
    memfd_t first_memfd("first");
    memfd_t second_memfd("second");
    ASSERT_GE(first_memfd.fd, 0);
    ASSERT_GE(second_memfd.fd, 0);

    const auto first = egl::make_dmabuf_import_key(make_surface(first_memfd.fd));
    const auto second = egl::make_dmabuf_import_key(make_surface(second_memfd.fd));

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
  }

  TEST(DmabufImportCache, KeyDiffersWhenLayoutChanges) {
    memfd_t memfd("layout");
    ASSERT_GE(memfd.fd, 0);

    const auto surface = make_surface(memfd.fd);
    const auto base = egl::make_dmabuf_import_key(surface);

    auto changed_pitch = surface;
    changed_pitch.pitches[0] += 64;
    const auto with_changed_pitch = egl::make_dmabuf_import_key(changed_pitch);

    auto changed_modifier = surface;
    changed_modifier.modifier = 0x0100000000000001ULL;
    const auto with_changed_modifier = egl::make_dmabuf_import_key(changed_modifier);

    ASSERT_TRUE(base);
    ASSERT_TRUE(with_changed_pitch);
    ASSERT_TRUE(with_changed_modifier);
    EXPECT_NE(*base, *with_changed_pitch);
    EXPECT_NE(*base, *with_changed_modifier);
  }

  TEST(DmabufImportCache, KeyRequiresAnOpenFd) {
    egl::surface_descriptor_t no_fds;
    no_fds.width = 64;
    no_fds.height = 64;
    no_fds.fourcc = 0x34325258;
    EXPECT_FALSE(egl::make_dmabuf_import_key(no_fds));

    int closed_fd = -1;
    {
      memfd_t memfd("closed");
      ASSERT_GE(memfd.fd, 0);
      closed_fd = memfd.fd;
    }

    auto closed_surface = make_surface(closed_fd);
    EXPECT_FALSE(egl::make_dmabuf_import_key(closed_surface));
  }

  TEST(DmabufImportCache, CacheFindsInsertedValue) {
    memfd_t memfd("cache");
    ASSERT_GE(memfd.fd, 0);
    const auto key = egl::make_dmabuf_import_key(make_surface(memfd.fd));
    ASSERT_TRUE(key);

    egl::dmabuf_import_cache_t<int, 2> cache;
    EXPECT_EQ(cache.find(*key), nullptr);

    cache.insert(*key, 1);
    const int *value = cache.find(*key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 1);
    EXPECT_EQ(cache.size(), 1u);
  }

  TEST(DmabufImportCache, CacheEvictsLeastRecentlyUsed) {
    memfd_t first_memfd("a");
    memfd_t second_memfd("b");
    memfd_t third_memfd("c");
    ASSERT_GE(first_memfd.fd, 0);
    ASSERT_GE(second_memfd.fd, 0);
    ASSERT_GE(third_memfd.fd, 0);

    const auto key_a = egl::make_dmabuf_import_key(make_surface(first_memfd.fd));
    const auto key_b = egl::make_dmabuf_import_key(make_surface(second_memfd.fd));
    const auto key_c = egl::make_dmabuf_import_key(make_surface(third_memfd.fd));
    ASSERT_TRUE(key_a);
    ASSERT_TRUE(key_b);
    ASSERT_TRUE(key_c);

    egl::dmabuf_import_cache_t<int, 2> cache;
    cache.insert(*key_a, 1);
    cache.insert(*key_b, 2);
    ASSERT_NE(cache.find(*key_a), nullptr);

    cache.insert(*key_c, 3);
    EXPECT_EQ(cache.find(*key_b), nullptr);
    EXPECT_NE(cache.find(*key_a), nullptr);
    EXPECT_NE(cache.find(*key_c), nullptr);
    EXPECT_EQ(cache.size(), 2u);
  }

  TEST(DmabufImportCache, CacheReplacesSameKeyAndClears) {
    memfd_t memfd("a");
    ASSERT_GE(memfd.fd, 0);

    const auto key_a = egl::make_dmabuf_import_key(make_surface(memfd.fd));
    ASSERT_TRUE(key_a);

    egl::dmabuf_import_cache_t<int, 2> cache;
    cache.insert(*key_a, 1);
    cache.insert(*key_a, 5);

    EXPECT_EQ(cache.size(), 1u);
    const int *value = cache.find(*key_a);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 5);

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.find(*key_a), nullptr);
  }
}  // namespace
