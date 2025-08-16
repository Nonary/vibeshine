import fs from 'fs';
import { resolve } from 'path';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { ViteEjsPlugin } from 'vite-plugin-ejs';

let assetsSrcPath = 'src_assets/common/assets/web';
let assetsDstPath = 'build/assets/web';

// Allow CMake to override paths (and resolve symlinks)
if (!process.env.SUNSHINE_BUILD_HOMEBREW) {
  if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
    assetsSrcPath = resolve(
      fs.realpathSync(process.env.SUNSHINE_SOURCE_ASSETS_DIR),
      'common/assets/web',
    );
  }
  if (process.env.SUNSHINE_ASSETS_DIR) {
    assetsDstPath = resolve(fs.realpathSync(process.env.SUNSHINE_ASSETS_DIR), 'assets/web');
  }
}

const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'), 'utf-8');

export default defineConfig(({ mode }) => {
  const isDebug = mode === 'debug';

  return {
    root: resolve(assetsSrcPath),
    base: '/',
    resolve: {
      alias: { '@': resolve(assetsSrcPath) },
    },
    plugins: [vue(), ViteEjsPlugin({ header })],
    css: {
      // Include CSS sources in sourcemaps during debug
      devSourcemap: isDebug,
    },
    build: {
      outDir: resolve(assetsDstPath),
      sourcemap: isDebug ? 'inline' : false,
      minify: isDebug ? false : 'esbuild',
      rollupOptions: {
        input: { index: resolve(assetsSrcPath, 'index.html') },
      },
    },
  };
});
