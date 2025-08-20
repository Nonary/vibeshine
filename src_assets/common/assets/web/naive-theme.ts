import type { GlobalThemeOverrides } from 'naive-ui';
import { ref, onMounted, onBeforeUnmount, watch } from 'vue';

// Use your existing CSS variables to keep the current color scheme.
// Naive UI accepts any valid CSS color string, so we reference the
// same tokens to maintain visual consistency across light/dark.

// Resolve `--color-xxx` (space-separated RGB like "77 163 255") to 'rgb(r, g, b)'
function cssVarRgb(name: string, fallback: string): string {
  if (typeof window === 'undefined') return fallback;
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  if (!raw) return fallback;
  // Accept formats like "77 163 255" or "77, 163, 255"
  const parts = raw.replace(/\s+/g, ' ').replace(/,/g, ' ').trim().split(' ');
  if (parts.length < 3) return fallback;
  const [r, g, b] = parts;
  const nr = Number(r),
    ng = Number(g),
    nb = Number(b);
  if ([nr, ng, nb].some((n) => !isFinite(n))) return fallback;
  return `rgb(${nr}, ${ng}, ${nb})`;
}

// Convert a cssVarRgb result to rgba with alpha. If the input isn't rgb(...),
// fall back to provided fallback color string.
function rgbaFromCssVar(name: string, fallbackRgb: string, alpha: number): string {
  const rgb = cssVarRgb(name, fallbackRgb);
  const m = /^rgb\((\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\)$/.exec(rgb);
  if (!m) return `rgba(0, 0, 0, ${alpha})`;
  const r = Number(m[1]);
  const g = Number(m[2]);
  const b = Number(m[3]);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

export function useNaiveThemeOverrides() {
  const overrides = ref<GlobalThemeOverrides>({});
  const compute = () => {
    overrides.value = {
      common: {
        primaryColor: cssVarRgb('--color-primary', '#18a058'),
        infoColor: cssVarRgb('--color-info', '#2080f0'),
        successColor: cssVarRgb('--color-success', '#18a058'),
        warningColor: cssVarRgb('--color-warning', '#f0a020'),
        errorColor: cssVarRgb('--color-danger', '#d03050'),

        baseColor: cssVarRgb('--color-light', '#ffffff'),
        bodyColor: cssVarRgb('--color-light', '#ffffff'),
        textColorBase: cssVarRgb('--color-dark', '#000000'),
        cardColor: cssVarRgb('--color-surface', '#ffffff'),
        modalColor: cssVarRgb('--color-surface', '#ffffff'),
        popoverColor: cssVarRgb('--color-surface', '#ffffff'),
        tableColor: cssVarRgb('--color-light', '#ffffff'),

        // Subtle borders/dividers using resolved RGB (avoid passing CSS vars to Naive color utils)
        borderColor: rgbaFromCssVar('--color-dark', 'rgb(0, 0, 0)', 0.1),
        dividerColor: rgbaFromCssVar('--color-dark', 'rgb(0, 0, 0)', 0.1),
      },
    } as GlobalThemeOverrides;
  };

  onMounted(compute);
  // Also export a small hook below that flags dark changes; recompute on changes
  const isDark = useDarkModeClassRef();
  watch(isDark, () => compute());

  return overrides;
}

// Small helper to sync Naive's theme with your existing dark-mode class.
// Usage: const isDark = useDarkModeClass();
export function useDarkModeClassRef() {
  const isDark = ref<boolean>(false);
  let observer: MutationObserver | null = null;

  const update = () => {
    if (typeof document !== 'undefined') {
      isDark.value = document.documentElement.classList.contains('dark');
    }
  };

  if (typeof window !== 'undefined') {
    update();
    onMounted(() => {
      update();
      observer = new MutationObserver(update);
      observer.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] });
    });
    onBeforeUnmount(() => {
      observer?.disconnect();
      observer = null;
    });
  }

  return isDark;
}
