<template>
  <div class="container">
    <h1 class="my-4">{{ $t("troubleshooting.troubleshooting") }}</h1>
    <div class="card p-2 my-4">
      <div class="card-body">
        <h2 id="close_apps">{{ $t("troubleshooting.force_close") }}</h2>
        <br />
        <p>{{ $t("troubleshooting.force_close_desc") }}</p>
        <div class="alert alert-success" v-if="closeAppStatus === true">
          {{ $t("troubleshooting.force_close_success") }}
        </div>
        <div class="alert alert-danger" v-if="closeAppStatus === false">
          {{ $t("troubleshooting.force_close_error") }}
        </div>
        <div>
          <button
            class="btn btn-warning"
            :disabled="closeAppPressed"
            @click="closeApp"
          >
            {{ $t("troubleshooting.force_close") }}
          </button>
        </div>
      </div>
    </div>
    <div class="card p-2 my-4">
      <div class="card-body">
        <h2 id="restart">{{ $t("troubleshooting.restart_sunshine") }}</h2>
        <br />
        <p>{{ $t("troubleshooting.restart_sunshine_desc") }}</p>
        <div class="alert alert-success" v-if="restartPressed === true">
          {{ $t("troubleshooting.restart_sunshine_success") }}
        </div>
        <div>
          <button
            class="btn btn-warning"
            :disabled="restartPressed"
            @click="restart"
          >
            {{ $t("troubleshooting.restart_sunshine") }}
          </button>
        </div>
      </div>
    </div>
    <div class="card p-2 my-4" v-if="platform === 'windows'">
      <div class="card-body">
        <h2 id="dd_reset">{{ $t("troubleshooting.dd_reset") }}</h2>
        <br />
        <p style="white-space: pre-line">
          {{ $t("troubleshooting.dd_reset_desc") }}
        </p>
        <div class="alert alert-success" v-if="ddResetStatus === true">
          {{ $t("troubleshooting.dd_reset_success") }}
        </div>
        <div class="alert alert-danger" v-if="ddResetStatus === false">
          {{ $t("troubleshooting.dd_reset_error") }}
        </div>
        <div>
          <button
            class="btn btn-warning"
            :disabled="ddResetPressed"
            @click="ddResetPersistence"
          >
            {{ $t("troubleshooting.dd_reset") }}
          </button>
        </div>
      </div>
    </div>
    <div class="card my-4">
      <div class="card-body">
        <div class="p-2">
          <div class="d-flex justify-content-end align-items-center">
            <h2 id="unpair" class="text-center me-auto">
              {{ $t("troubleshooting.unpair_title") }}
            </h2>
            <button
              class="btn btn-danger"
              :disabled="unpairAllPressed"
              @click="unpairAll"
            >
              {{ $t("troubleshooting.unpair_all") }}
            </button>
          </div>
          <br />
          <p class="mb-0">{{ $t("troubleshooting.unpair_desc") }}</p>
          <div class="alert alert-success mt-3" v-if="unpairAllStatus === true">
            {{ $t("troubleshooting.unpair_all_success") }}
          </div>
          <div class="alert alert-danger mt-3" v-if="unpairAllStatus === false">
            {{ $t("troubleshooting.unpair_all_error") }}
          </div>
        </div>
      </div>
      <ul
        class="list-group list-group-flush list-group-item-light"
        v-if="clients && clients.length > 0"
      >
        <div
          v-for="client in clients"
          class="list-group-item d-flex"
          :key="client.uuid"
        >
          <div class="p-2 flex-grow-1">
            {{
              client.name !== ""
                ? client.name
                : $t("troubleshooting.unpair_single_unknown")
            }}
          </div>
          <div
            class="me-2 ms-auto btn btn-danger"
            @click="unpairSingle(client.uuid)"
          >
            <font-awesome-icon icon="trash" />
          </div>
        </div>
      </ul>
      <ul v-else class="list-group list-group-flush list-group-item-light">
        <div class="list-group-item p-3 text-center">
          <em>{{ $t("troubleshooting.unpair_single_no_devices") }}</em>
        </div>
      </ul>
    </div>
    <div class="card p-2 my-4">
      <div class="card-body">
        <h2 id="logs">{{ $t("troubleshooting.logs") }}</h2>
        <br />
        <div class="d-flex justify-content-between align-items-baseline py-2">
          <p>{{ $t("troubleshooting.logs_desc") }}</p>
          <input
            type="text"
            class="form-control"
            v-model="logFilter"
            :placeholder="$t('troubleshooting.logs_find')"
            style="width: 300px"
          />
        </div>
        <div>
          <div class="troubleshooting-logs position-relative">
            <button class="copy-icon" @click="copyLogs">
              <font-awesome-icon icon="copy" /></button
            >{{ actualLogs }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
<script>
export default {
  data() {
    return {
      clients: [],
      closeAppPressed: false,
      closeAppStatus: null,
      ddResetPressed: false,
      ddResetStatus: null,
      logs: "Loading...",
      logFilter: null,
      logInterval: null,
      restartPressed: false,
      platform: "",
      unpairAllPressed: false,
      unpairAllStatus: null,
    };
  },
  computed: {
    actualLogs() {
      if (!this.logFilter) return this.logs;
      return this.logs
        .split("\n")
        .filter((l) => l.indexOf(this.logFilter) !== -1)
        .join("\n");
    },
  },
  created() {
    fetch("/api/config")
      .then((r) => r.json())
      .then((r) => {
        this.platform = r.platform;
      });
    this.logInterval = setInterval(() => this.refreshLogs(), 5000);
    this.refreshLogs();
    this.refreshClients();
  },
  beforeUnmount() {
    clearInterval(this.logInterval);
  },
  methods: {
    refreshLogs() {
      fetch("./api/logs")
        .then((r) => r.text())
        .then((r) => {
          this.logs = r;
        });
    },
    closeApp() {
      this.closeAppPressed = true;
      fetch("./api/apps/close", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
      })
        .then((r) => r.json())
        .then((r) => {
          this.closeAppPressed = false;
          this.closeAppStatus = r.status;
          setTimeout(() => (this.closeAppStatus = null), 5000);
        });
    },
    unpairAll() {
      this.unpairAllPressed = true;
      fetch("./api/clients/unpair-all", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
      })
        .then((r) => r.json())
        .then((r) => {
          this.unpairAllPressed = false;
          this.unpairAllStatus = r.status;
          setTimeout(() => (this.unpairAllStatus = null), 5000);
          this.refreshClients();
        });
    },
    unpairSingle(uuid) {
      fetch("./api/clients/unpair", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ uuid }),
      }).then(() => this.refreshClients());
    },
    refreshClients() {
      fetch("./api/clients/list")
        .then((r) => r.json())
        .then((resp) => {
          if (
            resp.status === true &&
            resp.named_certs &&
            resp.named_certs.length
          ) {
            this.clients = resp.named_certs.sort((a, b) =>
              a.name.toLowerCase() > b.name.toLowerCase() || a.name === ""
                ? 1
                : -1
            );
          } else {
            this.clients = [];
          }
        });
    },
    copyLogs() {
      navigator.clipboard.writeText(this.actualLogs);
    },
    restart() {
      this.restartPressed = true;
      setTimeout(() => (this.restartPressed = false), 5000);
      fetch("./api/restart", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
      });
    },
    ddResetPersistence() {
      this.ddResetPressed = true;
      fetch("/api/reset-display-device-persistence", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
      })
        .then((r) => r.json())
        .then((r) => {
          this.ddResetPressed = false;
          this.ddResetStatus = r.status;
          setTimeout(() => (this.ddResetStatus = null), 5000);
        });
    },
  },
};
</script>
<style scoped>
.troubleshooting-logs {
  white-space: pre;
  font-family: monospace;
  overflow: auto;
  max-height: 500px;
  min-height: 500px;
  font-size: 16px;
  position: relative;
  padding: 0.5rem 0.75rem;
  background: var(--bs-body-bg);
  border: 1px solid var(--bs-border-color);
  border-radius: 4px;
}
.copy-icon {
  position: absolute;
  top: 8px;
  right: 8px;
  padding: 8px;
  cursor: pointer;
  color: rgba(0, 0, 0, 1);
  appearance: none;
  border: none;
  background: none;
}
[data-bs-theme="dark"] .copy-icon {
  color: rgba(255, 255, 255, 0.85);
}
.copy-icon:hover {
  color: rgba(0, 0, 0, 0.75);
}
[data-bs-theme="dark"] .copy-icon:hover {
  color: rgba(255, 255, 255, 0.65);
}
.copy-icon:active {
  color: rgba(0, 0, 0, 1);
}
[data-bs-theme="dark"] .copy-icon:active {
  color: rgba(255, 255, 255, 0.9);
}
</style>
