import { inject, ComputedRef } from 'vue';

interface I18nGlobal {
  t(key: string): string;
}

class PlatformMessageI18n {
    platform: string;

    /**
     * @param {string} platform
     */
    constructor(platform: string) {
        this.platform = platform;
    }

    /**
     * @param {string} key
     * @param {string} platform identifier
     * @return {string} key with platform identifier
     */
    getPlatformKey(key: string, platform: string): string {
        return key + '_' + platform;
    }

    /**
     * @param {string} key
     * @param {string?} defaultMsg
     * @return {string} translated message or defaultMsg if provided
     */
    getMessageUsingPlatform(key: string, defaultMsg?: string): string {
        const realKey = this.getPlatformKey(key, this.platform);
        const i18n = inject<I18nGlobal>('i18n');
        if (!i18n) {
            return defaultMsg || key;
        }
        
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
export function usePlatformI18n(platform?: string): PlatformMessageI18n {
    if (!platform) {
        const platformRef = inject<ComputedRef<string>>('platform');
        if (!platformRef) {
            throw new Error('platform argument missing');
        }
        platform = platformRef.value;
    }

    if (!platform) {
        throw new Error('platform argument missing');
    }

    return inject(
        'platformMessage',
        () => new PlatformMessageI18n(platform),
        true
    ) as PlatformMessageI18n;
}

/**
 * @param {string} key
 * @param {string?} defaultMsg
 * @return {string} translated message or defaultMsg if provided
 */
export function $tp(key: string, defaultMsg?: string): string {
    const pm = usePlatformI18n();
    return pm.getMessageUsingPlatform(key, defaultMsg);
}