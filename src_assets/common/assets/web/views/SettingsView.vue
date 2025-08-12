<template>
		<main class="flex-1 px-0 md:px-2 xl:px-6 py-2 md:py-6 space-y-6 overflow-x-hidden" ref="mainEl">
			<header class="sticky top-0 z-20 -mx-0 md:-mx-2 xl:-mx-6 px-0 md:px-2 xl:px-6 py-3 bg-solar-light/70 dark:bg-lunar-dark/60 backdrop-blur supports-[backdrop-filter]:bg-solar-light/50 supports-[backdrop-filter]:dark:bg-lunar-dark/40 border-b border-solar-dark/10 dark:border-lunar-light/10">
				<div class="flex items-center justify-between gap-4 flex-wrap">
					<div class="min-w-0">
						<h2 class="text-sm font-semibold uppercase tracking-wider">Settings</h2>
						<p class="text-[11px] opacity-60">Configuration auto-saves; restart to apply runtime changes.</p>
					</div>
					<!-- In-page search -->
					<div class="relative flex-1 max-w-2xl min-w-[260px]">
					<input
						v-model="searchQuery"
						type="text"
						placeholder="Search settings... (Enter to jump)"
						class="w-full px-3 py-2 text-sm rounded-md bg-black/5 dark:bg-white/10 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40"
						@focus="searchOpen = searchQuery.length>0"
						@blur="() => setTimeout(()=> searchOpen=false, 120)"
						@keydown.enter.prevent="jumpFirstResult"
					/>
					<i class="fas fa-magnifying-glass absolute right-3 top-1/2 -translate-y-1/2 text-[12px] opacity-60"></i>
					<transition name="fade">
						<div v-if="searchOpen" class="absolute mt-2 w-full z-20 bg-white/95 dark:bg-lunar-surface/95 backdrop-blur rounded-md shadow-lg border border-black/5 dark:border-white/10 max-h-80 overflow-auto">
							<div v-if="searchResults.length===0" class="px-3 py-2 text-[12px] opacity-60">No results</div>
							<button v-for="(r,idx) in searchResults" :key="idx" class="w-full text-left px-3 py-2 hover:bg-black/5 dark:hover:bg-white/10 text-[13px] flex items-start gap-2" @click="goTo(r)">
								<span class="shrink-0 mt-0.5"><i class="fas fa-compass text-solar-primary dark:text-lunar-primary text-[11px]"></i></span>
								<span class="min-w-0">
									<span class="block font-medium truncate">{{ r.label }}</span>
									<span class="block text-[11px] opacity-60 truncate">{{ r.path }}</span>
								</span>
							</button>
						</div>
					</transition>
					</div>
					<div v-if="!autoSave" class="flex gap-2">
						<button class="btn" @click="save" :disabled="saving">Save</button>
						<button class="btn ghost" v-if="saved && !restarted" @click="apply">Apply</button>
					</div>
					<div v-else class="text-[11px] font-medium min-h-[1rem] flex items-center gap-2">
						<transition name="fade"><span v-if="saving">Saving…</span></transition>
						<transition name="fade"><span v-if="saved && !saving" class="text-green-600 dark:text-green-400">Saved</span></transition>
					</div>
				</div>
			</header>

			<div v-if="config" class="space-y-4">
				<section v-for="tab in tabs" :id="tab.id" :key="tab.id" :ref="el => sectionRefs.set(tab.id, el)" class="scroll-mt-24">
					<button type="button" class="w-full flex items-center justify-between px-3 py-2 rounded-lg bg-black/5 dark:bg-white/10 hover:bg-black/10 dark:hover:bg-white/15 transition text-left"
						@click="toggle(tab.id)">
						<span class="font-semibold">{{ tab.name }}</span>
						<i :class="['fas text-xs transition-transform', isOpen(tab.id)?'fa-chevron-up':'fa-chevron-down']"></i>
					</button>
					<transition name="fade">
						<div v-show="isOpen(tab.id)" class="mt-2 bg-white/80 dark:bg-lunar-surface/70 backdrop-blur-sm border border-black/5 dark:border-white/10 rounded-xl shadow-sm p-6 space-y-6">
							<component :is="tab.component" :config="config" :platform="platform" />
						</div>
					</transition>
				</section>
			</div>
			<div v-else class="text-xs opacity-60">Loading...</div>

			<div class="text-[11px]">
				<transition name="fade"><div v-if="saved && !restarted && !autoSave" class="text-green-600 dark:text-green-400">Saved. Click Apply to restart.</div></transition>
				<transition name="fade"><div v-if="restarted" class="text-green-600 dark:text-green-400">Restart triggered.</div></transition>
			</div>
		</main>

		<!-- Unsaved sticky bar (manual mode) -->
		<transition name="slide-fade">
			<div v-if="dirty && autoSave===false" class="fixed bottom-4 right-6 z-30">
				<div class="bg-white/90 dark:bg-lunar-surface/90 backdrop-blur rounded-lg shadow border border-black/5 dark:border-white/10 px-4 py-2 flex items-center gap-3">
					<span class="text-[11px] font-medium">Unsaved changes</span>
					<button class="btn" @click="save" :disabled="saving">Save</button>
				</div>
			</div>
		</transition>
  
