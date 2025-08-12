<template>
	<div v-if="open" class="fixed inset-0 z-50 flex items-start justify-center p-6 overflow-y-auto">
		<div class="fixed inset-0 bg-black/50 backdrop-blur-sm" @click="close"></div>
		<div class="relative w-full max-w-3xl mx-auto bg-white dark:bg-lunar-surface rounded-xl shadow-xl flex flex-col border border-black/10 dark:border-white/10">
			<div class="flex items-center justify-between px-6 py-4 border-b border-black/5 dark:border-white/10">
				<h2 class="font-semibold text-sm">{{ form.index === -1 ? 'Add Application' : 'Edit Application' }}</h2>
				<button @click="close" class="text-black/50 dark:text-white/50 hover:text-red-600"><i class="fas fa-times"></i></button>
			</div>
			<div class="p-6 space-y-6 text-xs overflow-y-auto max-h-[70vh]">
				<div class="grid gap-4 sm:grid-cols-2">
					<div class="space-y-1 col-span-2 sm:col-span-1">
						<label class="font-medium">Name</label>
						<input v-model="form.name" class="app-input" placeholder="Game or App Name" />
					</div>
						<div class="space-y-1 col-span-2 sm:col-span-1">
							<label class="font-medium">Image Path</label>
							<input v-model="form['image-path']" class="app-input font-mono" placeholder="/path/to/image.png" />
						</div>
					<div class="space-y-1 col-span-2">
						<label class="font-medium">Command (array or single)</label>
						<textarea v-model="cmdText" class="app-input font-mono h-20" placeholder="Executable or JSON array"></textarea>
						<p class="text-black/60 dark:text-white/50">Single command line or JSON array of segments.</p>
					</div>
					<div class="space-y-1 col-span-2">
						<label class="font-medium">Working Directory</label>
						<input v-model="form['working-dir']" class="app-input font-mono" placeholder="C:/Games/App" />
					</div>
					<div class="space-y-1 col-span-2 grid grid-cols-2 gap-4">
						<label class="inline-flex items-center gap-2 text-[11px]"><input type="checkbox" v-model="form['exclude-global-prep-cmd']" class="app-checkbox"> Exclude Global Prep</label>
						<label class="inline-flex items-center gap-2 text-[11px]"><input type="checkbox" v-model="form['auto-detach']" class="app-checkbox"> Auto Detach</label>
						<label class="inline-flex items-center gap-2 text-[11px]"><input type="checkbox" v-model="form['wait-all']" class="app-checkbox"> Wait All</label>
						<label v-if="platform==='windows'" class="inline-flex items-center gap-2 text-[11px]"><input type="checkbox" v-model="form.elevated" class="app-checkbox"> Elevated</label>
					</div>
					<div class="space-y-1 col-span-2 sm:col-span-1">
						<label class="font-medium">Exit Timeout (s)</label>
						<input type="number" min="0" v-model.number="form['exit-timeout']" class="app-input w-32" />
					</div>
				</div>
				<div>
					<div class="flex items-center justify-between mb-2">
						<h3 class="font-semibold text-xs uppercase tracking-wider">Prep Commands</h3>
						<button class="mini-btn" @click="addPrep"><i class="fas fa-plus"></i> Add</button>
					</div>
					<div v-if="form['prep-cmd'].length===0" class="text-[11px] text-black/60 dark:text-white/40">None</div>
					<div v-else class="space-y-3">
						<div v-for="(p,i) in form['prep-cmd']" :key="i" class="grid gap-2 md:grid-cols-5 items-start">
							<input v-model="p.do" placeholder="do" class="app-input font-mono md:col-span-2" />
							<input v-model="p.undo" placeholder="undo" class="app-input font-mono md:col-span-2" />
							<div class="flex items-center gap-2 md:col-span-1">
								<label v-if="platform==='windows'" class="inline-flex items-center gap-1 text-[11px]"><input type="checkbox" v-model="p.elevated" class="app-checkbox"> Elev</label>
								<button class="mini-btn danger" @click="form['prep-cmd'].splice(i,1)"><i class="fas fa-trash"></i></button>
							</div>
						</div>
					</div>
				</div>
				<div>
					<div class="flex items-center justify-between mb-2">
						<h3 class="font-semibold text-xs uppercase tracking-wider">Detached Commands</h3>
						<button class="mini-btn" @click="form.detached.push('')"><i class="fas fa-plus"></i> Add</button>
					</div>
					<div v-if="form.detached.length===0" class="text-[11px] text-black/60 dark:text-white/40">None</div>
					<div v-else class="space-y-2">
						<div v-for="(d,i) in form.detached" :key="i" class="flex gap-2 items-start">
							<input v-model="form.detached[i]" class="app-input font-mono flex-1" />
							<button class="mini-btn danger" @click="form.detached.splice(i,1)"><i class="fas fa-times"></i></button>
						</div>
					</div>
				</div>
			</div>
			<div class="flex items-center justify-between px-6 py-4 border-t border-black/5 dark:border-white/10 gap-3">
				<div class="flex gap-2">
					<button class="main-btn" @click="save" :disabled="saving.v"><i class="fas fa-save"></i> <span class="hidden sm:inline">Save</span></button>
					<button class="ghost-btn" @click="close">Cancel</button>
				</div>
				<button v-if="form.index!==-1" class="danger-btn" @click="del" :disabled="saving.v"><i class="fas fa-trash"></i></button>
			</div>
		</div>
	</div>
