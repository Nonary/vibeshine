import { fileURLToPath, URL } from 'node:url';
import fs from 'fs';
import { resolve } from 'path';
import { defineConfig } from 'vite';
import { ViteEjsPlugin } from 'vite-plugin-ejs';
import { codecovVitePlugin } from '@codecov/vite-plugin';
import vue from '@vitejs/plugin-vue';
import process from 'process';

/**
 * Before actually building the pages with Vite, we do an intermediate build step using ejs
 * Importing this separately and joining them using ejs
 * allows us to split some repeating HTML that cannot be added
 * by Vue itself (e.g. style/script loading, common meta head tags, Widgetbot)
 * The vite-plugin-ejs handles this automatically
 */
let assetsSrcPath = 'src_assets/common/assets/web';
let assetsDstPath = 'build/assets/web';

if (process.env.SUNSHINE_BUILD_HOMEBREW) {
  console.log('Building for homebrew, using default paths');
} else {
  // If the paths supplied in the environment variables contain any symbolic links
  // at any point in the series of directories, the entire build will fail with
  // a cryptic error message like this:
  //     RollupError: The "fileName" or "name" properties of emitted chunks and assets
  //     must be strings that are neither absolute nor relative paths.
  // To avoid this, we resolve the potential symlinks using `fs.realpathSync` before
  // doing anything else with the paths.
  if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
    let path = resolve(
      fs.realpathSync(process.env.SUNSHINE_SOURCE_ASSETS_DIR),
      'common/assets/web',
    );
    console.log('Using srcdir from Cmake: ' + path);
    assetsSrcPath = path;
  }
  if (process.env.SUNSHINE_ASSETS_DIR) {
    let path = resolve(fs.realpathSync(process.env.SUNSHINE_ASSETS_DIR), 'assets/web');
    console.log('Using destdir from Cmake: ' + path);
    assetsDstPath = path;
  }
}

let header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'));

// https://vitejs.dev/config/
const isDev =
  process.env.npm_lifecycle_event === 'build:debug' || process.env.NODE_ENV === 'development';

// When building in development mode (used by our CMake debug build),
// produce full source maps and do not minify so Vue devtools can step
// through original source.
export default defineConfig({
  resolve: {
    alias: {
      '@': resolve(assetsSrcPath),
      vue: 'vue/dist/vue.esm-bundler.js',
    },
  },
  base: '/',
  plugins: [
    vue(),
    ViteEjsPlugin({ header }),
    // Embed original source files into inline sourcemaps for debug builds so
    // browser devtools can display .vue sources even when served from the
    // compiled output directory.
    (function embedSourcesPlugin() {
      return {
        name: 'embed-sources-in-sourcemap',
        apply: 'build',
        async writeBundle(options, bundle) {
          if (!isDev) return;
          try {
            const outDir = resolve(assetsDstPath);
            const files = fs.readdirSync(outDir);
            for (const file of files) {
              if (!file.endsWith('.js')) continue;
              const full = resolve(outDir, file);
              let content = fs.readFileSync(full, 'utf8');
              // Look for inline source map (base64)
              const regex =
                /\/\/[#@] sourceMappingURL=data:application\/json(?:;charset=utf-8)?;base64,([A-Za-z0-9+/=]+)$/;
              const match = content.match(regex);
              if (!match) continue;
              const decoded = Buffer.from(match[1], 'base64').toString('utf8');
              const map = JSON.parse(decoded);
              // If sourcesContent is missing or all null, try to populate it
              const needsEmbed =
                !map.sourcesContent ||
                map.sourcesContent.every((s) => s === null || s === undefined);
              if (!needsEmbed) continue;
              map.sourcesContent = map.sources.map((src) => {
                // Attempt to resolve source paths relative to source assets dir
                try {
                  let candidate = src;
                  // If path looks like an absolute Windows drive or absolute posix, try it directly
                  if (!fs.existsSync(candidate)) {
                    // Remove leading slash if present
                    if (candidate.startsWith('/')) candidate = candidate.slice(1);
                    candidate = resolve(assetsSrcPath, candidate);
                  }
                  if (fs.existsSync(candidate)) {
                    return fs.readFileSync(candidate, 'utf8');
                  }
                } catch (e) {
                  // ignore and fallthrough
                }
                return null;
              });
              const newBase = Buffer.from(JSON.stringify(map)).toString('base64');
              content = content.replace(
                regex,
                `//# sourceMappingURL=data:application/json;base64,${newBase}`,
              );
              fs.writeFileSync(full, content, 'utf8');
            }
          } catch (e) {
            console.error('embed-sources-in-sourcemap plugin failed:', e);
          }
        },
      };
    })(),
    // The Codecov vite plugin should be after all other plugins
    codecovVitePlugin({
      enableBundleAnalysis: process.env.CODECOV_TOKEN !== undefined,
      bundleName: 'sunshine',
      uploadToken: process.env.CODECOV_TOKEN,
    }),
  ],
  root: resolve(assetsSrcPath),
  build: {
    outDir: resolve(assetsDstPath),
    // full inline source maps in debug so original .vue/.ts sources are embedded
    // and visible directly in browser devtools. Otherwise use default (hidden) source maps.
    sourcemap: isDev ? 'inline' : false,
    // avoid minification in debug so code is readable in browser devtools
    minify: isDev ? false : 'esbuild',
    rollupOptions: {
      input: {
        index: resolve(assetsSrcPath, 'index.html'),
      },
    },
  },
});