</template>
<script setup>
import { ref, reactive, onMounted, watch, nextTick } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import General from '../configs/tabs/General.vue'
import Inputs from '../configs/tabs/Inputs.vue'
import Network from '../configs/tabs/Network.vue'
import Files from '../configs/tabs/Files.vue'
import Advanced from '../configs/tabs/Advanced.vue'
import AudioVideo from '../configs/tabs/AudioVideo.vue'
import ContainerEncoders from '../configs/tabs/ContainerEncoders.vue'

// Core reactive state
const platform = ref('')
const config = ref(null)
const saved = ref(false)
const restarted = ref(false)
const saving = ref(false)
const dirty = ref(false)
const autoSave = ref(true) // toggle if desired
// In-page search state/index
const searchQuery = ref('')
const searchOpen = ref(false)
const searchResults = ref([])
const searchIndex = ref([]) // { sectionId, label, path, el }
const mainEl = ref(null)

// Sections definition (reusing existing components)
const tabs = reactive([
	{ id:'general', name:'General', component: General },
	{ id:'input', name:'Input', component: Inputs },
	{ id:'av', name:'Audio / Video', component: AudioVideo },
	{ id:'encoders', name:'Encoders', component: ContainerEncoders },
	{ id:'network', name:'Network', component: Network },
	{ id:'files', name:'Files', component: Files },
	{ id:'advanced', name:'Advanced', component: Advanced }
])

// Collapsible sections state: only General open initially
const openSections = ref(new Set(['general']))
function isOpen(id){ return openSections.value.has(id) }
function toggle(id){
	const set = new Set(openSections.value)
	if(set.has(id)) set.delete(id); else set.add(id)
	openSections.value = set
}

function pruneDefaults(c){ return JSON.parse(JSON.stringify(c)) }
async function load(){
	const r = await fetch('./api/config').then(r=>r.json())
	platform.value = r.platform
	delete r.platform; delete r.status; delete r.version
	const specials = ['dd_mode_remapping','global_prep_cmd']
	specials.forEach(k=>{ if(r[k]) try{ r[k]=JSON.parse(r[k]) }catch{} })
	config.value = r
	// Build index after DOM updates
	await nextTick()
	requestAnimationFrame(buildSearchIndex)
}
onMounted(load)

// Auto-save watcher
let saveTimer
watch(config, () => {
	if(!config.value) return
	dirty.value = true
	if(autoSave.value){
		clearTimeout(saveTimer)
		saveTimer = setTimeout(()=>{ save() }, 800)
	}
},{ deep:true })

async function save(){
	if(!config.value) return
	saving.value = true
	saved.value = false
	restarted.value = false
	const body = pruneDefaults(config.value)
	const res = await fetch('./api/config',{ method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body) })
	if(res.ok){ saved.value = true; dirty.value = false }
	saving.value = false
}
async function apply(){
	await save()
	if(!saved.value) return
	restarted.value = true
	fetch('./api/restart',{ method:'POST', headers:{'Content-Type':'application/json'} })
	setTimeout(()=>{ restarted.value=false; saved.value=false },5000)
}

