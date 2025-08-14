import { createApp } from 'vue'
import { createPinia } from 'pinia'
import { initApp } from './init'
import { router } from './router'
import Shell from './layout/Shell.vue'
import './styles/tailwind.css'
// Import Font Awesome locally (added dependency @fortawesome/fontawesome-free)
import '@fortawesome/fontawesome-free/css/all.min.css'

const app = createApp(Shell)
const pinia = createPinia()
app.use(pinia)
app.use(router)

// Provide an early config loader so runtime config is fetched and applied
// before the app mounts. This prevents components from rendering then
// immediately fetching config (which caused null deref race conditions).
initApp(app, async (appInstance) => {
	try {
		// dynamic import to avoid circular load order issues in tests/build
		const [{ useConfigStore }, { useAuthStore }] = await Promise.all([
			import('./stores/config.js'),
			import('./stores/auth.js')
		])
		const store = useConfigStore()
		const auth = useAuthStore()
		// init lightweight auth detection polling
		auth.init()

		// if already loaded (e.g., HMR) skip
		if (store.config && store.config.value) return

		// If already authenticated, fetch immediately. Otherwise wait for login event once.
		const doFetch = async () => {
			try {
				const res = await fetch('/api/config')
				if (!res.ok) return
				const json = await res.json()
				store.setConfig(json)
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
