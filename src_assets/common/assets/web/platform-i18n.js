import { inject } from 'vue';

class PlatformMessageI18n {
  /**
   * @param {string} platform
   */
  constructor(platform) {
    this.platform = platform;
  }

  /**
   * @param {string} key
   * @param {string} platform identifier
   * @return {string} key with platform identifier
   */
  getPlatformKey(key, platform) {
    return key + '_' + platform;
  }

  /**
   * @param {string} key
   * @param {string?} defaultMsg
   * @return {string} translated message or defaultMsg if provided
   */
  getMessageUsingPlatform(key, defaultMsg) {
    const realKey = this.getPlatformKey(key, this.platform);
    const i18n = inject('i18n');
    let message = i18n.t(realKey);

    if (message !== realKey) {
      // We got a message back, return early
      return message;
    }

    // If on Windows, we don't fallback to unix, so return early
    if (this.platform === 'windows') {
      return defaultMsg ? defaultMsg : message;
    }

    // there's no message for key, check for unix version
    const unixKey = this.getPlatformKey(key, 'unix');
    message = i18n.t(unixKey);

    if (message === unixKey && defaultMsg) {
      // there's no message for unix key, return defaultMsg
      return defaultMsg;
    }
    return message;
  }
}

/**
 * @param {string?} platform
 * @return {PlatformMessageI18n} instance
 */
export function usePlatformI18n(platform) {
  // Resolve platform from injected ref if not explicitly passed.
  if (!platform) {
    const injected = inject('platform', null);
    if (injected) {
      // Support either a ref or plain value
      platform = typeof injected === 'object' && 'value' in injected ? injected.value : injected;
    }
  }

  // Fallback defensively instead of throwing to avoid hard render errors
  if (!platform) {
    // Default to "windows" (platform with no unix fallback) to keep behavior predictable.
    platform = 'windows';
  }

  return inject('platformMessage', () => new PlatformMessageI18n(platform), true);
}

/**
 * @param {string} key
 * @param {string?} defaultMsg
 * @return {string} translated message or defaultMsg if provided
 */
export function $tp(key, defaultMsg) {
  try {
    const pm = usePlatformI18n();
    // Guard i18n injection absence (early render before init) inside message getter
    return pm.getMessageUsingPlatform(key, defaultMsg);
  } catch (e) {
    return defaultMsg || key;
  }
}
