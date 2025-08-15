<template>
  <div class="container">
    <h1 class="my-4">{{ $t("config.configuration") }}</h1>
    <div v-if="config">
      <ul class="nav nav-tabs">
        <li class="nav-item" v-for="tab in tabs" :key="tab.id">
          <a
            class="nav-link"
            :class="{ active: tab.id === currentTab }"
            href="#"
            @click.prevent="currentTab = tab.id"
            >{{ tab.name }}</a
          >
        </li>
      </ul>
      <!-- Core Tabs -->
      <General
        v-if="currentTab === 'general'"
        :config="config"
        :platform="platform"
      />
      <Inputs
        v-if="currentTab === 'input'"
        :config="config"
        :platform="platform"
      />
      <AudioVideo
        v-if="currentTab === 'av'"
        :config="config"
        :platform="platform"
      />
      <Network
        v-if="currentTab === 'network'"
        :config="config"
        :platform="platform"
      />
      <Files
        v-if="currentTab === 'files'"
        :config="config"
        :platform="platform"
      />
      <Advanced
        v-if="currentTab === 'advanced'"
        :config="config"
        :platform="platform"
      />

      <!-- Encoder Tabs (delegated to ContainerEncoders) -->
      <ContainerEncoders
        v-if="['nv', 'qsv', 'amd', 'vt', 'vaapi', 'sw'].includes(currentTab)"
        :currentTab="currentTab"
        :config="config"
        :platform="platform"
      />
    </div>
    <div class="alert alert-success my-4" v-if="saved && !restarted">
      <b>{{ $t("_common.success") }}</b> {{ $t("config.apply_note") }}
    </div>
    <div class="alert alert-success my-4" v-if="restarted">
      <b>{{ $t("_common.success") }}</b> {{ $t("config.restart_note") }}
    </div>
    <div class="mb-3 buttons">
      <button class="btn btn-primary mr-3" @click="save">
        {{ $t("_common.save") }}
      </button>
      <button class="btn btn-success" @click="apply" v-if="saved && !restarted">
        {{ $t("_common.apply") }}
      </button>
    </div>
  </div>
</template>
<script>
import { computed } from "vue";
import General from "@/configs/tabs/General.vue";
import Inputs from "@/configs/tabs/Inputs.vue";
import Network from "@/configs/tabs/Network.vue";
import Files from "@/configs/tabs/Files.vue";
import Advanced from "@/configs/tabs/Advanced.vue";
import AudioVideo from "@/configs/tabs/AudioVideo.vue";
import ContainerEncoders from "@/configs/tabs/ContainerEncoders.vue";

