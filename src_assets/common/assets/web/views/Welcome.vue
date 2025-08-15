<template>
  <main role="main" style="max-width: 1200px; margin: 1em auto">
    <div class="d-flex gap-4 flex-wrap">
      <div class="card p-2 flex-grow-1" style="min-width: 300px">
        <header>
          <h1 class="mb-0">
            <img src="/images/logo-sunshine-45.png" height="45" alt="" />
            {{ $t("welcome.greeting") }}
          </h1>
        </header>
        <p class="my-2 align-self-start">{{ $t("welcome.create_creds") }}</p>
        <div class="alert alert-warning">
          {{ $t("welcome.create_creds_alert") }}
        </div>
        <form @submit.prevent="save">
          <div class="mb-2">
            <label class="form-label">{{ $t("_common.username") }}</label>
            <input
              v-model="passwordData.newUsername"
              type="text"
              class="form-control"
              autocomplete="username"
            />
          </div>
          <div class="mb-2">
            <label class="form-label">{{ $t("_common.password") }}</label>
            <input
              v-model="passwordData.newPassword"
              required
              type="password"
              class="form-control"
              autocomplete="new-password"
            />
          </div>
          <div class="mb-2">
            <label class="form-label">{{
              $t("welcome.confirm_password")
            }}</label>
            <input
              v-model="passwordData.confirmNewPassword"
              required
              type="password"
              class="form-control"
              autocomplete="new-password"
            />
          </div>
          <button
            type="submit"
            class="btn btn-primary w-100 mb-2"
            :disabled="loading"
          >
            {{ $t("welcome.login") }}
          </button>
          <div class="alert alert-danger" v-if="error">
            <b>{{ $t("_common.error") }}</b> {{ error }}
          </div>
          <div class="alert alert-success" v-if="success">
            <b>{{ $t("_common.success") }}</b>
            {{ $t("welcome.welcome_success") }}
          </div>
        </form>
      </div>
      <div style="min-width: 260px">
        <ResourceCard />
      </div>
    </div>
  </main>
</template>
<script>
import ResourceCard from "@/ResourceCard.vue";
export default {
  components: { ResourceCard },
  data() {
    return {
      error: null,
      success: false,
      loading: false,
      passwordData: {
        newUsername: "sunshine",
        newPassword: "",
        confirmNewPassword: "",
      },
    };
  },
  methods: {
    save() {
      this.error = null;
      this.loading = true;
      fetch("./api/password", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(this.passwordData),
      }).then((r) => {
        this.loading = false;
        if (r.status === 200) {
          r.json().then((rj) => {
            this.success = rj.status;
            if (this.success === true) {
              setTimeout(() => location.reload(), 5000);
            } else {
              this.error = rj.error;
            }
          });
        } else {
          this.error = "Internal Server Error";
        }
      });
    },
  },
};
</script>
