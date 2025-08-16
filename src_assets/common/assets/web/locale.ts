import { createI18n, I18n } from 'vue-i18n';

// Import only the fallback language files
import en from '@/public/assets/locale/en.json';
import { http } from '@/http';

interface LocaleResponse {
  locale?: string;
}

type MessageSchema = typeof en;

export default async function (): Promise<any> {
  let r: LocaleResponse = await http
    .get('./api/configLocale', { validateStatus: () => true })
    .then((r) => (r.status === 200 ? r.data : {}))
    .catch(() => ({}));
  let locale = r.locale ?? 'en';
  document.querySelector('html')?.setAttribute('lang', locale);
  let messages: Record<string, MessageSchema> = {
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