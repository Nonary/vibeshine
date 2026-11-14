import { afterEach, describe, expect, it, vi } from "vitest";
import { mount, flushPromises } from "@vue/test-utils";
import { createPinia, setActivePinia } from "pinia";
import { createI18n } from "vue-i18n";
import AudioVideo from "@/configs/tabs/AudioVideo.vue";
import messagesEn from "@/public/assets/locale/en.json";
import { useConfigStore } from "@/stores/config";
import { http } from "@/http";

function mountAudioVideo(pinia: ReturnType<typeof createPinia>) {
  return mount(AudioVideo, {
    global: {
      plugins: [
        pinia,
        createI18n({
          legacy: false,
          locale: "en",
          messages: { en: messagesEn },
        }),
      ],
      provide: { i18n: { t: (key: string) => key } },
      stubs: {
        ConfigFieldRenderer: true,
        AdapterNameSelector: true,
        DisplayOutputSelector: true,
        DisplayDeviceOptions: true,
        DisplayModesSettings: true,
        FrameLimiterStep: true,
        Checkbox: true,
      },
    },
  });
}

function mockHost(statusCode: unknown, active: unknown = "vibeshine") {
  vi.spyOn(http, "get").mockImplementation(
    async (url) =>
      ({
        status: 200,
        data:
          url === "/api/config"
            ? {
                status: true,
                virtual_display_mode: "per_client",
                vdisplayStatus: 0,
                vdisplay: 0,
              }
            : {
                platform: "windows",
                virtual_display_driver: {
                  configured: "vibeshine",
                  active,
                  status_code: statusCode,
                },
              },
      }) as never,
  );
}

function createStore() {
  const pinia = createPinia();
  setActivePinia(pinia);
  return { pinia, store: useConfigStore() };
}

afterEach(() => {
  vi.restoreAllMocks();
  setActivePinia(undefined);
});

describe("legacy driver readiness", () => {
  it.each([
    [1, "Unknown"],
    [-1, "Not initialized"],
    [-2, "Driver version mismatch"],
    [-3, "Watchdog failed"],
    [undefined, "Unknown"],
    [null, "Unknown"],
    [false, "Unknown"],
    ["", "Unknown"],
    ["0", "Unknown"],
    [NaN, "Unknown"],
    [7, "Unknown"],
  ])("does not show ready for host status %s", async (code, expected) => {
    mockHost(code);
    const { pinia, store } = createStore();
    await store.fetchConfig(true);
    const wrapper = mountAudioVideo(pinia);
    expect(
      wrapper.text(),
      `host status ${String(code)} must not render ready`,
    ).not.toContain("Vibeshine driver ready");
    expect(wrapper.text()).toContain(`Vibeshine driver: ${expected}`);
    expect(wrapper.find(".bg-success\\/10").exists()).toBe(false);
    wrapper.unmount();
  });

  it("renders an explicit ready snapshot and follows runtime driver identity", async () => {
    mockHost(0, "sudovda");
    const { pinia, store } = createStore();
    await store.fetchConfig(true);
    const wrapper = mountAudioVideo(pinia);
    expect(wrapper.text()).toContain("SudoVDA driver ready");
    expect(wrapper.text()).not.toContain("Vibeshine driver ready");
    expect(wrapper.find(".bg-success\\/10").exists()).toBe(true);
    expect(store.serialize()).not.toHaveProperty("virtual_display_driver");
    expect(store.hasPendingPatch()).toBe(false);
    wrapper.unmount();
  });

  it("does not retain a previous ready observation after a metadata refresh fails", async () => {
    mockHost(0);
    const { pinia, store } = createStore();
    await store.fetchConfig(true);
    const wrapper = mountAudioVideo(pinia);
    expect(wrapper.text()).toContain("Vibeshine driver ready");
    vi.mocked(http.get).mockImplementation(async (url) => {
      if (url === "/api/metadata") throw new Error("unavailable");
      return {
        status: 200,
        data: { virtual_display_mode: "per_client" },
      } as never;
    });
    await store.fetchConfig(true);
    expect(wrapper.text()).toContain("Vibeshine driver: Unknown");
    expect(wrapper.find(".bg-success\\/10").exists()).toBe(false);
    expect(store.hasPendingPatch()).toBe(false);
    wrapper.unmount();
  });
  it.each([false, true])(
    "keeps the latest failed refresh non-ready when an older success arrives %s",
    async (olderArrivesLast) => {
      let completeOlder: (value: unknown) => void = () => {};
      let failLatest: (reason: unknown) => void = () => {};
      const olderMetadata = new Promise((resolve) => {
        completeOlder = resolve;
      });
      const latestMetadata = new Promise((_resolve, reject) => {
        failLatest = reject;
      });
      let metadataRequests = 0;
      vi.spyOn(http, "get").mockImplementation(async (url) => {
        if (url === "/api/config")
          return {
            status: 200,
            data: { virtual_display_mode: "per_client" },
          } as never;
        metadataRequests += 1;
        return (
          metadataRequests === 1 ? olderMetadata : latestMetadata
        ) as never;
      });
      const { pinia, store } = createStore();
      store.metadata = { platform: "windows" };
      const older = store.fetchConfig(true);
      await flushPromises();
      const latest = store.fetchConfig(true);
      await flushPromises();
      expect(metadataRequests).toBe(2);
      const wrapper = mountAudioVideo(pinia);
      const ready = {
        status: 200,
        data: {
          platform: "windows",
          virtual_display_driver: {
            configured: "vibeshine",
            active: "vibeshine",
            status_code: 0,
          },
        },
      };
      if (olderArrivesLast) {
        failLatest(new Error("unavailable"));
        await latest;
        completeOlder(ready);
        await older;
      } else {
        completeOlder(ready);
        await older;
        failLatest(new Error("unavailable"));
        await latest;
      }
      expect(wrapper.text()).toContain("Vibeshine driver: Unknown");
      expect(wrapper.find(".bg-success\\/10").exists()).toBe(false);
      expect(store.hasPendingPatch()).toBe(false);
      wrapper.unmount();
    },
  );

  it.each(["sudovda", "vibeshine"])(
    "keeps failure guidance with observed driver %s after the selection changes",
    async (active) => {
      mockHost(-3, active);
      const { pinia, store } = createStore();
      await store.fetchConfig(true);
      // Model the selection having changed independently of the runtime snapshot.
      store.setConfig({
        virtual_display_mode: "per_client",
        dd_use_sunshine_virtual_display_driver: active === "sudovda",
      });
      const wrapper = mountAudioVideo(pinia);
      const driverName = active === "sudovda" ? "SudoVDA" : "Vibeshine";
      expect(wrapper.text()).toContain(`${driverName} driver: Watchdog failed`);
      const hint =
        active === "sudovda"
          ? messagesEn.config.virtual_display_status_hint_sudovda
          : messagesEn.config.virtual_display_status_hint_vibeshine;
      const otherHint =
        active === "sudovda"
          ? messagesEn.config.virtual_display_status_hint_vibeshine
          : messagesEn.config.virtual_display_status_hint_sudovda;
      expect(wrapper.text()).toContain(hint);
      expect(wrapper.text()).not.toContain(otherHint);
      expect(store.hasPendingPatch()).toBe(false);
      wrapper.unmount();
    },
  );
});
