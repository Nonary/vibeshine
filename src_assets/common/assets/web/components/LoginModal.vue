<template>
  <transition name="fade-fast">
    <div v-if="visible" class="fixed inset-0 z-[100] flex flex-col">
      <div class="absolute inset-0 bg-gradient-to-br from-black/70 via-black/60 to-black/70 backdrop-blur-md"
        @click="onBackdrop"></div>
      <div class="relative flex-1 flex flex-col items-center justify-center p-4 overflow-y-auto">
        <div
          class="w-full max-w-lg rounded-2xl shadow-2xl border border-white/10 dark:border-white/10 bg-white/80 dark:bg-neutral-900/80 backdrop-blur-xl ring-1 ring-black/5 dark:ring-white/5">
          <header class="px-6 py-5 border-b border-black/10 dark:border-white/10 flex items-center gap-3">
            <div class="flex-1">
              <h2 class="text-xl font-semibold tracking-tight text-gray-900 dark:text-white select-none">
                {{ credentialsConfigured ? t('auth.login_title') : t('auth.create_first_user') }}
              </h2>
              <p v-if="!credentialsConfigured"
                class="mt-1 text-xs font-medium uppercase tracking-wider text-gray-500 dark:text-gray-400">
                {{ t('auth.first_user_subtitle') }}
              </p>
            </div>
          </header>
          <form class="px-6 py-6 space-y-5" novalidate @submit.prevent="submit">
            <div class="space-y-1">
              <label class="block text-sm font-medium text-gray-700 dark:text-gray-300">{{
                t('auth.username')
                }}</label>
              <input v-model.trim="username" required autocomplete="username" autofocus
                class="w-full rounded-lg border border-gray-300/70 dark:border-gray-600/70 bg-white/70 dark:bg-neutral-800/70 px-3 py-2 text-sm placeholder-gray-400 dark:placeholder-gray-500 focus:outline-none focus:ring-2 focus:ring-primary/50 focus:border-transparent transition" />
            </div>
            <div v-if="credentialsConfigured" class="space-y-1">
              <label class="block text-sm font-medium text-gray-700 dark:text-gray-300">{{
                t('auth.password')
                }}</label>
              <input v-model.trim="password" type="password" required autocomplete="current-password"
                class="w-full rounded-lg border border-gray-300/70 dark:border-gray-600/70 bg-white/70 dark:bg-neutral-800/70 px-3 py-2 text-sm focus:outline-none focus:ring-2 focus:ring-primary/50 focus:border-transparent transition" />
            </div>
            <template v-else>
              <div class="grid grid-cols-1 sm:grid-cols-2 gap-4">
                <div class="space-y-1 sm:col-span-2">
                  <label class="block text-sm font-medium text-gray-700 dark:text-gray-300">{{
                    t('auth.new_password')
                    }}</label>
                  <input v-model.trim="newPassword" type="password" required autocomplete="new-password"
                    class="w-full rounded-lg border border-gray-300/70 dark:border-gray-600/70 bg-white/70 dark:bg-neutral-800/70 px-3 py-2 text-sm focus:outline-none focus:ring-2 focus:ring-primary/50 focus:border-transparent transition" />
                </div>
                <div class="space-y-1 sm:col-span-2">
                  <label class="block text-sm font-medium text-gray-700 dark:text-gray-300">{{
                    t('auth.confirm_new_password')
                    }}</label>
                  <input v-model.trim="confirmNewPassword" type="password" required autocomplete="new-password"
                    class="w-full rounded-lg border border-gray-300/70 dark:border-gray-600/70 bg-white/70 dark:bg-neutral-800/70 px-3 py-2 text-sm focus:outline-none focus:ring-2 focus:ring-primary/50 focus:border-transparent transition" />
                </div>
              </div>
            </template>
            <div class="min-h-[1.25rem]">
              <p v-if="error" class="text-sm font-medium text-red-600 dark:text-red-400 flex items-center gap-2">
                <i class="fas fa-triangle-exclamation" /> {{ error }}
              </p>
              <p v-else-if="success"
                class="text-sm font-medium text-emerald-600 dark:text-emerald-400 flex items-center gap-2">
                <i class="fas fa-circle-check" /> {{ success }}
              </p>
            </div>
            <div class="flex items-center justify-end pt-1">
              <button :disabled="submitting"
                class="inline-flex items-center justify-center gap-2 px-5 py-2.5 rounded-lg text-sm font-semibold tracking-wide bg-primary text-onPrimary shadow disabled:opacity-50 disabled:cursor-not-allowed hover:brightness-110 focus:outline-none focus-visible:ring-2 focus-visible:ring-offset-2 focus-visible:ring-primary/60 transition">
                <span v-if="!credentialsConfigured">{{
                  submitting ? t('auth.creating_user') : t('auth.create_user')
                  }}</span>
                <span v-else>{{
                  submitting ? t('auth.login_loading') : t('auth.login_sign_in')
                  }}</span>
                <i v-if="submitting" class="fas fa-spinner fa-spin" />
              </button>
            </div>
          </form>
        </div>
        <p class="mt-6 text-center text-[10px] tracking-wider text-gray-400 dark:text-gray-500 uppercase select-none">
          Sunshine
        </p>
      </div>
    </div>
  </transition>
</template>
<script setup>
import { computed, ref, watch, onMounted, onBeforeUnmount } from 'vue';
import { useAuthStore } from '@/stores/auth.js';
import { http } from '@/http.js';
import { useI18n } from 'vue-i18n';

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

function close() {
  // Prevent closing unless already authenticated
  if (auth.isAuthenticated) auth.hideLogin();
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

function onBackdrop(e) {
  e.stopPropagation();
  // do not close
}

function handleKey(e) {
  if (e.key === 'Escape') {
    e.preventDefault();
    e.stopPropagation();
  }
}

onMounted(() => {
  window.addEventListener('keydown', handleKey, true);
});
onBeforeUnmount(() => {
  window.removeEventListener('keydown', handleKey, true);
});
</script>
<style>
.fade-fast-enter-active,
.fade-fast-leave-active {
  transition: opacity 0.18s ease;
}

.fade-fast-enter-from,
.fade-fast-leave-to {
  opacity: 0;
}
</style>