</template>
<script setup>
import { reactive, watch, computed } from 'vue'
const props = defineProps({ modelValue:Boolean, platform:String, app:Object, index:{type:Number,default:-1} })
const emit = defineEmits(['update:modelValue','saved','deleted'])
const open = computed(()=>props.modelValue)
function fresh(){ return { name:'', output:'', cmd:[], index:-1, 'exclude-global-prep-cmd':false, elevated:false, 'auto-detach':true, 'wait-all':true, 'exit-timeout':5, 'prep-cmd':[], detached:[], 'image-path':'', 'working-dir':'' } }
const form = reactive(fresh())
const cmdText = computed({ get(){ return Array.isArray(form.cmd)? JSON.stringify(form.cmd): (form.cmd||'') }, set(v){ v=v.trim(); if(v.startsWith('[')){ try{ form.cmd = JSON.parse(v) }catch{ form.cmd=v } } else form.cmd=v } })
watch(()=>props.app,(val)=>{ if(!open.value) return; Object.assign(form,fresh(), JSON.parse(JSON.stringify(val||{}))); form.index = props.index??-1 },{ immediate:true })
watch(open,o=>{ if(o){ Object.assign(form,fresh(), JSON.parse(JSON.stringify(props.app||{}))); form.index = props.index??-1 } })
function close(){ emit('update:modelValue', false) }
function addPrep(){ form['prep-cmd'].push({ do:'', undo:'', ...(props.platform==='windows'? { elevated:false }: {}) }) }
const saving = reactive({ v:false })
async function save(){ saving.v=true; const payload = JSON.parse(JSON.stringify(form)); try{ await fetch('./api/apps',{ method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(payload) }); emit('saved'); close() } finally { saving.v=false } }
async function del(){ if(!confirm('Delete this application?')) return; saving.v=true; try { await fetch(`./api/apps/${form.index}`,{ method:'DELETE' }); emit('deleted'); close() } finally { saving.v=false } }
</script>
<style scoped>
.app-input{ width:100%; border:1px solid rgba(0,0,0,.12); background:rgba(255,255,255,.7); padding:4px 8px; border-radius:6px; font-size:11px; line-height:1.2; }
.dark .app-input{ background:rgba(10,16,40,.6); border-color:rgba(255,255,255,.15); color:#F5F9FF }
.app-input:focus{ outline:2px solid rgba(253,184,19,.5); }
.dark .app-input:focus{ outline:2px solid rgba(77,163,255,.5); }
.app-checkbox{ width:14px; height:14px; }
.main-btn, .ghost-btn, .danger-btn, .mini-btn{ font-size:11px; font-weight:500; line-height:1; border-radius:6px; display:inline-flex; align-items:center; gap:6px; padding:6px 10px; cursor:pointer; }
.main-btn{ background:rgba(253,184,19,0.9); color:#212121; }
.main-btn:hover{ background:#FDB813; }
.dark .main-btn{ background:rgba(77,163,255,0.85); color:#050B1E; }
.dark .main-btn:hover{ background:#4DA3FF; }
.ghost-btn{ background:transparent; color:#0D3B66; }
.ghost-btn:hover{ background:rgba(13,59,102,0.1); }
.dark .ghost-btn{ color:#F5F9FF; }
.dark .ghost-btn:hover{ background:rgba(255,255,255,0.1); }
.danger-btn{ background:#D32F2F; color:#fff; padding:6px 10px; }
.danger-btn:hover{ background:#b71c1c; }
.mini-btn{ background:rgba(0,0,0,.65); color:#fff; padding:4px 8px; }
.mini-btn:hover{ background:rgba(0,0,0,.85); }
.mini-btn.danger{ background:#D32F2F; }
.mini-btn.danger:hover{ background:#b71c1c; }
</style>
