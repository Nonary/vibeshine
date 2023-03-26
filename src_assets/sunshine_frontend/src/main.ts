import { createApp } from 'vue'
import { createPinia } from 'pinia'

import App from './App.vue'
import router from './router'

import './assets/main.css'
import 'bootstrap/dist/css/bootstrap.min.css'
import 'bootstrap/dist/js/bootstrap.bundle.min.js'

import { library } from '@fortawesome/fontawesome-svg-core'
import { faCoffee, faHome, faUnlock, faStream, faCog, faUserShield, faInfo } from '@fortawesome/free-solid-svg-icons'
library.add(faCoffee, faHome, faUnlock, faStream, faCog, faUserShield, faInfo);



const app = createApp(App)

app.use(createPinia())
app.use(router)

app.mount('#app')
