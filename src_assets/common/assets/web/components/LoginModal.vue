<template>
  <n-modal :show="visible" :mask-closable="false" :close-on-esc="false">
    <n-card :style="'max-width: 48rem; width: 100%'" :bordered="false">
      <template #header>
        <div class="flex flex-col items-center gap-1 text-center w-full">
          <div
            class="h-14 w-14 rounded-full bg-gradient-to-br from-primary/20 to-primary/10 text-primary flex items-center justify-center shadow-inner mb-2"
          >
            <i class="fas fa-lock text-xl" />
          </div>
          <h2 class="text-xl font-semibold tracking-tight">
            {{ credentialsConfigured ? t('auth.login_title') : t('auth.create_first_user') }}
          </h2>
          <p
            v-if="!credentialsConfigured"
            class="text-xs font-medium uppercase tracking-wider opacity-70"
          >
            {{ t('auth.first_user_subtitle') }}
          </p>
        </div>
      </template>

      <form
        class="px-1 py-2 space-y-4"
        novalidate
        @submit.prevent="submit"
        @keydown.ctrl.enter.stop.prevent="submit"
      >
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{
            t('auth.username')
          }}</label>
          <n-input v-model:value="username" autocomplete="username" />
        </div>
        <div v-if="credentialsConfigured" class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{
            t('auth.password')
          }}</label>
          <n-input v-model:value="password" type="password" autocomplete="current-password" />
        </div>
        <template v-else>
          <div class="space-y-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{
              t('auth.new_password')
            }}</label>
            <n-input v-model:value="newPassword" type="password" autocomplete="new-password" />
          </div>
          <div class="space-y-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{
              t('auth.confirm_new_password')
            }}</label>
            <n-input
              v-model:value="confirmNewPassword"
              type="password"
              autocomplete="new-password"
            />
          </div>
        </template>

        <div class="min-h-[1.25rem]">
          <n-alert v-if="error" type="error" :show-icon="true">{{ error }}</n-alert>
          <n-alert v-else-if="success" type="success" :show-icon="true">{{ success }}</n-alert>
        </div>

        <section class="sr-only">
          <button type="submit" tabindex="-1" aria-hidden="true"></button>
        </section>
      </form>

      <template #footer>
        <div class="flex items-center justify-end w-full">
          <n-button type="primary" :disabled="submitting" :loading="submitting" @click="submit">
            <span v-if="!credentialsConfigured">{{
              submitting ? t('auth.creating_user') : t('auth.create_user')
            }}</span>
            <span v-else>{{ submitting ? t('auth.login_loading') : t('auth.login_sign_in') }}</span>
          </n-button>
        </div>
      </template>
    </n-card>
  </n-modal>
</template>
<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import { useI18n } from 'vue-i18n';
import { NModal, NCard, NInput, NAlert, NButton } from 'naive-ui';

const auth = useAuthStore();
const { t } = useI18n();

// Show modal only when auth layer is ready, it has requested login,
// and the user is not already authenticated. This prevents the modal
// from flashing or appearing for non-auth errors.
const visible = computed(
  () => auth.ready && auth.showLoginModal && !auth.isAuthenticated && !auth.logoutInitiated,
);
const credentialsConfigured = computed(() => auth.credentialsConfigured);

const username = ref('');
const password = ref('');
const newPassword = ref('');
const confirmNewPassword = ref('');
const error = ref('');
const success = ref('');
const submitting = ref(false);

watch(visible, (v) => {
  if (v) reset();
});

function reset() {
  username.value = '';
  password.value = '';
  newPassword.value = '';
  confirmNewPassword.value = '';
  error.value = '';
  success.value = '';
}

async function submit() {
  const MIN_LOGIN_DELAY_MS = 1000;
  const start = Date.now();
  error.value = '';
  success.value = '';
  if (submitting.value) return;
  submitting.value = true;
  try {
    auth.loggingIn.value = true;
  } catch {}
  try {
    if (!credentialsConfigured.value) {
      if (!newPassword.value || newPassword.value !== confirmNewPassword.value) {
        error.value = t('auth.password_mismatch');
        return;
      }
      // Use password save endpoint to create first credentials (no auth required when none configured)
      const res = await http.post(
        '/api/password',
        {
          currentUsername: username.value,
          currentPassword: newPassword.value, // treated as current when none exist
          newUsername: username.value,
          newPassword: newPassword.value,
          confirmNewPassword: confirmNewPassword.value,
        },
        { validateStatus: () => true },
      );
      if (res.status !== 200 || !res.data || !res.data.status) {
        error.value = res.data && res.data.error ? res.data.error : t('auth.create_user_failed');
        return;
      }
      auth.setCredentialsConfigured(true);
      success.value = t('auth.user_created');
      // Auto attempt login after slight delay
      await new Promise((r) => setTimeout(r, 250));
    }
    // Perform login
    const loginRes = await http.post(
      '/api/auth/login',
      {
        username: username.value,
        password: credentialsConfigured.value
          ? password.value || newPassword.value
          : newPassword.value,
        redirect: auth.pendingRedirect,
      },
      { validateStatus: () => true },
    );
    if (loginRes.status === 200 && loginRes.data && loginRes.data.status) {
      // Ensure the login feels deliberate: keep the loading state at least MIN_LOGIN_DELAY_MS
      const elapsed = Date.now() - start;
      if (elapsed < MIN_LOGIN_DELAY_MS) {
        await new Promise((r) => setTimeout(r, MIN_LOGIN_DELAY_MS - elapsed));
      }
      auth.setAuthenticated(true);
      success.value = t('auth.login_success');
      setTimeout(() => {
        auth.hideLogin();
      }, 400);
    } else {
      error.value =
        loginRes.data && loginRes.data.error ? loginRes.data.error : t('auth.login_failed');
    }
  } catch (e) {
    error.value = t('auth.login_network_error');
  } finally {
    submitting.value = false;
    try {
      auth.loggingIn.value = false;
    } catch {}
  }
}

// Backdrop and escape are disabled by UiModal when dismissible=false
</script>
<style scoped>
.ui-input {
  width: 100%;
  border: 1px solid rgba(0, 0, 0, 0.12);
  background: rgba(255, 255, 255, 0.85);
  padding: 10px 12px;
  border-radius: 10px;
  font-size: 14px;
}
.dark .ui-input {
  background: rgba(13, 16, 28, 0.65);
  border-color: rgba(255, 255, 255, 0.14);
  color: #f5f9ff;
}
/* Increase backdrop opacity and blur for stricter, less see-through modal */
/* Use deep selector to target Naive UI modal mask rendered via teleport */
:deep(.n-modal-mask) {
  background-color: rgba(0, 0, 0, 0.6) !important;
  backdrop-filter: blur(3px);
}
.dark :deep(.n-modal-mask) {
  background-color: rgba(0, 0, 0, 0.65) !important;
}
</style>
