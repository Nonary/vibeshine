<template>
  <div class="card p-3">
    <div class="card-body text-center">
      <h2 class="text-lg font-semibold">{{ $t('resource_card.resources') }}</h2>
      <div class="my-3 text-sm">{{ $t('resource_card.resources_desc') }}</div>
      <div class="flex justify-center gap-3 p-4">
        <a class="btn btn-success px-4 py-2" href="https://app.lizardbyte.dev" target="_blank">
          {{ $t('resource_card.lizardbyte_website') }}</a
        >
        <a
          class="btn btn-primary px-4 py-2"
          href="https://app.lizardbyte.dev/discord"
          target="_blank"
        >
          <i class="fab fa-fw fa-discord" /> Discord</a
        >
        <a
          class="btn btn-secondary px-4 py-2"
          href="https://github.com/orgs/LizardByte/discussions"
          target="_blank"
        >
          <i class="fab fa-fw fa-github" /> {{ $t('resource_card.github_discussions') }}</a
        >
      </div>
    </div>
  </div>
  <!-- Legal -->
  <div class="card p-3 mt-4">
    <div class="card-body text-center">
      <h2 class="text-lg font-semibold">{{ $t('resource_card.legal') }}</h2>
      <div class="my-3 text-sm">{{ $t('resource_card.legal_desc') }}</div>
      <div class="flex justify-center gap-3 p-4">
        <a
          class="btn btn-danger px-4 py-2"
          href="https://github.com/LizardByte/Sunshine/blob/master/LICENSE"
          target="_blank"
        >
          <i class="fas fa-fw fa-file-alt" /> {{ $t('resource_card.license') }}</a
        >
        <a
          class="btn btn-danger px-4 py-2"
          href="https://github.com/LizardByte/Sunshine/blob/master/NOTICE"
          target="_blank"
        >
          <i class="fas fa-fw fa-exclamation" /> {{ $t('resource_card.third_party_notice') }}</a
        >
      </div>
    </div>
  </div>
</template>
