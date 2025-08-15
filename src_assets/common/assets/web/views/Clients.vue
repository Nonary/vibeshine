<template>
  <div class="container py-4">
    <h1>{{ $t('clients.title') }}</h1>
    <p class="text-muted">{{ $t('clients.description') }}</p>
    <div v-if="loading">{{ $t('common.loading') }}</div>
    <table v-else class="table">
      <thead>
        <tr>
          <th>{{ $t('clients.name') }}</th>
          <th>{{ $t('clients.ip') }}</th>
          <th>{{ $t('clients.lastSeen') }}</th>
          <th></th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="c in clients" :key="c.id">
          <td>{{ c.name }}</td>
          <td>{{ c.ip }}</td>
          <td>{{ c.lastSeen }}</td>
          <td>
            <button class="btn btn-sm btn-danger" @click="unpair(c.id)">{{ $t('clients.unpair') }}</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script>
export default {
  data() {
    return {
      loading: true,
      clients: [],
    }
  },
  methods: {
    async load() {
      this.loading = true;
      const r = await fetch('./api/clients');
      if (r.ok) this.clients = await r.json();
      this.loading = false;
    },
    async unpair(id) {
      if (!confirm(this.$t('clients.confirmUnpair'))) return;
      await fetch(`./api/clients/${id}`, { method: 'DELETE' });
      this.load();
    }
  },
  mounted() { this.load(); }
}
</script>

<style scoped>
/* simple styling to match other tables */
</style>
