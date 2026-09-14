import { initCloud } from './utils/cloud'
import { store } from './utils/store'

App<IAppOption>({
  globalData: {
    cloudReady: false,
  },
  onLaunch() {
    initCloud()
    store.load()
  },
})