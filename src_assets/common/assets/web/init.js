import i18n from './locale'

// Auth-aware fetch wrapper: redirect to login on 401 while preserving current relative path securely
(function installAuthFetchRedirect() {
    if (typeof window === 'undefined' || window.__authFetchInstalled) return;
    window.__authFetchInstalled = true;
    const origFetch = window.fetch.bind(window);

    function sanitizePath(path) {
        try {
            if (typeof path !== 'string') return '/';
            // Only allow same-origin relative paths without protocol indicators
            if (!path.startsWith('/')) return '/';
            if (path.startsWith('//')) return '/';
            if (path.includes('://')) return '/';
            if (path.length > 512) return '/';
            return path;
        } catch { return '/'; }
    }

    async function authFetch(input, init) {
        const response = await origFetch(input, init);
        if (response && response.status === 401) {
            // Avoid redirect loops and ignore while already on login page
            if (!window.__redirectingToLogin && window.location.pathname !== '/login') {
                window.__redirectingToLogin = true;
                const current = sanitizePath(window.location.pathname + window.location.search + window.location.hash);
                try {
                    window.location.href = '/login?redirect=' + encodeURIComponent(current);
                } catch { /* noop */ }
            }
        }
        return response;
    }

    window.fetch = authFetch;
})();

// must import even if not implicitly using here
// https://github.com/aurelia/skeleton-navigation/issues/894
// https://discourse.aurelia.io/t/bootstrap-import-bootstrap-breaks-dropdown-menu-in-navbar/641/9
import 'bootstrap/dist/js/bootstrap'

export function initApp(app, config) {
    // Wait for locale initialization, then run optional app-level setup (like loading config)
    // If a `config` callback is provided it may be async — run it before mounting so
    // stores and components see the runtime config immediately.
    i18n().then(async i18n => {
        app.use(i18n);
        app.provide('i18n', i18n.global)
        if (config) {
            try {
                // allow `config` to be async and wait for it to complete
                await config(app)
            } catch (e) {
                // swallow errors to avoid blocking app entirely
                console.error('initApp: config loader failed', e)
            }
        }
        // Mount after any early initialization
        app.mount('#app');
    });
}