// Routing integration: expand and scroll to section when query changes
const route = useRoute()
const router = useRouter()
const sectionRefs = new Map()
onMounted(()=>{
	if(route.query.sec){ scrollToOpen(route.query.sec) }
})
watch(()=> route.query.sec, (val)=>{ if(typeof val === 'string') scrollToOpen(val) })
function scrollToOpen(id){
	if(!id) return
	// ensure open
	if(!openSections.value.has(id)) toggle(id)
	const el = sectionRefs.get(id)
	if(el){ el.scrollIntoView({ behavior:'smooth', block:'start' }) }
}
function goSection(id){
	if(route.path === '/settings') router.replace({ path:'/settings', query:{ sec:id } })
	else router.push({ path:'/settings', query:{ sec:id } })
}

// Build a searchable index of labels/controls
function buildSearchIndex(){
	const root = mainEl.value
	if(!root) return
	const items = []
	const sections = Array.from(root.querySelectorAll('section[id]'))
	sections.forEach(sec => {
		const sectionId = sec.getAttribute('id')
		const sectionTitle = sec.querySelector('h3')?.textContent?.trim() || sectionId
		const labels = Array.from(sec.querySelectorAll('label'))
		labels.forEach(lbl => {
			const text = (lbl.textContent || '').trim()
			if(!text) return
			const forId = lbl.getAttribute('for')
			let target = null
			if(forId){ try{ target = sec.querySelector('#'+CSS.escape(forId)) }catch{} }
			if(!target){ target = lbl.closest('div')?.querySelector('input,select,textarea,.form-control') }
			if(target){ items.push({ sectionId, label:text, path: `${sectionTitle} › ${text}`, el: target }) }
		})
	})
	searchIndex.value = items
}

watch(searchQuery, (q)=>{
	const v = (q||'').trim().toLowerCase()
	searchOpen.value = v.length>0
	if(!v){ searchResults.value = []; return }
	const max = 15
	searchResults.value = searchIndex.value.filter(it =>
		it.label.toLowerCase().includes(v) || it.path.toLowerCase().includes(v)
	).slice(0,max)
})

function jumpFirstResult(){ if(searchResults.value.length) goTo(searchResults.value[0]) }
function goTo(item){
	searchOpen.value = false
	if(item?.sectionId){ router.replace({ path:'/settings', query:{ sec:item.sectionId } }) }
	setTimeout(()=>{
		try{ item.el?.scrollIntoView({ behavior:'smooth', block:'center' }); flash(item.el) }catch{}
	}, 250)
}
function flash(el){
	if(!el) return
	el.classList.add('flash-highlight')
	setTimeout(()=> el.classList.remove('flash-highlight'), 1200)
}
</script>
<style scoped>
/* Buttons reused */
.btn{ display:inline-flex; align-items:center; gap:0.5rem; border-radius:0.375rem; background:rgba(253,184,19,0.9); color:#212121; padding:0.375rem 0.75rem; font-size:0.75rem; font-weight:500; line-height:1; transition:background .15s; }
.dark .btn{ background:rgba(77,163,255,0.8); color:#F5F9FF; }
.btn:hover{ background:#FDB813; }
.dark .btn:hover{ background:#4DA3FF; }
.btn.ghost{ background:transparent; color:#0D3B66; }
.dark .btn.ghost{ color:#F5F9FF; }
.btn.ghost:hover{ background:rgba(13,59,102,0.1); }
.dark .btn.ghost:hover{ background:rgba(245,249,255,0.1); }
.fade-enter-active,.fade-leave-active{ transition: opacity .25s }
.fade-enter-from,.fade-leave-to{ opacity:0 }
.slide-fade-enter-active,.slide-fade-leave-active{ transition: all .25s }
.slide-fade-enter-from,.slide-fade-leave-to{ opacity:0; transform: translateY(6px) }
.flash-highlight{ box-shadow: 0 0 0 3px rgba(253,184,19,0.35); outline: 2px solid rgba(253,184,19,0.35); border-radius: 6px; transition: box-shadow .2s }
/* Utilities */
.no-scrollbar::-webkit-scrollbar{ display:none }
.no-scrollbar{ -ms-overflow-style:none; scrollbar-width:none }
</style>