export default {
  components: {
    General,
    Inputs,
    Network,
    Files,
    Advanced,
    AudioVideo,
    ContainerEncoders,
  },
  // Provide reactive platform for platform-i18n helper ($tp) used in tab components
  provide() {
    return {
      platform: computed(() => this.platform),
    };
  },
  data() {
    return {
      platform: "",
      saved: false,
      restarted: false,
      config: null,
      currentTab: "general",
      tabs: [
        {
          id: "general",
          name: "General",
          options: {
            locale: "en",
            sunshine_name: "",
            min_log_level: 2,
            global_prep_cmd: [],
            notify_pre_releases: "disabled",
          },
        },
        {
          id: "input",
          name: "Input",
          options: {
            controller: "enabled",
            gamepad: "auto",
            ds4_back_as_touchpad_click: "enabled",
            motion_as_ds4: "enabled",
            touchpad_as_ds4: "enabled",
            back_button_timeout: -1,
            keyboard: "enabled",
            key_repeat_delay: 500,
            key_repeat_frequency: 24.9,
            always_send_scancodes: "enabled",
            key_rightalt_to_key_win: "disabled",
            mouse: "enabled",
            high_resolution_scrolling: "enabled",
            native_pen_touch: "enabled",
            keybindings: "[0x10,0xA0,0x11,0xA2,0x12,0xA4]",
          },
        },
        {
          id: "av",
          name: "Audio/Video",
          options: {
            audio_sink: "",
            virtual_sink: "",
            install_steam_audio_drivers: "enabled",
            adapter_name: "",
            output_name: "",
            dd_configuration_option: "disabled",
            dd_resolution_option: "auto",
            dd_manual_resolution: "",
            dd_refresh_rate_option: "auto",
            dd_manual_refresh_rate: "",
            dd_hdr_option: "auto",
            dd_config_revert_delay: 3000,
            dd_config_revert_on_disconnect: "disabled",
            dd_mode_remapping: {
              mixed: [],
              resolution_only: [],
              refresh_rate_only: [],
            },
            dd_wa_hdr_toggle_delay: 0,
            max_bitrate: 0,
            minimum_fps_target: 0,
          },
        },
        {
          id: "network",
          name: "Network",
          options: {
            upnp: "disabled",
            address_family: "ipv4",
            port: 47989,
            origin_web_ui_allowed: "lan",
            external_ip: "",
            lan_encryption_mode: 0,
            wan_encryption_mode: 1,
            ping_timeout: 10000,
          },
        },
        {
          id: "files",
          name: "Config Files",
          options: {
            file_apps: "",
            credentials_file: "",
            log_path: "",
            pkey: "",
            cert: "",
            file_state: "",
          },
        },
        {
          id: "advanced",
          name: "Advanced",
          options: {
            fec_percentage: 20,
            qp: 28,
            min_threads: 2,
            hevc_mode: 0,
            av1_mode: 0,
            capture: "",
            encoder: "",
          },
        },
        {
          id: "nv",
          name: "NVIDIA NVENC Encoder",
          options: {
            nvenc_preset: 1,
            nvenc_twopass: "quarter_res",
            nvenc_spatial_aq: "disabled",
            nvenc_vbv_increase: 0,
            nvenc_realtime_hags: "enabled",
            nvenc_latency_over_power: "enabled",
            nvenc_opengl_vulkan_on_dxgi: "enabled",
            nvenc_h264_cavlc: "disabled",
          },
        },
        {
          id: "qsv",
          name: "Intel QuickSync Encoder",
          options: {
            qsv_preset: "medium",
            qsv_coder: "auto",
            qsv_slow_hevc: "disabled",
          },
        },
        {
          id: "amd",
          name: "AMD AMF Encoder",
          options: {
            amd_usage: "ultralowlatency",
            amd_rc: "vbr_latency",
            amd_enforce_hrd: "disabled",
            amd_quality: "balanced",
            amd_preanalysis: "disabled",
            amd_vbaq: "enabled",
            amd_coder: "auto",
          },
        },
        {
          id: "vt",
          name: "VideoToolbox Encoder",
          options: {
            vt_coder: "auto",
            vt_software: "auto",
            vt_realtime: "enabled",
          },
        },
        {
          id: "vaapi",
          name: "VA-API Encoder",
          options: { vaapi_strict_rc_buffer: "disabled" },
        },
        {
          id: "sw",
          name: "Software Encoder",
          options: { sw_preset: "superfast", sw_tune: "zerolatency" },
        },
      ],
    };
  },
  mounted() {
    this.handleHash();
    window.addEventListener("hashchange", this.handleHash);
  },
  beforeUnmount() {
    window.removeEventListener("hashchange", this.handleHash);
  },
  created() {
    fetch("./api/config")
      .then((r) => r.json())
      .then((r) => {
        this.config = r;
        this.platform = this.config.platform;
        delete this.config.platform;
        delete this.config.status;
        delete this.config.version;
        ["dd_mode_remapping", "global_prep_cmd"].forEach((k) => {
          if (this.config.hasOwnProperty(k)) {
            this.config[k] = JSON.parse(this.config[k]);
          }
        });
        this.tabs.forEach((tab) => {
          Object.keys(tab.options).forEach((opt) => {
            if (this.config[opt] === undefined) {
              this.config[opt] = JSON.parse(JSON.stringify(tab.options[opt]));
            }
          });
        });
        if (this.platform === "windows") {
          this.tabs = this.tabs.filter(
            (el) => el.id !== "vt" && el.id !== "vaapi"
          );
        }
        if (this.platform === "linux") {
          this.tabs = this.tabs.filter(
            (el) => el.id !== "amd" && el.id !== "qsv" && el.id !== "vt"
          );
        }
        if (this.platform === "macos") {
          this.tabs = this.tabs.filter(
            (el) =>
              el.id !== "amd" &&
              el.id !== "nv" &&
              el.id !== "qsv" &&
              el.id !== "vaapi"
          );
        }
      });
  },
  methods: {
    handleHash() {
      let hash = window.location.hash.substring(1);
      if (!hash) return;
      this.tabs.forEach((tab) => {
        Object.keys(tab.options).forEach((key) => {
          if (tab.id === hash || key === hash) {
            this.currentTab = tab.id;
          }
        });
      });
    },
    forceUpdate() {
      this.$forceUpdate();
    },
    serialize() {
      return JSON.parse(JSON.stringify(this.config));
    },
    save() {
      this.saved = false;
      this.restarted = false;
      let config = this.serialize();
      this.tabs.forEach((tab) => {
        Object.keys(tab.options).forEach((optionKey) => {
          if (
            JSON.stringify(config[optionKey]) ===
            JSON.stringify(tab.options[optionKey])
          ) {
            delete config[optionKey];
          }
        });
      });
      return fetch("./api/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(config),
      }).then((r) => {
        if (r.status === 200) {
          this.saved = true;
          return this.saved;
        }
        return false;
      });
    },
    apply() {
      this.saved = this.restarted = false;
      this.save().then((result) => {
        if (result === true) {
          this.restarted = true;
          setTimeout(() => {
            this.saved = this.restarted = false;
          }, 5000);
          fetch("./api/restart", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
          });
        }
      });
    },
  },
};
</script>
