// Exercise the real shared library through the Vulkan loader ABI, with a
// deterministic next layer standing in for the driver. No GPU is required.
#include <vulkan/vk_layer.h>
#include "src/platform/linux/global_fps_policy.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>

namespace {
  struct dispatchable { void *dispatch; };
  int instance_key, device_key;
  dispatchable instance_storage {&instance_key}, physical_storage {&instance_key};
  dispatchable device_storage {&device_key}, queue_storage {&device_key};
  const auto instance = reinterpret_cast<VkInstance>(&instance_storage);
  const auto physical = reinterpret_cast<VkPhysicalDevice>(&physical_storage);
  const auto device = reinterpret_cast<VkDevice>(&device_storage);
  const auto queue = reinterpret_cast<VkQueue>(&queue_storage);
  const auto swapchain = reinterpret_cast<VkSwapchainKHR>(uintptr_t {42});
  PFN_vkGetInstanceProcAddr layer_gipa;
  int instances_created, instances_destroyed, devices_created, devices_destroyed;
  int swapchains_created, swapchains_destroyed, presents;
  bool extension_enabled = true;
  void VKAPI_CALL marker() {}
  void VKAPI_CALL physical_marker() {}
  VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *, VkInstance *out) {
    assert(static_cast<const VkLayerInstanceCreateInfo *>(info->pNext)->u.pLayerInfo == nullptr);
    *out = instance;
    ++instances_created;
    return VK_SUCCESS;
  }
  void VKAPI_CALL destroy_instance(VkInstance value, const VkAllocationCallbacks *) {
    assert(value == instance);
    ++instances_destroyed;
  }
  VkResult VKAPI_CALL create_device(VkPhysicalDevice value, const VkDeviceCreateInfo *info,
      const VkAllocationCallbacks *, VkDevice *out) {
    assert(value == physical);
    assert(static_cast<const VkLayerDeviceCreateInfo *>(info->pNext)->u.pLayerInfo == nullptr);
    *out = device;
    ++devices_created;
    return VK_SUCCESS;
  }
  void VKAPI_CALL destroy_device(VkDevice value, const VkAllocationCallbacks *) {
    assert(value == device);
    ++devices_destroyed;
  }
  VkResult VKAPI_CALL create_swapchain(VkDevice value, const VkSwapchainCreateInfoKHR *,
      const VkAllocationCallbacks *, VkSwapchainKHR *out) {
    assert(value == device);
    *out = swapchain;
    ++swapchains_created;
    return VK_SUCCESS;
  }
  void VKAPI_CALL destroy_swapchain(VkDevice value, VkSwapchainKHR sc, const VkAllocationCallbacks *) {
    assert(value == device && sc == swapchain);
    ++swapchains_destroyed;
  }
  VkResult VKAPI_CALL present(VkQueue value, const VkPresentInfoKHR *info) {
    assert(value == queue && info->swapchainCount == 1 && info->pSwapchains[0] == swapchain);
    // Re-enter the layer while the downstream callback runs: this would
    // deadlock if queue-present retained the global dispatch lock.
    assert(layer_gipa(instance, "driverFunction") == reinterpret_cast<PFN_vkVoidFunction>(marker));
    if (info->pResults) info->pResults[0] = VK_ERROR_OUT_OF_DATE_KHR;
    ++presents;
    return VK_SUBOPTIMAL_KHR;
  }
  PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance value, const char *name) {
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
    if (!std::strcmp(name, "vkCreateDevice")) {
      assert(value == instance);
      return reinterpret_cast<PFN_vkVoidFunction>(create_device);
    }
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_instance);
    if (!std::strcmp(name, "driverFunction")) return reinterpret_cast<PFN_vkVoidFunction>(marker);
    return nullptr;
  }
  PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice value, const char *name) {
    assert(value == device);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
    if (!std::strcmp(name, "driverFunction")) return reinterpret_cast<PFN_vkVoidFunction>(marker);
    if (!extension_enabled) return nullptr;
    if (!std::strcmp(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(create_swapchain);
    if (!std::strcmp(name, "vkDestroySwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_swapchain);
    if (!std::strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(present);
    return nullptr;
  }
  PFN_vkVoidFunction VKAPI_CALL gpdpa(VkInstance value, const char *) {
    assert(value == instance);
    return reinterpret_cast<PFN_vkVoidFunction>(physical_marker);
  }
}

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::cerr << dlerror() << '\n'; return 1; }
  const auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
    dlsym(library, "vkNegotiateLoaderLayerInterfaceVersion"));
  assert(negotiate);
  VkNegotiateLayerInterface api {};
  api.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
  api.loaderLayerInterfaceVersion = 2;
  assert(negotiate(&api) == VK_SUCCESS);
  layer_gipa = api.pfnGetInstanceProcAddr;

  VkLayerInstanceLink ilink {nullptr, gipa, gpdpa};
  VkLayerInstanceCreateInfo ichain {};
  ichain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
  ichain.function = VK_LAYER_LINK_INFO;
  ichain.u.pLayerInfo = &ilink;
  VkInstanceCreateInfo ici {};
  ici.pNext = &ichain;
  VkInstance created_instance;
  assert(reinterpret_cast<PFN_vkCreateInstance>(layer_gipa(VK_NULL_HANDLE, "vkCreateInstance"))(
    &ici, nullptr, &created_instance) == VK_SUCCESS);
  assert(api.pfnGetPhysicalDeviceProcAddr(instance, "physicalExtension") == reinterpret_cast<PFN_vkVoidFunction>(physical_marker));

  VkLayerDeviceLink dlink {nullptr, gipa, gdpa};
  VkLayerDeviceCreateInfo dchain {};
  dchain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
  dchain.function = VK_LAYER_LINK_INFO;
  dchain.u.pLayerInfo = &dlink;
  VkDeviceCreateInfo dci {};
  dci.pNext = &dchain;
  VkDevice created_device;
  assert(reinterpret_cast<PFN_vkCreateDevice>(layer_gipa(instance, "vkCreateDevice"))(
    physical, &dci, nullptr, &created_device) == VK_SUCCESS);
  const auto layer_gdpa = api.pfnGetDeviceProcAddr;
  assert(layer_gdpa(device, "driverFunction") == reinterpret_cast<PFN_vkVoidFunction>(marker));
  extension_enabled = false;
  assert(layer_gdpa(device, "vkQueuePresentKHR") == nullptr);
  extension_enabled = true;
  VkSwapchainCreateInfoKHR sci {};
  VkSwapchainKHR created_swapchain;
  assert(reinterpret_cast<PFN_vkCreateSwapchainKHR>(layer_gdpa(device, "vkCreateSwapchainKHR"))(
    device, &sci, nullptr, &created_swapchain) == VK_SUCCESS);
  VkResult swapchain_result = VK_SUCCESS;
  VkPresentInfoKHR pi {};
  pi.swapchainCount = 1;
  pi.pSwapchains = &created_swapchain;
  pi.pResults = &swapchain_result;
  assert(reinterpret_cast<PFN_vkQueuePresentKHR>(layer_gdpa(device, "vkQueuePresentKHR"))(
    queue, &pi) == VK_SUBOPTIMAL_KHR);
  assert(swapchain_result == VK_ERROR_OUT_OF_DATE_KHR);
#ifdef __linux__
  if (std::getenv("VIBESHINE_TEST_WAIT_FOR_DISABLE")) {
    std::cout << "waiting" << std::endl;
    const auto started = platf::global_fps::monotonic_ns();
    reinterpret_cast<PFN_vkQueuePresentKHR>(layer_gdpa(device, "vkQueuePresentKHR"))(queue, &pi);
    assert(platf::global_fps::monotonic_ns() - started < 5000000000);
    --presents;
  }
  if (std::getenv("VIBESHINE_TEST_EXPECT_FPS")) {
    const auto started = platf::global_fps::monotonic_ns();
    const auto present_fn = reinterpret_cast<PFN_vkQueuePresentKHR>(layer_gdpa(device, "vkQueuePresentKHR"));
    present_fn(queue, &pi);
    present_fn(queue, &pi);
    assert(platf::global_fps::monotonic_ns() - started >= 30000000);
    presents -= 2;
  }
#endif
  reinterpret_cast<PFN_vkDestroySwapchainKHR>(layer_gdpa(device, "vkDestroySwapchainKHR"))(device, swapchain, nullptr);
  reinterpret_cast<PFN_vkDestroyDevice>(layer_gdpa(device, "vkDestroyDevice"))(device, nullptr);
  reinterpret_cast<PFN_vkDestroyInstance>(layer_gipa(instance, "vkDestroyInstance"))(instance, nullptr);
  assert(instances_created == 1 && instances_destroyed == 1 && devices_created == 1 && devices_destroyed == 1);
  assert(swapchains_created == 1 && swapchains_destroyed == 1 && presents == 1);
  dlclose(library);

  using namespace platf::global_fps;
  constexpr std::int64_t start = 10000000000;
  pacer_t pacer;
  assert(pacer.deadline(start, 60000) == start);
  assert(pacer.deadline(start + 1000, 60000) == start + 16666666);
  assert(pacer.deadline(start + 2000, 0) == start + 2000);
  assert(pacer.deadline(start + 3000, 59940) == start + 3000);
  assert(pacer.deadline(start + 4000, 59940) == start + 3000 + 1000000000000LL / 59940);
  assert(pacer.deadline(start + 1000000000, 59940) == start + 1000000000);
  assert(valid({1, 60000, start + lease_duration_ns}, start));
  assert(!valid({1, 60000, start}, start));
  assert(!valid({1, 60000, start + lease_duration_ns + 1}, start));
  assert(!valid({2, 60000, start + 1}, start));
  assert(!valid({1, 0, start + 1}, start));
  assert(selected("global", true) && selected("auto", false) && !selected("auto", true));
  assert(!selected("none", false) && !selected("mangohud", false));
  std::cout << "Vulkan layer dispatch and global limiter policy passed\n";
}
