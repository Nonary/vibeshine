import { createApp, ref, watch } from 'vue'
import { createPinia } from 'pinia'
import { initApp } from '@/init'
import { router } from '@/router'
import App from '@/App.vue';
import './styles/tailwind.css'
import { initHttpLayer, http } from '@/http.js'
// Import Font Awesome locally (added dependency @fortawesome/fontawesome-free)
import '@fortawesome/fontawesome-free/css/all.min.css'

const app = createApp(App)
const pinia = createPinia()
app.use(pinia)
app.use(router)

// Provide a reactive platform ref early so feature/platform specific
// rendering & i18n (see platform-i18n.js) can safely inject it without
// crashing even before config has finished loading. We update it once
// the config store is populated. Components using inject('platform')
// will now always receive a ref (possibly empty string) eliminating
// previous 'injection "platform" not found' errors during early mount.
const platformRef = ref('')
app.provide('platform', platformRef)

// Provide an early config loader so runtime config is fetched and applied
// before the app mounts. This prevents components from rendering then
// immediately fetching config (which caused null deref race conditions).
initApp(app, async (appInstance) => {
	try {
		// dynamic import to avoid circular load order issues in tests/build
		const [{ useConfigStore }, { useAuthStore }] = await Promise.all([
			import('@/stores/config.js'),
			import('@/stores/auth.js')
		])
		const store = useConfigStore()
		const auth = useAuthStore()
		// Initialize axios/http layer & perform a single auth validate
		await initHttpLayer()
		auth.init()

		// Keep provided platform ref in sync with store once config arrives
		watch(() => store.config.value && store.config.value.platform, (val) => {
			platformRef.value = val || ''
		}, { immediate: true })

		// if already loaded (e.g., HMR) skip
		if (store.config && store.config.value) return

		// If already authenticated, fetch immediately. Otherwise wait for login event once.
		const doFetch = async () => {
			try {
				const res = await http.get('/api/config')
				if (res.status !== 200) return
				store.setConfig(res.data)
			} catch (e) {
				console.error('early config fetch failed', e)
			}
		}

		if (auth.isAuthenticated) {
			await doFetch()
		} else {
			// wait for first login event, but do not block forever — also allow immediate return
			let unsub = auth.onLogin(async () => {
				await doFetch()
				if (unsub) unsub()
			})
			// no further action here; mount will proceed, components remain defensive
		}
	} catch (e) {
		// don't block the app on config failures
		// components should still defensively handle missing config
		console.error('early config load failed', e)
	}
})
