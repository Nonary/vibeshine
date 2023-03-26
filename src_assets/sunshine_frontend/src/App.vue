

<template>
  <nav class="navbar navbar-expand-lg navbar-light bg-warning fixed-top">
    <div class="container-fluid">
      <a class="navbar-brand" href="/" title="Sunshine">
        <img src="@/assets/images/logo-sunshine-45.png" height="45" alt="Sunshine">
      </a>
      <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
        aria-controls="navbarSupportedContent" aria-expanded="false" aria-label="Toggle navigation">
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav">
          <li class="nav-item">
            <router-link class="nav-link" to="/" active-class="active">
              <font-awesome-icon :icon="['fas', 'home']" />
              Home
            </router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/pin" active-class="active">
              <font-awesome-icon :icon="['fas', 'unlock']" />
              PIN
            </router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/apps" active-class="active">
              <font-awesome-icon :icon="['fas', 'stream']" />
              Applications
            </router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/config" active-class="active">
              <font-awesome-icon :icon="['fas', 'cog']" />
              Configuration
            </router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/password" active-class="active">
              <font-awesome-icon :icon="['fas', 'user-shield']" />
              Change Password
            </router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/troubleshooting" active-class="active">
              <font-awesome-icon :icon="['fas', 'info']" />
              Troubleshooting
            </router-link>
          </li>
        </ul>
      </div>
    </div>
  </nav>

  <RouterView />
</template>

<style scoped>
header {
  line-height: 1.5;
  max-height: 100vh;
}

.logo {
  display: block;
  margin: 0 auto 2rem;
}

nav {
  width: 100%;
  font-size: 12px;
  text-align: center;
  margin-top: 2rem;
}

nav a.router-link-exact-active {
  color: var(--color-text);
}

nav a.router-link-exact-active:hover {
  background-color: transparent;
}

nav a {
  display: inline-block;
  padding: 0 1rem;
  border-left: 1px solid var(--color-border);
}

nav a:first-of-type {
  border: 0;
}

@media (min-width: 1024px) {
  header {
    display: flex;
    place-items: center;
    padding-right: calc(var(--section-gap) / 2);
  }

  .logo {
    margin: 0 2rem 0 0;
  }

  header .wrapper {
    display: flex;
    place-items: flex-start;
    flex-wrap: wrap;
  }

  nav {
    text-align: left;
    margin-left: -1rem;
    font-size: 1rem;

    padding: 1rem 0;
    margin-top: 1rem;
  }
}
</style>
<script lang="ts">



import { RouterLink, RouterView } from 'vue-router'
//@ts-ignore
import { FontAwesomeIcon } from '@fortawesome/vue-fontawesome'
import { defineComponent } from 'vue';


export default defineComponent({
  components: {
    FontAwesomeIcon
  },
  setup() {
    // do nothing?
  }
});

</script>

<style>
.nav-link.active {
  font-weight: 500;
}
</style>
