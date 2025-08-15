import { createI18n, I18n } from "vue-i18n";

// Import only the fallback language files
import en from './public/assets/locale/en.json'

interface ConfigLocaleResponse {
  locale?: string;
}

interface Messages {
  [key: string]: any;
}

export default async function(): Promise<I18n> {
  const response = await fetch("./api/configLocale");
  const r: ConfigLocaleResponse = await response.json();
  const locale = r.locale ?? "en";
  document.querySelector('html')!.setAttribute('lang', locale);
  const messages: Messages = {
    en
  };
  try {
    if (locale !== 'en') {
      const localeResponse = await fetch(`./assets/locale/${locale}.json`);
      const localeData = await localeResponse.json();
      messages[locale] = localeData;
    }
  } catch (e) {
    console.error("Failed to download translations", e);
  }
  const i18n = createI18n({
    locale: locale, // set locale
    fallbackLocale: 'en', // set fallback locale
    messages: messages
  });
  return i18n;
}