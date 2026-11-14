import assert from 'node:assert/strict';
import test from 'node:test';
import { createServer } from 'vite';
import { createSSRApp } from 'vue';
import { renderToString } from 'vue/server-renderer';
import { createI18n } from 'vue-i18n';

test('Windows driver component omits deferred startup but renders real results and request errors', async () => {
  const server = await createServer({
    server: { middlewareMode: true, hmr: false, watch: { ignored: ['**/*'] } },
  });
  try {
    const { default: component } = await server.ssrLoadModule(
      '/components/settings/WindowsDisplayStatus.vue',
    );
    const metadata = {
      platform: 'windows',
      virtual_display_driver: {
        configured: 'vibeshine',
        active: null,
        status: 'unknown',
        status_code: 1,
        initialization_attempted: false,
      },
    };
    async function render(props: Record<string, unknown>) {
      const app = createSSRApp(component, props);
      app.use(
        createI18n({
          legacy: false,
          locale: 'en',
          messages: { en: {} },
          missingWarn: false,
          fallbackWarn: false,
        }),
      );
      return renderToString(app);
    }
    assert.equal(await render({ metadata }), '<!---->');
    for (const initialization_attempted of [true, undefined, 'false']) {
      const recovery = await render({
        metadata: {
          ...metadata,
          virtual_display_driver: { ...metadata.virtual_display_driver, initialization_attempted },
        },
      });
      assert.match(recovery, /windows-display-status/);
      assert.match(recovery, /messages.unknown.title/);
    }
    assert.match(await render({ metadata, loading: true }), /windows-display-status/);
    const error = await render({ metadata, error: 'Unable to fetch driver status' });
    assert.match(error, /Unable to fetch driver status/);
    assert.match(error, /vs-inline-alert--danger/);
    for (const [status, tone] of [
      ['ready', 'success'],
      ['failed', 'danger'],
      ['version_incompatible', 'warning'],
      ['watchdog_failed', 'danger'],
      ['unexpected', 'info'],
    ] as const) {
      const html = await render({
        metadata: {
          ...metadata,
          virtual_display_driver: { configured: 'vibeshine', active: null, status },
        },
      });
      assert.match(html, /windows-display-status/);
      assert.ok(html.includes(`vs-inline-alert--${tone}`), status);
    }
    assert.equal(await render({ metadata }), '<!---->');
    assert.equal(
      await render({ metadata: { ...metadata, platform: 'linux' }, error: 'Error' }),
      '<!---->',
    );
  } finally {
    await server.close();
  }
});
