#include "device_profile.h"

#include <3ds/services/apt.h>

namespace {

struct DeviceProfileState {
  bool initialized = false;
  bool cached_is_old3ds = true;
  LightLock lock;

  DeviceProfileState() : initialized(false), cached_is_old3ds(true), lock() {
    LightLock_Init(&lock);
  }
};

DeviceProfileState g_device_profile_state;

} // namespace

bool is_old3ds_baseline_device() {
  LightLock_Lock(&g_device_profile_state.lock);
  if (!g_device_profile_state.initialized) {
    bool is_new3ds = false;
    if (R_SUCCEEDED(APT_CheckNew3DS(&is_new3ds))) {
      g_device_profile_state.cached_is_old3ds = !is_new3ds;
    }
    g_device_profile_state.initialized = true;
  }
  const bool is_old3ds = g_device_profile_state.cached_is_old3ds;
  LightLock_Unlock(&g_device_profile_state.lock);
  return is_old3ds;
}
