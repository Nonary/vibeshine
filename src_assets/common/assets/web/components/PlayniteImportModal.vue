<template>
  <div
    v-if="open"
    class="fixed inset-0 z-50 flex items-start justify-center p-6 overflow-y-auto"
  >
    <div
      class="fixed inset-0 bg-black/50 backdrop-blur-sm"
      @click="close"
    />
    <div class="relative w-full max-w-2xl mx-auto bg-white dark:bg-lunar-surface rounded-xl shadow-xl flex flex-col border border-black/10 dark:border-white/10">
      <div class="flex items-center justify-between px-6 py-4 border-b border-black/5 dark:border-white/10">
        <h2 class="font-semibold text-sm">
          Import from Playnite
        </h2>
        <button
          class="text-black/50 dark:text-white/50 hover:text-red-600"
          @click="close"
        >
          <i class="fas fa-times" />
        </button>
      </div>
      <div class="p-6 space-y-4 text-xs">
        <!-- Playnite configuration (collapsible) -->
        <div>
          <div class="flex items-center justify-between">
            <h3 class="font-semibold text-sm">
              Playnite
            </h3>
            <button
              class="mini-btn"
              @click="playniteOpen = !playniteOpen"
            >
              <i :class="playniteOpen ? 'fas fa-chevron-up' : 'fas fa-chevron-down'" />
            </button>
          </div>
          <div
            v-show="playniteOpen"
            class="mt-3 space-y-3"
          >
            <div class="grid gap-2 sm:grid-cols-3 items-center">
              <div class="col-span-2">
                <label class="font-medium">Playnite Install Path</label>
                <input
                  v-model="playnitePath"
                  class="app-input font-mono"
                  placeholder="C:\\Program Files\\Playnite"
                >
              </div>
              <div class="flex items-center gap-2">
                <div class="flex items-center gap-2">
                  <span
                    :class="['w-3 h-3 rounded-full', pluginInstalled ? 'bg-green-500' : 'bg-red-500']"
                    aria-hidden="true"
                  />
                  <span class="text-[11px] text-black/60 dark:text-white/40">{{ pluginInstalled ? 'Sunshine plugin installed' : 'Plugin not detected' }}</span>
                </div>
              </div>
            </div>
            <div class="flex items-center gap-2">
              <button
                class="main-btn"
                :disabled="installing"
                @click="installPlugin"
              >
                {{ pluginInstalled ? 'Reinstall Plugin' : 'Install Plugin' }}
              </button>
              <button
                class="ghost-btn"
                @click="detectPlugin"
              >
                Detect
              </button>
              <span
                v-if="installing"
                class="text-[11px] text-black/60 dark:text-white/40"
              >Installing...</span>
            </div>
          </div>
        </div>

        <!-- Manual add section -->
        <div>
          <div class="flex items-center justify-between">
            <h3 class="font-semibold text-sm">
              Add to Sunshine (Manual)
            </h3>
            <div class="flex items-center gap-2">
              <label class="inline-flex items-center gap-2 text-[12px]"><input
                v-model="autoSync"
                type="checkbox"
                class="app-checkbox"
              > Auto-sync categories</label>
            </div>
          </div>
          <div class="mt-2">
            <label class="font-medium">Search Playnite Games</label>
            <div class="flex gap-2 mt-1">
              <input
                v-model="manualQuery"
                placeholder="Search games to add..."
                class="app-input flex-1"
                @input="onManualSearch"
              >
              <div class="relative">
                <button
                  class="mini-btn"
                  @click="onManualSearch"
                >
                  <i class="fas fa-search" />
                </button>
                <div
                  v-if="manualResults.length"
                  class="absolute right-0 mt-2 w-72 bg-white dark:bg-lunar-surface border rounded shadow max-h-48 overflow-y-auto z-50"
                >
                  <ul>
                    <li
                      v-for="(m,mi) in manualResults"
                      :key="mi"
                      class="p-2 hover:bg-black/5 dark:hover:bg-white/5 cursor-pointer flex items-center gap-2"
                      @click="selectManual(m)"
                    >
                      <div class="w-8 h-10 bg-black/5 dark:bg-white/5 rounded overflow-hidden flex items-center justify-center">
                        <img
                          v-if="m.cover"
                          :src="m.cover"
                          class="max-w-full max-h-full object-contain"
                        >
                        <div
                          v-else
                          class="text-lg font-bold text-solar-primary/40 dark:text-lunar-primary/40"
                        >
                          {{ m.name?.substring(0,1) }}
                        </div>
                      </div>
                      <div class="flex-1 text-sm">
                        {{ m.name }}
                      </div>
                    </li>
                  </ul>
                </div>
              </div>
            </div>
            <div
              v-if="selectedManual"
              class="mt-3 border p-3 rounded"
            >
              <div class="flex items-start gap-3">
                <div class="w-14 h-20 bg-black/5 dark:bg-white/5 rounded overflow-hidden flex items-center justify-center">
                  <img
                    v-if="selectedManual.cover"
                    :src="selectedManual.cover"
                    class="max-w-full max-h-full object-contain"
                  >
                  <div
                    v-else
                    class="text-3xl font-bold text-solar-primary/40 dark:text-lunar-primary/40"
                  >
                    {{ selectedManual.name?.substring(0,1) }}
                  </div>
                </div>
                <div class="flex-1">
                  <div class="font-medium">
                    {{ selectedManual.name }}
                  </div>
                  <div class="text-[11px] text-black/60 dark:text-white/40">
                    Platform: {{ selectedManual.platform }}
                  </div>
                  <div class="mt-2 flex flex-wrap gap-2">
                    <span
                      v-for="c in selectedManual.categories"
                      :key="c"
                      class="px-2 py-0.5 rounded-full text-[11px] bg-black/5 dark:bg-white/5"
                    >{{ c }}</span>
                  </div>
                  <div class="mt-3 flex gap-2">
                    <button
                      class="main-btn"
                      @click="addToSunshine(selectedManual)"
                    >
                      <i class="fas fa-plus" /> Add to Sunshine
                    </button>
                    <button
                      class="ghost-btn"
                      @click="selectedManual = null"
                    >
                      Clear
                    </button>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>
        <div>
          <label class="font-medium">Search Playnite Games</label>
          <div class="flex gap-2 mt-1">
            <input
              v-model="query"
              placeholder="Search games by name..."
              class="app-input flex-1"
              @input="onSearch"
            >
            <button
              class="mini-btn"
              @click="onSearch"
            >
              <i class="fas fa-search" />
            </button>
          </div>
          <div class="mt-3">
            <div
              v-if="results.length===0"
              class="text-[11px] text-black/60 dark:text-white/40"
            >
              No results (placeholder). Connect to Playnite to enable live search.
            </div>
            <div class="mt-2 flex flex-wrap gap-2">
              <span
                v-for="c in allCategories"
                :key="c"
                :class="['px-2 py-0.5 rounded-full text-[11px] cursor-pointer', selectedCategory===c ? 'bg-solar-primary text-white' : 'bg-black/5 dark:bg-white/5 text-black/70 dark:text-white/60']"
                @click="toggleCategory(c)"
              >{{ c }}</span>
            </div>
            <ul class="space-y-2 mt-2">
              <li
                v-for="(r,i) in results"
                :key="i"
                class="flex items-center justify-between border p-2 rounded"
              >
                <div class="flex items-center gap-3">
                  <div class="w-10 h-12 bg-black/5 dark:bg-white/5 rounded overflow-hidden flex items-center justify-center">
                    <img
                      v-if="r.cover"
                      :src="r.cover"
                      class="max-w-full max-h-full object-contain"
                    >
                    <div
                      v-else
                      class="text-lg font-bold text-solar-primary/40 dark:text-lunar-primary/40"
                    >
                      {{ r.name?.substring(0,1) }}
                    </div>
                  </div>
                  <div>
                    <div class="font-medium text-sm">
                      {{ r.name }}
                    </div>
                    <div class="text-[11px] text-black/60 dark:text-white/40">
                      {{ r.platform || 'Platform' }}
                    </div>
                  </div>
                </div>
                <div>
                  <button
                    class="main-btn"
                    @click="importGame(r)"
                  >
                    <i class="fas fa-plus" /> Import
                  </button>
                </div>
              </li>
            </ul>
          </div>
        </div>
      </div>
      <div class="flex items-center justify-between px-6 py-3 border-t border-black/5 dark:border-white/10">
        <button
          class="ghost-btn"
          @click="close"
        >
          Close
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
const props = defineProps({ modelValue:Boolean })
const emit = defineEmits(['update:modelValue','imported'])
const open = computed(()=>props.modelValue)
const autoSync = ref(false)
const query = ref('')
const results = ref([])

