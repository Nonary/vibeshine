import i18n from './locale'

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
