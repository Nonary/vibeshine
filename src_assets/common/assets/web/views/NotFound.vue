<template>
  <div class="nf-wrapper">
    <Navbar />
    <main
      class="nf-main container d-flex flex-column align-items-center justify-content-center text-center"
    >
      <div class="nf-card shadow-sm p-4 p-md-5">
        <div class="nf-code display-3 fw-bold mb-2">404</div>
        <h1 class="h3 mb-3">{{ t("errors.404") }}</h1>
        <p class="text-muted mb-4">{{ subtitle }}</p>
        <div class="d-flex gap-2 justify-content-center flex-wrap">
          <router-link to="/" class="btn btn-primary btn-lg">
            <font-awesome-icon icon="home" class="me-2" />
            {{ t("errors.backHome") }}
          </router-link>
          <button
            type="button"
            class="btn btn-outline-secondary btn-lg"
            @click="goBack"
          >
            <font-awesome-icon icon="arrow-left" class="me-2" />
            {{ t("_common.dismiss") }}
          </button>
        </div>
      </div>
    </main>
  </div>
</template>

<script setup>
import { computed } from "vue";
import { useI18n } from "vue-i18n";
import { useRouter } from "vue-router";

const Navbar = () => import("../Navbar.vue");
const { t } = useI18n();
const router = useRouter();

const goBack = () => {
  if (window.history.length > 1) router.back();
  else router.push("/");
};

// Provide a little dynamic flavor (host / path)
const subtitle = computed(
  () => `${window.location.host}${window.location.pathname}`
);
</script>

<style scoped>
.nf-wrapper {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}
.nf-main {
  flex: 1 1 auto;
}
.nf-card {
  max-width: 640px;
  border-radius: 1rem;
  background: var(--bs-body-bg);
}
.nf-code {
  letter-spacing: 0.15em;
}
@media (prefers-color-scheme: dark) {
  .nf-card {
    background: #1f1f21;
  }
}
</style>
