import { defineStore } from 'pinia'
import { ref } from 'vue'
import { http } from '@/http.js'

// Centralized store for configuration defaults and runtime config
export const useConfigStore = defineStore('config', () => {
  // initial defaults moved from config.html tabs
  const tabs = ref([
    {
      id: "general",
      name: "General",
      options: {
        "locale": "en",
        "sunshine_name": "",
        "min_log_level": 2,
        "global_prep_cmd": [],
        "notify_pre_releases": "disabled",
        "session_token_ttl_seconds": 86400,
      },
    },
    {
      id: "input",
      name: "Input",
      options: {
        "controller": "enabled",
        "gamepad": "auto",
        "ds4_back_as_touchpad_click": "enabled",
        "motion_as_ds4": "enabled",
        "touchpad_as_ds4": "enabled",
        "back_button_timeout": -1,
        "keyboard": "enabled",
        "key_repeat_delay": 500,
        "key_repeat_frequency": 24.9,
        "always_send_scancodes": "enabled",
        "key_rightalt_to_key_win": "disabled",
        "mouse": "enabled",
        "high_resolution_scrolling": "enabled",
        "native_pen_touch": "enabled",
        "keybindings": "[0x10,0xA0,0x11,0xA2,0x12,0xA4]",
      },
    },
    {
      id: "av",
      name: "Audio/Video",
      options: {
        "audio_sink": "",
        "virtual_sink": "",
        "install_steam_audio_drivers": "enabled",
        "adapter_name": "",
        "output_name": "",
        "dd_configuration_option": "disabled",
        "dd_resolution_option": "auto",
        "dd_manual_resolution": "",
        "dd_refresh_rate_option": "auto",
        "dd_manual_refresh_rate": "",
        "dd_hdr_option": "auto",
        "dd_config_revert_delay": 3000,
        "dd_config_revert_on_disconnect": "disabled",
        "dd_mode_remapping": { "mixed": [], "resolution_only": [], "refresh_rate_only": [] },
        "dd_wa_hdr_toggle_delay": 0,
        "max_bitrate": 0,
        "minimum_fps_target": 0
      },
    },
    {
      id: "network",
      name: "Network",
      options: {
        "upnp": "disabled",
        "address_family": "ipv4",
        "port": 47989,
        "origin_web_ui_allowed": "lan",
        "external_ip": "",
        "lan_encryption_mode": 0,
        "wan_encryption_mode": 1,
        "ping_timeout": 10000,
      },
    },
    {
      id: "files",
      name: "Config Files",
      options: {
        "file_apps": "",
        "credentials_file": "",
        "log_path": "",
        "pkey": "",
        "cert": "",
        "file_state": "",
      },
    },
    {
      id: "advanced",
      name: "Advanced",
      options: {
        "fec_percentage": 20,
        "qp": 28,
        "min_threads": 2,
        "hevc_mode": 0,
        "av1_mode": 0,
        "capture": "",
        "encoder": "",
      },
    },
    {
      id: "nv",
      name: "NVIDIA NVENC Encoder",
      options: {
        "nvenc_preset": 1,
        "nvenc_twopass": "quarter_res",
        "nvenc_spatial_aq": "disabled",
        "nvenc_vbv_increase": 0,
        "nvenc_realtime_hags": "enabled",
        "nvenc_latency_over_power": "enabled",
        "nvenc_opengl_vulkan_on_dxgi": "enabled",
        "nvenc_h264_cavlc": "disabled",
      },
    },
    {
      id: "qsv",
      name: "Intel QuickSync Encoder",
      options: {
        "qsv_preset": "medium",
        "qsv_coder": "auto",
        "qsv_slow_hevc": "disabled",
      },
    },
    {
      id: "amd",
      name: "AMD AMF Encoder",
      options: {
        "amd_usage": "ultralowlatency",
        "amd_rc": "vbr_latency",
        "amd_enforce_hrd": "disabled",
        "amd_quality": "balanced",
        "amd_preanalysis": "disabled",
        "amd_vbaq": "enabled",
        "amd_coder": "auto",
      },
    },
    {
      id: "vt",
      name: "VideoToolbox Encoder",
      options: {
        "vt_coder": "auto",
        "vt_software": "auto",
        "vt_realtime": "enabled",
      },
    },
    {
      id: "vaapi",
      name: "VA-API Encoder",
      options: {
        "vaapi_strict_rc_buffer": "disabled",
      },
    },
    {
      id: "sw",
      name: "Software Encoder",
      options: {
        "sw_preset": "superfast",
        "sw_tune": "zerolatency",
      },
    },
  ])

  // runtime config loaded from server
  const config = ref(null)

  function setConfig(obj) {
    // deep clone from server
    config.value = JSON.parse(JSON.stringify(obj))

    // special options that are JSON-encoded on server
    const specialOptions = ["dd_mode_remapping", "global_prep_cmd"]
    for (const optionKey of specialOptions) {
      if (config.value && config.value.hasOwnProperty(optionKey) && typeof config.value[optionKey] === 'string') {
        try {
          config.value[optionKey] = JSON.parse(config.value[optionKey])
        } catch (e) {
          // leave as-is
        }
      }
    }

    // Keep server-only keys like platform/status/version in the store for UI use.
    // They will be removed when serializing for POST.

    // populate defaults
    tabs.value.forEach(tab => {
      Object.keys(tab.options).forEach(optionKey => {
        if (config.value[optionKey] === undefined) {
          config.value[optionKey] = JSON.parse(JSON.stringify(tab.options[optionKey]))
        }
      })
    })
  }

  function updateOption(key, value) {
    if (!config.value) config.value = {}
    config.value[key] = value
  }

  function serialize() {
    // Return a deep clone without server-only keys
    const out = JSON.parse(JSON.stringify(config.value))
    if (!out) return out
    delete out.platform
    delete out.status
    delete out.version
    return out
  }

  // Fetch runtime config from the server and apply via setConfig
  async function fetchConfig(force = false) {
    if (config.value && !force) return config.value
    try {
      const r = await http.get('/api/config')
      if (r.status !== 200) return null
      setConfig(r.data)
      return config.value
    } catch (e) {
      console.error('fetchConfig failed', e)
      return null
    }
  }

  return {
    tabs,
    config,
    setConfig,
    updateOption,
    serialize,
    fetchConfig,
  }
})
