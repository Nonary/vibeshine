<template>
  <RouterLink
    :to="to"
    class="group flex items-center gap-3 rounded-md mx-2 transition"
    :class="[
      collapsed ? 'justify-center px-0 py-2.5' : 'px-4 py-2.5',
      sub ? (collapsed ? 'text-[0]' : 'pl-8 pr-3 py-2 text-[12px]') : 'text-[13px]',
      baseClasses,
      isActive ? activeClasses : hoverClasses
    ]"
  >
    <i v-if="!sub" :class="['fas text-sm w-4 text-center', 'fa-fw', icon]"></i>
    <span v-if="!collapsed" class="tracking-wide whitespace-nowrap"> <slot /> </span>
  </RouterLink>
</template>
<script setup>
import { useRoute } from 'vue-router'
import { computed } from 'vue'
const props = defineProps({
  to: { type: [String,Object], required: true },
  icon: { type: String, default: 'fa-circle' },
  collapsed: { type: Boolean, default: false },
  sub: { type: Boolean, default: false }
})
const route = useRoute()
const isActive = computed(() => {
  if(typeof props.to === 'string') return route.path === props.to
  if(props.to && typeof props.to === 'object'){
    const pathMatch = route.path === props.to.path
    if(!pathMatch) return false
    if(props.to.query && props.to.query.sec){
      return route.query.sec === props.to.query.sec
    }
    return pathMatch
  }
  return false
})
const baseClasses = 'text-solar-dark/80 dark:text-lunar-light/70'
const hoverClasses = 'hover:text-solar-dark dark:hover:text-lunar-light hover:bg-solar-primary/10 dark:hover:bg-lunar-primary/10'
const activeClasses = 'bg-solar-primary/15 dark:bg-lunar-primary/15 text-solar-dark dark:text-lunar-light font-medium'
</script>
<style scoped></style>