// Playnite section state
const playniteOpen = ref(true)
const playnitePath = ref('')
const pluginInstalled = ref(false)
const installing = ref(false)

function detectPlugin(){
	// simulate detection: installed if path contains 'Playnite'
	pluginInstalled.value = !!(playnitePath.value && /playnite/i.test(playnitePath.value))
}
async function installPlugin(){
	installing.value = true
	pluginInstalled.value = false
	// simulate async install
	await new Promise(r=>setTimeout(r,1000))
	pluginInstalled.value = true
	installing.value = false
}

// Manual add state
const manualQuery = ref('')
const manualResults = ref([])
const selectedManual = ref(null)

function onManualSearch(){
	const q = manualQuery.value.trim().toLowerCase()
	if(!q){ manualResults.value = [] ; return }
	manualResults.value = sampleGames.filter(g => g.name.toLowerCase().includes(q) || (g.categories||[]).some(c=>c.toLowerCase().includes(q))).slice(0,6)
}
function selectManual(item){ selectedManual.value = item; manualResults.value = [] ; manualQuery.value = item.name }

function addToSunshine(item){
	// placeholder: emit as imported and include autoSync/categories info
	emit('imported', { game: item, autoSync: autoSync.value, from:'manual' })
	// mark selection cleared
	selectedManual.value = null
	manualQuery.value = ''
}

