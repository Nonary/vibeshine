<template>
  <nav class="navbar navbar-light navbar-expand-lg navbar-background header">
    <div class="container-fluid">
      <a class="navbar-brand" href="./" title="Sunshine">
        <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine">
      </a>
      <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
              aria-controls="navbarSupportedContent" aria-expanded="false" aria-label="Toggle navigation">
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav me-auto mb-2 mb-lg-0">
          <li class="nav-item">
            <a class="nav-link" href="./"><i class="fas fa-fw fa-home"></i> {{ $t('navbar.home') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./pin"><i class="fas fa-fw fa-unlock"></i> {{ $t('navbar.pin') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./apps"><i class="fas fa-fw fa-stream"></i> {{ $t('navbar.applications') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./config"><i class="fas fa-fw fa-cog"></i> {{ $t('navbar.configuration') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./password"><i class="fas fa-fw fa-user-shield"></i> {{ $t('navbar.password') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./troubleshooting"><i class="fas fa-fw fa-info"></i> {{ $t('navbar.troubleshoot') }}</a>
          </li>
          <li class="nav-item" v-if="showAutosaveStatus">
            <span class="navbar-text autosave-status" :class="autosaveStatusClass">
              <i :class="autosaveStatusIcon"></i>
              {{ autosaveStatusText }}
            </span>
          </li>
          <li class="nav-item">
            <ThemeToggle/>
          </li>
        </ul>
      </div>
    </div>
  </nav>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'
import { initDiscord } from '@lizardbyte/shared-web/src/js/discord.js'

export default {
  components: { ThemeToggle },
  data() {
    return {
      autosaveStatus: 0, // 0=idle, 1=saving, 2=recently_applied, 3=pending_session_end, 4=pending_restart_approval
      showAutosaveStatus: false,
      statusCheckInterval: null
    }
  },
  computed: {
    autosaveStatusClass() {
      switch (this.autosaveStatus) {
        case 1: return 'status-saving'
        case 2: return 'status-applied'
        case 3: return 'status-pending-session'
        case 4: return 'status-pending-restart'
        default: return 'status-idle'
      }
    },
    autosaveStatusIcon() {
      switch (this.autosaveStatus) {
        case 1: return 'fas fa-spinner fa-spin'
        case 2: return 'fas fa-check'
        case 3: return 'fas fa-clock'
        case 4: return 'fas fa-exclamation-triangle'
        default: return ''
      }
    },
    autosaveStatusText() {
      switch (this.autosaveStatus) {
        case 1: return this.$t ? this.$t('autosave.saving') : 'Saving...'
        case 2: return this.$t ? this.$t('autosave.applied') : 'Applied'
        case 3: return this.$t ? this.$t('autosave.pending_session') : 'Pending session end'
        case 4: return this.$t ? this.$t('autosave.pending_restart') : 'Restart required'
        default: return ''
      }
    }
  },
  created() {
    console.log("Header mounted!")
  },
  mounted() {
    let el = document.querySelector("a[href='" + document.location.pathname + "']");
    if (el) el.classList.add("active")
    initDiscord();
    
    // Only show autosave status on config page
    if (window.location.pathname.includes('/config')) {
      this.startStatusPolling();
    }
  },
  beforeUnmount() {
    this.stopStatusPolling();
  },
  methods: {
    startStatusPolling() {
      this.checkAutosaveStatus();
      this.statusCheckInterval = setInterval(() => {
        this.checkAutosaveStatus();
      }, 2000); // Check every 2 seconds
    },
    
    stopStatusPolling() {
      if (this.statusCheckInterval) {
        clearInterval(this.statusCheckInterval);
        this.statusCheckInterval = null;
      }
    },
    
    async checkAutosaveStatus() {
      try {
        const response = await fetch('./api/config/status');
        if (response.ok) {
          const data = await response.json();
          this.autosaveStatus = data.status;
          this.showAutosaveStatus = this.autosaveStatus !== 0; // Show if not idle
        }
      } catch (error) {
        console.warn('Failed to check autosave status:', error);
      }
    }
  }
}
</script>

<style>
.navbar-background {
  background-color: #ffc400
}

.header .nav-link {
  color: rgba(0, 0, 0, .65) !important;
}

.header .nav-link.active {
  color: rgb(0, 0, 0) !important;
  font-weight: 500;
}

.header .nav-link:hover {
  color: rgb(0, 0, 0) !important;
  font-weight: 500;
}

.header .navbar-toggler {
  color: rgba(var(--bs-dark-rgb), .65) !important;
  border: var(--bs-border-width) solid rgba(var(--bs-dark-rgb), 0.15) !important;
}

.header .navbar-toggler-icon {
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%2833, 37, 41, 0.75%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e") !important;
}

.form-control::placeholder {
  opacity: 0.5;
}

/* Autosave status indicator styles */
.autosave-status {
  font-size: 0.875rem;
  padding: 0.25rem 0.75rem;
  border-radius: 0.375rem;
  margin: 0 0.5rem;
}

.status-saving {
  background-color: #0dcaf0;
  color: #000;
}

.status-applied {
  background-color: #198754;
  color: #fff;
}

.status-pending-session {
  background-color: #fd7e14;
  color: #000;
}

.status-pending-restart {
  background-color: #dc3545;
  color: #fff;
}

.status-idle {
  display: none;
}
</style>
