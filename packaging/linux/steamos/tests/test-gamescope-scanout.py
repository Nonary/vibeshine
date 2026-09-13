#!/usr/bin/env python3
"""Execute the patched Gamescope device-opening code with a fake DRM/session API.

This verifies device selection and rejection, not GPU import or presentation.
The same harness runs on macOS and Linux; no compositor or devices are opened.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

source = (pathlib.Path(sys.argv[1]) / 'src/Backends/DRMBackend.cpp').read_text()
start = source.index('static bool open_drm_scanout_device(')
end = source.index('\nbool init_drm(', start)
selection = source[start:end]

harness = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
constexpr int DRM_NODE_PRIMARY = 0;
constexpr int DRM_NODE_RENDER = 2;
struct drmDevice { int available_nodes; char *nodes[3]; };
struct drm_t {
    char *device_name = nullptr;
    int fd = -1;
    bool bSeparateScanoutDevice = false;
    ~drm_t() { free(device_name); }
};
struct Logger {
    template<class... T> void errorf(const char *, T...) {}
    template<class... T> void infof(const char *, T...) {}
} drm_log;
const char *g_sDRMDevice = nullptr;
bool have_renderer = true, kms = true, stat_ok = true, primary_available = true;
int lookup_error = 0, open_result = 5, node_type = DRM_NODE_PRIMARY;
int opens = 0, closes = 0, releases = 0;
dev_t scanout_id = 100;
std::string requested;
bool vulkan_primary_dev_id(dev_t *id) { *id = 100; return have_renderer; }
int drmGetDeviceFromDevId(dev_t id, int, drmDevice **device) {
    assert(id == 100);
    if (lookup_error) return lookup_error;
    static char path[] = "/dev/dri/card0";
    static drmDevice value;
    value = {primary_available ? 1 : 0, {path, nullptr, nullptr}};
    *device = &value;
    return 0;
}
void drmFreeDevice(drmDevice **device) { ++releases; *device = nullptr; }
int wlsession_open_kms(const char *path) {
    ++opens; requested = path ? path : "auto"; return open_result;
}
void wlsession_close_kms() { ++closes; }
int drmGetNodeTypeFromFd(int) { return node_type; }
bool drmIsKMS(int) { return kms; }
int fake_fstat(int, struct stat *attributes) {
    attributes->st_rdev = scanout_id; return stat_ok ? 0 : -1;
}
#define fstat fake_fstat
''' + selection + r'''
void reset() {
    g_sDRMDevice = nullptr;
    have_renderer = kms = stat_ok = primary_available = true;
    lookup_error = 0; open_result = 5; node_type = DRM_NODE_PRIMARY;
    opens = closes = releases = 0; scanout_id = 100; requested.clear();
}
int main() {
    { reset(); drm_t device;
      assert(open_drm_scanout_device(&device));
      assert(requested == "/dev/dri/card0" && releases == 1);
      assert(!device.bSeparateScanoutDevice && opens == 1 && closes == 0); }
    { reset(); drm_t device; g_sDRMDevice = "/dev/dri/card2"; scanout_id = 200;
      lookup_error = -1; // Explicit selection does not depend on primary-node lookup.
      assert(open_drm_scanout_device(&device));
      assert(requested == g_sDRMDevice && device.bSeparateScanoutDevice);
      assert(opens == 1 && releases == 0); }
    { reset(); drm_t device; g_sDRMDevice = "/dev/dri/card0";
      assert(open_drm_scanout_device(&device)); assert(!device.bSeparateScanoutDevice); }
    { reset(); drm_t device; have_renderer = false;
      assert(open_drm_scanout_device(&device));
      assert(requested == "auto" && device.bSeparateScanoutDevice); }
    { reset(); drm_t device; have_renderer = false; g_sDRMDevice = "/dev/dri/card2";
      assert(open_drm_scanout_device(&device));
      assert(requested == g_sDRMDevice && device.bSeparateScanoutDevice); }
    { reset(); drm_t device; g_sDRMDevice = "card2";
      assert(!open_drm_scanout_device(&device)); assert(opens == 0); }
    { reset(); drm_t device; g_sDRMDevice = "";
      assert(!open_drm_scanout_device(&device)); assert(opens == 0); }
    { reset(); drm_t device; lookup_error = -1;
      assert(!open_drm_scanout_device(&device)); assert(opens == 0); }
    { reset(); drm_t device; primary_available = false;
      assert(!open_drm_scanout_device(&device)); assert(opens == 0 && releases == 1); }
    { reset(); drm_t device; g_sDRMDevice = "/dev/dri/card2"; open_result = -1;
      assert(!open_drm_scanout_device(&device));
      assert(opens == 1 && closes == 0 && !device.device_name); }
    for (int failure = 0; failure < 3; ++failure) {
      reset(); drm_t device; g_sDRMDevice = "/dev/dri/card2";
      if (failure == 0) node_type = DRM_NODE_RENDER;
      if (failure == 1) kms = false;
      if (failure == 2) stat_ok = false;
      assert(!open_drm_scanout_device(&device));
      assert(opens == 1 && closes == 1 && device.fd == -1 && !device.device_name);
    }
}
'''

with tempfile.TemporaryDirectory(prefix='vibeshine-scanout-') as directory:
    root = pathlib.Path(directory)
    (root / 'test.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Gamescope scanout selection regression checks passed (no GPU validation)')