const sampleGames = [
	{ name: 'Hollow Knight', platform: 'PC', cover: './assets/hollow_knight.jpg', categories:['Metroidvania','Indie'] },
	{ name: 'The Witcher 3: Wild Hunt', platform: 'PC', cover: './assets/witcher3.jpg', categories:['RPG','Open World'] },
	{ name: 'Celeste', platform: 'PC', cover: './assets/celeste.jpg', categories:['Platformer','Indie'] },
	{ name: 'Stardew Valley', platform: 'PC', cover: './assets/stardew_valley.jpg', categories:['Simulation','Indie'] },
	{ name: 'Doom Eternal', platform: 'PC', cover: './assets/doom_eternal.jpg', categories:['FPS','Action'] },
	{ name: 'Forza Horizon 4', platform: 'PC', cover: './assets/forza_horizon_4.jpg', categories:['Racing','Open World'] },
	{ name: 'Ori and the Blind Forest', platform: 'PC', cover: './assets/ori_blind_forest.jpg', categories:['Platformer','Adventure'] },
	{ name: 'Sekiro: Shadows Die Twice', platform: 'PC', cover: './assets/sekiro.jpg', categories:['Action','Souls-like'] },
	{ name: 'Hades', platform: 'PC', cover: './assets/hades.jpg', categories:['Rogue-like','Indie'] },
	{ name: 'Minecraft', platform: 'PC', cover: './assets/minecraft.jpg', categories:['Sandbox','Survival'] }
]

const selectedCategory = ref('')
const allCategories = computed(()=>{
	const s = new Set()
	for(const g of sampleGames){ (g.categories||[]).forEach(c=>s.add(c)) }
	return Array.from(s).sort()
})

function close(){ emit('update:modelValue', false) }
function onSearch(){
	const q = query.value.trim().toLowerCase()
	const cat = selectedCategory.value
	let filtered = sampleGames
	if(q){ filtered = filtered.filter(g => g.name.toLowerCase().includes(q) || (g.categories||[]).some(c=>c.toLowerCase().includes(q))) }
	if(cat){ filtered = filtered.filter(g => (g.categories||[]).includes(cat)) }
	results.value = filtered.slice(0,10)
}

function toggleCategory(cat){
	selectedCategory.value = (selectedCategory.value===cat) ? '' : cat
	onSearch()
}
function importGame(game){
    // Placeholder emit - in real integration we'd call backend to import
    emit('imported', { game, autoSync: autoSync.value })
}
</script>

<style scoped>
.app-input{ width:100%; border:1px solid rgba(0,0,0,.12); background:rgba(255,255,255,.7); padding:4px 8px; border-radius:6px; font-size:11px; line-height:1.2; }
.dark .app-input{ background:rgba(10,16,40,.6); border-color:rgba(255,255,255,.15); color:#F5F9FF }
.main-btn, .ghost-btn, .mini-btn{ font-size:11px; font-weight:500; line-height:1; border-radius:6px; display:inline-flex; align-items:center; gap:6px; padding:6px 10px; cursor:pointer; }
.main-btn{ background:rgba(253,184,19,0.9); color:#212121; }
.ghost-btn{ background:transparent; color:#0D3B66; }
.mini-btn{ background:rgba(0,0,0,.65); color:#fff; }
</style>
