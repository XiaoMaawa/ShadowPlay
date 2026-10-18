import { createRequire } from 'node:module'
import { fileURLToPath, URL } from 'node:url'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import vueJsx from '@vitejs/plugin-vue-jsx'
import vueDevTools from 'vite-plugin-vue-devtools'

// 临时根目录的唯一来源是 app/temp-const.cjs（%TEMP%/shadow-play/app，
// 或环境变量 SHADOW_TEMP_ROOT 覆盖）。构建脚本、Electron 主进程、验证脚本
// 全都从这里取，谁也别自己拼路径 —— 拼歪了就是"界面加载不到 dist"。
const tempConst = createRequire(import.meta.url)('../app/temp-const.cjs') as {
  TEMP_ROOT: string
}

// https://vite.dev/config/
export default defineConfig({
  plugins: [
    vue(),
    vueJsx(),
    vueDevTools(),
  ],
  // 构建产物放到 <临时根>/dist，Electron 主进程按同一路径加载（app/main.cjs）。
  // 理由同其它临时产物：dist 是跑一次就有、删掉能重建的东西。
  // base './' 是为了让 loadFile() 能正确解析 ./assets/*。
  base: './',
  build: {
    outDir: `${tempConst.TEMP_ROOT.replace(/\\/g, '/')}/dist`,
    emptyOutDir: true,
  },
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    },
  },
})
