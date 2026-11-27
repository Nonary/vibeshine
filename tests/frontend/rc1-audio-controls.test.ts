import { afterEach, describe, expect, it, vi } from 'vitest';
import { mount } from '@vue/test-utils';
import { createPinia, setActivePinia } from 'pinia';
import { createI18n } from 'vue-i18n';
import AudioVideo from '@/configs/tabs/AudioVideo.vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import legacyEn from '@/public/assets/locale/en.json';
import legacyDe from '@/public/assets/locale/de.json';
import modernEn from '../../src_assets/common/assets/web/public/assets/locale/en.json';
import modernDe from '../../src_assets/common/assets/web/public/assets/locale/de.json';
import { settingsCategories, restartRequiredKeys } from '../../src_assets/common/assets/web/configs/settingsSchema';

afterEach(() => { vi.restoreAllMocks(); setActivePinia(undefined); });

describe('RC1 existing audio controls', () => {
  for (const [name, locale] of Object.entries({ legacyEn, legacyDe, modernEn, modernDe })) {
    it(`uses only the requested short labels in ${name}`, () => {
      expect(locale.config.audio_714_source).toBe('Audio capture');
      expect(locale.config.audio_714_off).toBe('Default');
      expect(locale.config.audio_usb_source).toBe('Vibe Speakers');
      expect(locale.config.audio_usb_sample_format).toBe('Bit depth');
      expect(locale.config.audio_714_pcm24).toBe('24-bit');
      expect(locale.config.audio_714_pcm16).toBe('16-bit');
      expect(locale.config.audio_714_source_desc).toBe('');
    });
  }
  it('keeps the modern source/precision controls and next-session semantics', () => {
    const fields = settingsCategories.flatMap(c => c.groups.flatMap(g => g.fields));
    const source = fields.find(f => f.key === 'audio_714_source')!;
    const bits = fields.find(f => f.key === 'audio_usb_sample_format')!;
    expect(source.options?.map(o => o.value)).toEqual(['off', 'vibe_usb']);
    expect(source.descriptionKey).toBeUndefined();
    expect(bits.options?.map(o => o.value)).toEqual(['pcm24', 'pcm16']);
    expect(bits.visibleWhen).toEqual({ key: 'audio_714_source', equals: 'vibe_usb' });
    expect(restartRequiredKeys.has('audio_714_source')).toBe(false);
    expect(restartRequiredKeys.has('audio_usb_sample_format')).toBe(false);
  });
  it('uses the real legacy models, saves precision, and preserves Default device choices', async () => {
    vi.spyOn(http, 'get').mockResolvedValue({ status: 200, data: {} } as never);
    const pinia = createPinia(); setActivePinia(pinia);
    const store = useConfigStore(); store.metadata = { platform: 'windows' };
    store.setConfig({ audio_sink: 'Existing physical output', virtual_sink: 'Existing virtual output' });
    const wrapper = mount(AudioVideo, { global: {
      plugins: [pinia, createI18n({ legacy: false, locale: 'en', messages: { en: legacyEn } })],
      provide: { i18n: { t: (key: string) => key } },
      stubs: { PlatformLayout: false, ConfigFieldRenderer: true, AdapterNameSelector: true,
        DisplayOutputSelector: true, DisplayDeviceOptions: true, DisplayModesSettings: true, FrameLimiterStep: true },
    } });
    const field = (key: string) => wrapper.findAllComponents(ConfigFieldRenderer).find(f => f.props('settingKey') === key);
    try {
      expect(store.config.audio_714_source).toBe('off');
      expect(field('audio_usb_sample_format')).toBeUndefined();
      expect(field('audio_714_source')!.props('options').map((o: { label: string }) => o.label)).toEqual(['Default', 'Vibe Speakers']);
      await field('audio_714_source')!.vm.$emit('update:modelValue', 'vibe_usb');
      expect(field('audio_usb_sample_format')!.props('options').map((o: { label: string }) => o.label)).toEqual(['24-bit', '16-bit']);
      await field('audio_usb_sample_format')!.vm.$emit('update:modelValue', 'pcm16');
      expect(store.config.audio_usb_sample_format).toBe('pcm16');
      expect(field('audio_sink')).toBeUndefined();
      await field('audio_714_source')!.vm.$emit('update:modelValue', 'off');
      expect(field('audio_usb_sample_format')).toBeUndefined();
      expect(field('audio_sink')).toBeDefined();
      expect(store.config.audio_sink).toBe('Existing physical output');
      expect(store.config.virtual_sink).toBe('Existing virtual output');
      expect(store.config.audio_usb_sample_format).toBe('pcm16');
    } finally { wrapper.unmount(); }
  });
});
