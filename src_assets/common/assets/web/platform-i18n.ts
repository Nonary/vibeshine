import { inject, Ref } from 'vue'

interface I18n {
  t(key: string): string;
}

class PlatformMessageI18n {
  platform: string;

  constructor(platform: string) {
    this.platform = platform
  }

  getPlatformKey(key: string, platform: string): string {
    return key + '_' + platform
  }

  getMessageUsingPlatform(key: string, defaultMsg?: string): string {
    const realKey = this.getPlatformKey(key, this.platform)
    const i18n = inject<I18n>('i18n')!
    let message = i18n.t(realKey)

    if (message !== realKey) {
      // We got a message back, return early
      return message
    }
    
    // If on Windows, we don't fallback to unix, so return early
    if (this.platform === 'windows') {
      return defaultMsg ? defaultMsg : message
    }
    
    // there's no message for key, check for unix version
    const unixKey = this.getPlatformKey(key, 'unix')
    message = i18n.t(unixKey)

    if (message === unixKey && defaultMsg) {
      // there's no message for unix key, return defaultMsg
      return defaultMsg
    }
    return message
  }
}

export function usePlatformI18n(platform?: string): PlatformMessageI18n {
  if (!platform) {
    platform = inject<Ref<string>>('platform')!.value
  }

  if (!platform) {
    throw 'platform argument missing'
  }

  return inject(
    'platformMessage',
    () => new PlatformMessageI18n(platform!),
    true
  )!
}

export function $tp(key: string, defaultMsg?: string): string {
  const pm = usePlatformI18n()
  return pm.getMessageUsingPlatform(key, defaultMsg)
}