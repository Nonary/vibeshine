<template>
  <div class="container">
    <h1 class="my-4 text-center">{{ $t("pin.pin_pairing") }}</h1>
    <form
      class="form d-flex flex-column align-items-center"
      @submit.prevent="registerDevice"
    >
      <div class="card flex-column d-flex p-4 mb-4">
        <input
          v-model="pin"
          type="text"
          pattern="\d*"
          :placeholder="$t('navbar.pin')"
          autofocus
          class="form-control mt-2"
          required
        />
        <input
          v-model="name"
          type="text"
          :placeholder="$t('pin.device_name')"
          class="form-control my-4"
          required
        />
        <button class="btn btn-primary">{{ $t("pin.send") }}</button>
      </div>
      <div class="alert alert-warning">
        <b>{{ $t("_common.warning") }}</b> {{ $t("pin.warning_msg") }}
      </div>
      <div v-html="statusHtml"></div>
    </form>
  </div>
</template>
<script>
export default {
  data() {
    return { pin: "", name: "", statusHtml: "" };
  },
  methods: {
    registerDevice() {
      this.statusHtml = "";
      fetch("./api/pin", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ pin: this.pin, name: this.name }),
      })
        .then((r) => r.json())
        .then((r) => {
          if (r.status) {
            this.statusHtml = `<div class=\"alert alert-success\">${this.$i18n.t(
              "pin.pair_success"
            )}</div>`;
            this.pin = "";
            this.name = "";
          } else {
            this.statusHtml = `<div class=\"alert alert-danger\">${this.$i18n.t(
              "pin.pair_failure"
            )}</div>`;
          }
        });
    },
  },
};
</script>
