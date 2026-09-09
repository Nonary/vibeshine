// Implicit Vulkan layer: all presentation hooks are inert without a live
// stream lease. Do not link this library against the Vulkan loader.
#include "global_fps_policy.h"

#include <vulkan/vk_layer.h>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace {
  using namespace platf::global_fps;
  template<class T> void *key(T handle) { return *reinterpret_cast<void **>(handle); }
  struct instance_t {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_GetPhysicalDeviceProcAddr gpdpa;
  };
  struct swapchain_t {
    std::mutex mutex;
    pacer_t pacer;
  };
  struct device_t {
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkDestroyDevice destroy;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<swapchain_t>> swapchains;
  };
  std::mutex dispatch_mutex;
  std::unordered_map<void *, instance_t> instances;
  std::unordered_map<void *, std::shared_ptr<device_t>> devices;

  bool excluded_process() {
    char path[4096] {};
    const auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return true;
    const auto *name = std::strrchr(path, '/');
    if (!name) return true;
    ++name;
    for (const auto *excluded : {"sunshine", "vibeshine", "vibeshine-host", "kwin_wayland",
           "kwin_x11", "gamescope", "gnome-shell", "Xwayland", "Xorg", "weston", "cage",
           "steam", "steamwebhelper", "sway", "river", "labwc", "wayfire", "niri", "cosmic-comp"}) {
      if (std::strcmp(name, excluded) == 0) return true;
    }
    return false;
  }

  std::uint32_t read_lease(std::int64_t now) {
    const char *home_directory = std::getenv("HOME");
    const auto path = lease_path(home_directory ? home_directory : "");
    if (path.empty()) return 0;
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return 0;
    struct stat attributes {};
    lease_t lease {};
    const bool safe = fstat(descriptor, &attributes) == 0 && S_ISREG(attributes.st_mode) &&
                      attributes.st_uid == getuid() && attributes.st_nlink == 1 &&
                      (attributes.st_mode & 0022) == 0 && attributes.st_size == sizeof(lease) &&
                      pread(descriptor, &lease, sizeof(lease), 0) == sizeof(lease);
    close(descriptor);
    return safe && valid(lease, now) ? lease.limit_millihz : 0;
  }

  std::uint32_t current_limit() {
    static const bool excluded = excluded_process();
    if (excluded) return 0;
    static std::mutex lease_mutex;
    static std::int64_t checked_at = 0;
    static std::uint32_t limit = 0;
    std::lock_guard lock(lease_mutex);
    const auto now = monotonic_ns();
    if (now - checked_at >= 100000000) {
      limit = read_lease(now);
      checked_at = now;
    }
    return limit;
  }

  template<class T>
  T *chain_info(const void *next, VkStructureType type) {
    auto *entry = const_cast<VkBaseInStructure *>(static_cast<const VkBaseInStructure *>(next));
    while (entry) {
      if (entry->sType == type && reinterpret_cast<T *>(entry)->function == VK_LAYER_LINK_INFO)
        return reinterpret_cast<T *>(entry);
      entry = const_cast<VkBaseInStructure *>(entry->pNext);
    }
    return nullptr;
  }
}

