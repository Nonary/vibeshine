/** @type {import('tailwindcss').Config} */
module.exports = {
  darkMode: 'class',
  content: [
    './src_assets/common/assets/web/**/*.{html,js,ts,vue}',
  ],
  theme: {
    extend: {
      colors: {
        // SOLAR (Light Mode - Sunshine)
        solar: {
          primary: '#FDB813',
          secondary: '#0D3B66',
          success: '#4CAF50',
          warning: '#F57C00',
          danger: '#D32F2F',
          info: '#0288D1',
          light: '#FFFDF8',
          dark: '#212121',
          surface: '#FFF3E0',
          accent: '#EF6C00',
          onPrimary: '#212121',
          onSecondary: '#FFFFFF',
          onAccent: '#FFFFFF',
          onLight: '#212121',
          onDark: '#FFFFFF',
        },
        // LUNAR (Dark Mode - Night Sky)
        lunar: {
          primary: '#4DA3FF',
          secondary: '#12224D',
          success: '#81C784',
          warning: '#FFB74D',
          danger: '#E57373',
          info: '#8CCBFF',
          light: '#F5F9FF',
          dark: '#050B1E',
          surface: '#0B1432',
          accent: '#8CCBFF',
          onPrimary: '#050B1E',
          onSecondary: '#F5F9FF',
          onAccent: '#050B1E',
          onLight: '#050B1E',
          onDark: '#F5F9FF',
        },
      },
    },
  },
  // Avoid conflicting with Bootstrap resets
  corePlugins: {
    preflight: false,
    visibility: false,
  },
  plugins: [],
};
