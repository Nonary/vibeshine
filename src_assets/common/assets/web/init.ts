import { App } from 'vue';
import { I18n } from 'vue-i18n';
import i18n from './locale';

// must import even if not implicitly using here
// https://github.com/aurelia/skeleton-navigation/issues/894
// https://discourse.aurelia.io/t/bootstrap-import-bootstrap-breaks-dropdown-menu-in-navbar/641/9
import 'bootstrap/dist/js/bootstrap';

export function initApp(app: App, config?: (app: App) => void): void {
    //Wait for locale initialization, then render
    i18n().then((i18nInstance: I18n) => {
        app.use(i18nInstance);
        app.provide('i18n', i18nInstance.global);
        app.mount('#app');
        if (config) {
            config(app);
        }
    });
}