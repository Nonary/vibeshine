<template>
  <UiModal :open="visible" :dismissible="false" :backdrop-close="false" size="xl">
    <template #icon>
      <div class="h-14 w-14 rounded-full bg-gradient-to-br from-primary/20 to-primary/10 text-primary flex items-center justify-center shadow-inner">
        <i class="fas fa-lock text-xl" />
      </div>
    </template>
    <template #title>
      <div class="flex flex-col items-center gap-1 text-center">
        <h2 class="text-xl font-semibold tracking-tight">
          {{ credentialsConfigured ? t('auth.login_title') : t('auth.create_first_user') }}
        </h2>
        <p v-if="!credentialsConfigured" class="text-xs font-medium uppercase tracking-wider opacity-70">
          {{ t('auth.first_user_subtitle') }}
        </p>
      </div>
    </template>

    <form class="px-1 py-2 space-y-5" novalidate @submit.prevent="submit" @keydown.ctrl.enter.stop.prevent="submit">
      <div class="space-y-1">
        <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{ t('auth.username') }}</label>
        <input
          v-model.trim="username"
          required
          autocomplete="username"
          class="ui-input"
        />
      </div>
      <div v-if="credentialsConfigured" class="space-y-1">
        <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{ t('auth.password') }}</label>
        <input
          v-model.trim="password"
          type="password"
          required
          autocomplete="current-password"
          class="ui-input"
        />
      </div>
      <template v-else>
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{ t('auth.new_password') }}</label>
          <input
            v-model.trim="newPassword"
            type="password"
            required
            autocomplete="new-password"
            class="ui-input"
          />
        </div>
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">{{ t('auth.confirm_new_password') }}</label>
          <input
            v-model.trim="confirmNewPassword"
            type="password"
            required
            autocomplete="new-password"
            class="ui-input"
          />
        </div>
      </template>

      <div class="min-h-[1.25rem]">
        <p v-if="error" class="text-sm font-medium text-red-600 dark:text-red-400 flex items-center gap-2">
          <i class="fas fa-triangle-exclamation" /> {{ error }}
        </p>
        <p v-else-if="success" class="text-sm font-medium text-emerald-600 dark:text-emerald-400 flex items-center gap-2">
          <i class="fas fa-circle-check" /> {{ success }}
        </p>
      </div>

      <section class="sr-only">
        <button type="submit" tabindex="-1" aria-hidden="true"></button>
      </section>
    </form>

    <template #footer>
      <div class="flex items-center justify-end w-full">
        <UiButton :disabled="submitting" variant="primary" @click="submit">
          <span v-if="!credentialsConfigured">{{ submitting ? t('auth.creating_user') : t('auth.create_user') }}</span>
          <span v-else>{{ submitting ? t('auth.login_loading') : t('auth.login_sign_in') }}</span>
          <i v-if="submitting" class="fas fa-spinner fa-spin" />
        </UiButton>
      </div>
    </template>
  </UiModal>
</template>
<script setup>
import { computed, ref, watch } from 'vue';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import { useI18n } from 'vue-i18n';
import UiModal from '@/components/UiModal.vue';
import UiButton from '@/components/UiButton.vue';

const auth = useAuthStore();
const { t } = useI18n();

// Show modal only when auth layer is ready, it has requested login,
// and the user is not already authenticated. This prevents the modal
// from flashing or appearing for non-auth errors.
const visible = computed(() => auth.ready && auth.showLoginModal && !auth.isAuthenticated);
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
  error.value = '';
  success.value = '';
  if (submitting.value) return;
  submitting.value = true;
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
.dark .ui-input { background: rgba(13,16,28,0.65); border-color: rgba(255,255,255,0.14); color: #f5f9ff; }
</style>
