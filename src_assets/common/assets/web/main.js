import { createApp } from 'vue'
import { initApp } from './init'
import { router } from './router'
import Shell from './layout/Shell.vue'
import './styles/tailwind.css'

const app = createApp(Shell)
app.use(router)
initApp(app)
