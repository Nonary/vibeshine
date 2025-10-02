<template>
  <section class="py-6 md:py-10 space-y-6">
    <header>
      <h2 class="text-xl font-semibold tracking-tight">
        {{ $t('password.password_change') }}
      </h2>
    </header>

    <n-card
      class="border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur rounded-2xl shadow-sm"
    >
      <n-form label-placement="top" class="space-y-6">
        <div class="grid grid-cols-1 lg:grid-cols-2 gap-6">
          <fieldset class="border border-dark/10 dark:border-light/10 rounded-2xl p-4 space-y-4">
            <legend class="px-2 text-sm font-semibold uppercase tracking-wide">
              {{ $t('password.current_creds') }}
            </legend>
            <n-form-item :label="$t('_common.username')">
              <n-input v-model:value="form.currentUsername" autocomplete="username" />
            </n-form-item>
            <n-form-item :label="$t('_common.password')">
              <n-input
                v-model:value="form.currentPassword"
                type="password"
                autocomplete="current-password"
                show-password-on="mousedown"
              />
            </n-form-item>
          </fieldset>

          <fieldset class="border border-dark/10 dark:border-light/10 rounded-2xl p-4 space-y-4">
            <legend class="px-2 text-sm font-semibold uppercase tracking-wide">
              {{ $t('password.new_creds') }}
            </legend>
            <n-form-item :label="$t('_common.username')">
              <n-input v-model:value="form.newUsername" autocomplete="username" />
              <div class="mt-1 text-xs opacity-70">
                {{ $t('password.new_username_desc') }}
              </div>
            </n-form-item>
            <n-form-item :label="$t('_common.password')">
              <n-input
                v-model:value="form.newPassword"
                type="password"
                autocomplete="new-password"
                show-password-on="mousedown"
              />
            </n-form-item>
            <n-form-item :label="$t('password.confirm_password')">
              <n-input
                v-model:value="form.confirmNewPassword"
                type="password"
                autocomplete="new-password"
                show-password-on="mousedown"
              />
            </n-form-item>
          </fieldset>
        </div>

        <n-alert v-if="error" type="error" closable @close="error = null">
          {{ error }}
        </n-alert>
        <n-alert v-else-if="success" type="success">
          {{ $t('password.success_msg') }}
        </n-alert>

        <div class="flex justify-end">
          <n-button type="primary" :loading="loading" @click="submit">
            {{ $t('_common.save') }}
          </n-button>
        </div>
      </n-form>
    </n-card>
  </section>
</template>

<script setup lang="ts">
import { reactive, ref, onBeforeUnmount } from 'vue';
import { useI18n } from 'vue-i18n';
import { http } from '@/http';
import { NCard, NForm, NFormItem, NInput, NAlert, NButton } from 'naive-ui';

const { t } = useI18n();

const form = reactive({
  currentUsername: '',
  currentPassword: '',
  newUsername: '',
  newPassword: '',
  confirmNewPassword: '',
});

const loading = ref(false);
const error = ref<string | null>(null);
const success = ref(false);
let reloadTimer: ReturnType<typeof setTimeout> | null = null;

onBeforeUnmount(() => {
  if (reloadTimer) {
    clearTimeout(reloadTimer);
    reloadTimer = null;
  }
});

async function submit(): Promise<void> {
  if (loading.value) return;
  error.value = null;
  success.value = false;

  if (!form.currentUsername.trim()) {
    error.value = t('_common.username') + ' is required';
    return;
  }
  if (!form.newPassword) {
    error.value = 'New password is required';
    return;
  }
  if (!form.confirmNewPassword) {
    error.value = 'Confirm password is required';
    return;
  }
  if (form.newPassword !== form.confirmNewPassword) {
    error.value = 'Passwords do not match';
    return;
  }

  loading.value = true;
  try {
    const payload = {
      currentUsername: form.currentUsername,
      currentPassword: form.currentPassword,
      newUsername: form.newUsername,
      newPassword: form.newPassword,
      confirmNewPassword: form.confirmNewPassword,
    };
    const response = await http.post('/api/password', payload, {
      validateStatus: () => true,
    });
    if (response.status === 200) {
      const data = response.data as { status?: boolean; error?: string };
      if (data?.status === true) {
        success.value = true;
        form.currentPassword = '';
        form.newPassword = '';
        form.confirmNewPassword = '';
        reloadTimer = setTimeout(() => window.location.reload(), 5000);
      } else {
        error.value = data?.error || 'Password update failed';
      }
    } else {
      error.value = 'Internal Server Error';
    }
  } catch (e: any) {
    error.value = e?.message || 'Password update failed';
  } finally {
    loading.value = false;
  }
}
</script>