extern "C" {
  __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
  vkGetInstanceProcAddr(VkInstance instance, const char *name);
  __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
  vkGetDeviceProcAddr(VkDevice device, const char *name);

  static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
      const VkAllocationCallbacks *allocator, VkInstance *instance) {
    auto *link = chain_info<VkLayerInstanceCreateInfo>(info->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gpdpa = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto next = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const auto result = next(info, allocator, instance);
    if (result == VK_SUCCESS) {
      std::lock_guard lock(dispatch_mutex);
      instances[key(*instance)] = {*instance, gipa, gpdpa};
    }
    return result;
  }

  static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    if (!instance) return;
    PFN_vkGetInstanceProcAddr gipa;
    {
      std::lock_guard lock(dispatch_mutex);
      const auto found = instances.find(key(instance));
      if (found == instances.end()) return;
      gipa = found->second.gipa;
      instances.erase(found);
    }
    const auto next = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    next(instance, allocator);
  }

  static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
      const VkAllocationCallbacks *allocator, VkDevice *device) {
    auto *link = chain_info<VkLayerDeviceCreateInfo>(info->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    VkInstance instance;
    { std::lock_guard lock(dispatch_mutex); instance = instances.at(key(physical)).instance; }
    const auto next = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const auto result = next(physical, info, allocator, device);
    if (result == VK_SUCCESS) {
      auto data = std::make_shared<device_t>();
      data->gdpa = gdpa;
      data->present = reinterpret_cast<PFN_vkQueuePresentKHR>(gdpa(*device, "vkQueuePresentKHR"));
      data->create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(gdpa(*device, "vkCreateSwapchainKHR"));
      data->destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(gdpa(*device, "vkDestroySwapchainKHR"));
      data->destroy = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*device, "vkDestroyDevice"));
      std::lock_guard lock(dispatch_mutex);
      devices[key(*device)] = std::move(data);
    }
    return result;
  }

  static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks *allocator) {
    if (!device) return;
    std::shared_ptr<device_t> data;
    {
      std::lock_guard lock(dispatch_mutex);
      const auto found = devices.find(key(device));
      if (found == devices.end()) return;
      data = found->second;
      devices.erase(found);
    }
    data->destroy(device, allocator);
  }

  static VKAPI_ATTR VkResult VKAPI_CALL create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
      const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
    std::shared_ptr<device_t> data;
    { std::lock_guard lock(dispatch_mutex); data = devices.at(key(device)); }
    const auto result = data->create_swapchain(device, info, allocator, swapchain);
    if (result == VK_SUCCESS) {
      std::lock_guard lock(dispatch_mutex);
      data->swapchains[*swapchain] = std::make_shared<swapchain_t>();
    }
    return result;
  }

  static VKAPI_ATTR void VKAPI_CALL destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
      const VkAllocationCallbacks *allocator) {
    std::shared_ptr<device_t> data;
    {
      std::lock_guard lock(dispatch_mutex);
      data = devices.at(key(device));
      data->swapchains.erase(swapchain);
    }
    data->destroy_swapchain(device, swapchain, allocator);
  }

  static VKAPI_ATTR VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR *info) {
    std::shared_ptr<device_t> data;
    std::shared_ptr<swapchain_t> swapchain;
    {
      std::lock_guard lock(dispatch_mutex);
      data = devices.at(key(queue));
      if (info && info->swapchainCount) {
        const auto found = data->swapchains.find(info->pSwapchains[0]);
        if (found != data->swapchains.end()) swapchain = found->second;
      }
    }
    if (swapchain) {
      const auto limit = current_limit();
      std::int64_t deadline;
      {
        std::lock_guard lock(swapchain->mutex);
        deadline = swapchain->pacer.deadline(monotonic_ns(), limit);
      }
      // Do not hold dispatch or pacing locks across a wait or driver callback.
      auto delay = deadline - monotonic_ns();
      while (delay > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<std::int64_t>(delay, 100000000)));
        const auto updated_limit = current_limit();
        if (updated_limit != limit) {
          std::lock_guard lock(swapchain->mutex);
          (void) swapchain->pacer.deadline(monotonic_ns(), updated_limit);
          break;
        }
        delay = deadline - monotonic_ns();
      }
    }
    return data->present(queue, info);
  }

  static PFN_vkVoidFunction intercept(const char *name) {
    if (!name) return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_instance);
    if (!std::strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(create_device);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
    if (!std::strcmp(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(create_swapchain);
    if (!std::strcmp(name, "vkDestroySwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_swapchain);
    if (!std::strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(queue_present);
    return nullptr;
  }

  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name) return nullptr;
    if (!std::strcmp(name, "vkCreateInstance") || !std::strcmp(name, "vkGetInstanceProcAddr")) return intercept(name);
    if (!instance) return nullptr;
    PFN_vkGetInstanceProcAddr next;
    { std::lock_guard lock(dispatch_mutex); next = instances.at(key(instance)).gipa; }
    const auto downstream = next(instance, name);
    if (downstream) if (const auto hook = intercept(name)) return hook;
    return downstream;
  }

  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    if (!device) return nullptr;
    PFN_vkGetDeviceProcAddr next;
    { std::lock_guard lock(dispatch_mutex); next = devices.at(key(device))->gdpa; }
    const auto downstream = next(device, name);
    // Never advertise a disabled device extension just because it is hooked.
    if (downstream) if (const auto hook = intercept(name)) return hook;
    return downstream;
  }

  static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL physical_proc(VkInstance instance, const char *name) {
    if (!instance) return nullptr;
    PFN_GetPhysicalDeviceProcAddr next;
    { std::lock_guard lock(dispatch_mutex); next = instances.at(key(instance)).gpdpa; }
    return next ? next(instance, name) : nullptr;
  }

  __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
  vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *interface) {
    if (!interface || interface->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || interface->loaderLayerInterfaceVersion < 2)
      return VK_ERROR_INITIALIZATION_FAILED;
    interface->loaderLayerInterfaceVersion = 2;
    interface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    interface->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    interface->pfnGetPhysicalDeviceProcAddr = physical_proc;
    return VK_SUCCESS;
  }
}
