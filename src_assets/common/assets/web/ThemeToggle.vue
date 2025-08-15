<script setup>
import {
  loadAutoTheme,
  setupThemeToggleListener,
  getPreferredTheme,
} from "./theme";
import { onMounted, ref } from "vue";

const activeIcon = ref("circle-half-stroke");
function updateActiveIcon(theme) {
  if (theme === "light") activeIcon.value = "sun";
  else if (theme === "dark") activeIcon.value = "moon";
  else activeIcon.value = "circle-half-stroke";
}

onMounted(() => {
  loadAutoTheme();
  setupThemeToggleListener();
  updateActiveIcon(getPreferredTheme());
  window.addEventListener("sunshine-theme-change", (e) =>
    updateActiveIcon(e.detail.theme)
  );
});
</script>

<template>
  <div class="dropdown bd-mode-toggle">
    <a
      class="nav-link dropdown-toggle align-items-center"
      id="bd-theme"
      type="button"
      aria-expanded="false"
      data-bs-toggle="dropdown"
      aria-label="{{ $t('navbar.toggle_theme') }} ({{ $t('navbar.theme_auto') }})"
    >
      <span class="bi my-1 theme-icon-active"
        ><font-awesome-icon :icon="activeIcon"
      /></span>
      <span id="bd-theme-text">{{ $t("navbar.toggle_theme") }}</span>
    </a>
    <ul class="dropdown-menu dropdown-menu-end" aria-labelledby="bd-theme-text">
      <li>
        <button
          type="button"
          class="dropdown-item d-flex align-items-center"
          data-bs-theme-value="light"
          aria-pressed="false"
        >
          <font-awesome-icon icon="sun" class="bi me-2 theme-icon fa-fw" />
          {{ $t("navbar.theme_light") }}
        </button>
      </li>
      <li>
        <button
          type="button"
          class="dropdown-item d-flex align-items-center"
          data-bs-theme-value="dark"
          aria-pressed="false"
        >
          <font-awesome-icon icon="moon" class="bi me-2 theme-icon fa-fw" />
          {{ $t("navbar.theme_dark") }}
        </button>
      </li>
      <li>
        <button
          type="button"
          class="dropdown-item d-flex align-items-center active"
          data-bs-theme-value="auto"
          aria-pressed="true"
        >
          <font-awesome-icon
            icon="circle-half-stroke"
            class="bi me-2 theme-icon fa-fw"
          />
          {{ $t("navbar.theme_auto") }}
        </button>
      </li>
    </ul>
  </div>
</template>

<style scoped></style>
