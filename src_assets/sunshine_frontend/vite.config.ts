import { fileURLToPath, URL } from 'node:url'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [vue()],
  publicDir: './assets',
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  },
  optimizeDeps: {
    include: [
      '@fortawesome/fontawesome-free/css/all.min.css',
      'bootstrap/dist/css/bootstrap.min.css',
      'bootstrap/dist/js/bootstrap.bundle.min.js'
    ]
  }
})
