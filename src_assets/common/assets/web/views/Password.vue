<template>
  <div class="container">
    <h1 class="my-4">{{ $t("password.password_change") }}</h1>
    <form @submit.prevent="save">
      <div class="card d-flex p-4 flex-row flex-wrap">
        <div class="col-md-6 px-4">
          <h4>{{ $t("password.current_creds") }}</h4>
          <div class="mb-3">
            <label class="form-label">{{ $t("_common.username") }}</label>
            <input
              required
              v-model="passwordData.currentUsername"
              type="text"
              class="form-control"
            />
          </div>
          <div class="mb-3">
            <label class="form-label">{{ $t("_common.password") }}</label>
            <input
              v-model="passwordData.currentPassword"
              autocomplete="current-password"
              type="password"
              class="form-control"
            />
          </div>
        </div>
        <div class="col-md-6 px-4">
          <h4>{{ $t("password.new_creds") }}</h4>
          <div class="mb-3">
            <label class="form-label">{{ $t("_common.username") }}</label>
            <input
              v-model="passwordData.newUsername"
              type="text"
              class="form-control"
            />
            <div class="form-text">{{ $t("password.new_username_desc") }}</div>
          </div>
          <div class="mb-3">
            <label class="form-label">{{ $t("_common.password") }}</label>
            <input
              required
              v-model="passwordData.newPassword"
              autocomplete="new-password"
              type="password"
              class="form-control"
            />
          </div>
          <div class="mb-3">
            <label class="form-label">{{
              $t("password.confirm_password")
            }}</label>
            <input
              required
              v-model="passwordData.confirmNewPassword"
              autocomplete="new-password"
              type="password"
              class="form-control"
            />
          </div>
        </div>
      </div>
      <div class="alert alert-danger" v-if="error">
        <b>Error: </b>{{ error }}
      </div>
      <div class="alert alert-success" v-if="success">
        <b>{{ $t("_common.success") }}</b> {{ $t("password.success_msg") }}
      </div>
      <div class="mb-3 buttons">
        <button class="btn btn-primary">{{ $t("_common.save") }}</button>
      </div>
    </form>
  </div>
</template>
<script>
export default {
  data() {
    return {
      error: null,
      success: false,
      passwordData: {
        currentUsername: "",
        currentPassword: "",
        newUsername: "",
        newPassword: "",
        confirmNewPassword: "",
      },
    };
  },
  methods: {
    save() {
      this.error = null;
      fetch("./api/password", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(this.passwordData),
      }).then((r) => {
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
