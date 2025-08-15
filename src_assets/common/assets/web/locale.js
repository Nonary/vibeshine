import { createI18n } from 'vue-i18n';

// Import only the fallback language files
import en from '@/public/assets/locale/en.json';
import { http } from '@/http.js';

export default async function () {
  let r = await http
    .get('./api/configLocale', { validateStatus: () => true })
    .then((r) => (r.status === 200 ? r.data : {}))
    .catch(() => ({}));
  let locale = r.locale ?? 'en';
  document.querySelector('html').setAttribute('lang', locale);
  let messages = {
    en,
  };
  try {
    if (locale !== 'en') {
      let r = await http
        .get(`/assets/locale/${locale}.json`, { validateStatus: () => true })
        .then((r) => (r.status === 200 ? r.data : null));
      if (r) messages[locale] = r;
    }
  } catch (e) {
    console.error('Failed to download translations', e);
  }
  const i18n = createI18n({
    locale: locale, // set locale
    fallbackLocale: 'en', // set fallback locale
    messages: messages,
  });
  return i18n;
}
