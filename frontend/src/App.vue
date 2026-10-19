<template>
  <div class="app">
    <!-- 顶栏：只留状态，不放日志 -->
    <header class="bar">
      <span class="dot" :class="statusClass" />
      <strong class="name">ShadowPlay 即时重放</strong>

      <span v-if="running" class="meta">
        <b>{{ fpsShown }}</b> fps
      </span>

      <!-- 录制状态：只留一个圆点 + 是否在录。分片数、磁盘占用这些是给排查用的
           内部数字，对用户没有意义，所以不再展示（需要时看服务日志或导出按钮）。 -->
      <span
        v-if="activeLive"
        class="meta rec"
        :class="{ bad: activeLive.recordFailed, off: !activeLive.recording }"
        :title="recordTitle"
      >
        <span class="recdot" />
        {{ activeLive.recording ? '录制中' : activeLive.recordFailed ? '录制失败' : '未录制' }}
      </span>

      <span class="spacer" />

      <!-- 设置：齿轮就在"服务运行中"左边。
           只放一行入口，真正的开关在弹窗里 —— 顶栏已经够挤了，而且这类
           参数平时不用看，收起来更干净。 -->
      <button class="gear" title="设置" @click="openSettings">⚙</button>

      <!-- 服务状态。
           以前这里写的是"外部服务"，因为服务的状态只走 stdout，而 stdout 只有
           启动它的进程读得到 —— 于是 cmd / npm run service 起的服务，界面一律
           当它没在跑，录制徽标空着、导出按钮灰着，可服务其实跑得好好的。
           现在服务还会每秒写一份状态文件，主进程读它，所以**不管谁起的**
           都看得见。这里就显示"运行中"，并注明状态是从哪条路来的。 -->
      <span
        v-if="live && live.running"
        class="meta svc"
        :class="{ offcfg: live.profilesMismatch }"
        :title="serviceStatusTitle"
      >
        <span class="svcdot" />
        服务运行中
        <em v-if="live.profilesMismatch">（配置不同）</em>
        <em v-else-if="live.source === 'file'">（状态文件）</em>
        <em v-else-if="!live.owned">（外部）</em>
      </span>
      <span v-else class="meta warn" :title="serviceStatusTitle">服务未启动</span>

      <button
        class="export"
        :disabled="!canExport"
        :title="exportTitle"
        @click="doExport"
      >
        {{ exporting ? '导出中…' : '导出回放' }}
      </button>

      <button v-if="fatal" class="retry" @click="retry">重试连接</button>
    </header>

    <!-- 导出结果/失败的提示条 -->
    <div v-if="exportNotice" class="toast" :class="{ bad: exportNotice.bad }">
      <span class="toast-text">{{ exportNotice.text }}</span>
      <button class="toast-x" @click="dismissExportNotice">×</button>
    </div>

    <!-- 设置弹窗 ------------------------------------------------------------
         点击遮罩或右上角 × 关闭；Esc 也能关（见 settingsKeydown）。
         刻意做成"覆盖层"而不是新窗口：新开 BrowserWindow 要另写一份 preload
         和 IPC，为一个滑杆不值当。 -->
    <div v-if="settingsOpen" class="sheet-mask" @click.self="closeSettings">
      <div class="sheet" role="dialog" aria-modal="true" aria-label="设置">
        <div class="sheet-head">
          <strong>设置</strong>
          <button class="sheet-x" title="关闭" @click="closeSettings">×</button>
        </div>

        <div class="sheet-body">
          <label class="row">
            <span class="row-label">预览画面最大帧率</span>
            <span class="row-ctl">
              <input
                :value="previewStepIndex"
                type="range"
                min="0"
                :max="previewFpsSteps.length - 1"
                step="1"
                :disabled="settingsSaving"
                @input="onPreviewStepInput"
              />
              <b class="row-val">{{ previewMaxFpsDraft === 0 ? '不限' : `${previewMaxFpsDraft} fps` }}</b>
            </span>
          </label>
        </div>

        <div class="sheet-foot">
          <span v-if="settingsError" class="sheet-err">{{ settingsError }}</span>
          <span class="spacer" />
          <button class="sheet-btn primary" :disabled="settingsSaving" @click="saveSettings">
            {{ settingsSaving ? '保存中…' : '确认' }}
          </button>
        </div>
      </div>
    </div>

    <div class="body">
      <!-- 左栏：配置列表（需求 2 的"逐项开关"就在这里） -->
      <aside class="side">
        <div class="side-head">
          <span>配置</span>
          <button class="mini" :disabled="saving" @click="addProfile">＋ 新建</button>
        </div>

        <div v-if="!profiles.length" class="empty">
          还没有配置。点"＋ 新建"加一路。
        </div>

        <ul class="plist">
          <li
            v-for="p in profiles"
            :key="p.id"
            class="pitem"
            :class="{ active: p.id === selectedId, off: !p.enabled }"
            @click="select(p.id)"
          >
            <!-- iPhone 那种滑动开关：一个 checkbox 用 CSS 画成轨道 + 圆钮。
                 保留原生 input 是为了键盘可达性（空格能切）和无障碍语义。 -->
            <label class="switch" :title="p.enabled ? '停用当前配置' : '启用当前配置'" @click.stop>
              <input
                type="checkbox"
                :checked="p.enabled"
                :aria-label="`启用 ${p.name || p.id}`"
                @change="toggleEnabled(p)"
              />
              <span class="switch-track"><span class="switch-knob" /></span>
            </label>
            <div class="pmeta">
              <div class="pname">
                {{ p.name || p.id }}
                <span class="pid">{{ p.id }}</span>
              </div>
              <div class="pdesc">{{ describeProfile(p) }}</div>
            </div>
          </li>
        </ul>

        <!-- 选中配置的详情 + 编辑入口 -->
        <div v-if="selectedProfile" class="editor">
          <div class="side-head">
            <span>当前配置</span>
            <button class="mini" @click="editing = !editing">
              {{ editing ? '收起' : '编辑' }}
            </button>
          </div>

          <!-- 详情面板只放用户真正要看的：当前配置录的是什么、录多大、录多清。
               分辨率、帧率实测值、分片数、落地目录、采集方式（WGC/裁剪）这些都是
               排查用的内部数字，对用户没有意义，一律不展示。 -->
          <dl class="kv">
            <dt>源</dt>
            <dd>{{ describeSource(selectedProfile) }}</dd>
            <dt v-if="selectedProfile.source.kind === 'window'">匹配到</dt>
            <dd v-if="selectedProfile.source.kind === 'window'">
              {{ selectedLive?.matchedTitle || '（还没匹到）' }}
            </dd>
            <dt>当前配置</dt>
            <dd>{{ describeProfile(selectedProfile) }}</dd>
          </dl>

          <p v-if="selectedLive?.recordFailed" class="bad">{{ selectedLive.recordDetail }}</p>

          <!-- 编辑表单：窗口匹配规则 / 帧率 / 比特率 / 回放时长 -->
          <form v-if="editing && selectedProfile" class="form" @submit.prevent="saveEdits">
            <label>
              配置名称
              <input v-model="draft.name" type="text" />
            </label>

            <label>
              采集方式
              <select v-model="draft.sourceKind">
                <option value="monitor">显示器捕获</option>
                <option value="window">窗口捕获</option>
              </select>
            </label>

            <!-- 显示器捕获：下拉选显示器，右边一个刷新按钮 -->
            <label v-if="draft.sourceKind === 'monitor'">
              显示器
              <span class="pickrow">
                <select v-model.number="draft.monitorIndex">
                  <option v-for="m in monitors" :key="m.index" :value="m.index">
                    显示器 {{ m.index + 1 }} · {{ m.width }}×{{ m.height
                    }}{{ m.primary ? ' · 主屏' : '' }}
                  </option>
                  <!-- 列表还没拉回来 / 拉回来但没有当前这块：留一个占位，
                       否则 <select> 会静默选中第一项，用户一存就换了显示器 -->
                  <option v-if="!monitors.some((m) => m.index === draft.monitorIndex)" :value="draft.monitorIndex">
                    显示器 {{ draft.monitorIndex + 1 }}（当前配置）
                  </option>
                </select>
                <button
                  type="button"
                  class="iconbtn"
                  :class="{ spin: loadingMonitors }"
                  :title="loadingMonitors ? '正在刷新…' : '刷新显示器列表'"
                  aria-label="刷新显示器列表"
                  :disabled="loadingMonitors"
                  @click="loadMonitors"
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M21 12a9 9 0 1 1-2.64-6.36" />
                    <polyline points="21 3 21 9 15 9" />
                  </svg>
                </button>
              </span>
            </label>

            <template v-if="draft.sourceKind === 'window'">
              <!-- 窗口捕获：OBS 同款的窗口下拉 + 刷新按钮 -->
              <label>
                窗口
                <span class="pickrow">
                  <select :value="windowPickValue" @change="useWindow(($event.target as HTMLSelectElement).value)">
                    <option value="">（选择一个窗口）</option>
                    <option v-for="w in windows" :key="w.hwnd" :value="String(w.hwnd)">
                      {{ windowLabel(w) }}
                    </option>
                  </select>
                  <button
                    type="button"
                    class="iconbtn"
                    :class="{ spin: loadingWindows }"
                    :title="loadingWindows ? '正在刷新…' : '刷新窗口列表'"
                    aria-label="刷新窗口列表"
                    :disabled="loadingWindows"
                    @click="loadWindows"
                  >
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <path d="M21 12a9 9 0 1 1-2.64-6.36" />
                      <polyline points="21 3 21 9 15 9" />
                    </svg>
                  </button>
                </span>
              </label>

              <!-- 匹配优先级：拖动上下调整，越靠上越优先 -->
              <div class="prio">
                <div class="prio-head">
                  <span>匹配优先级</span>
                  <span class="prio-tip">上下拖动调整，越靠上越优先</span>
                </div>
                <ul class="prio-list">
                  <li
                    v-for="(f, i) in draft.matchOrder"
                    :key="f"
                    class="prio-item"
                    :class="{ dragging: dragIndex === i }"
                    draggable="true"
                    @dragstart="onDragStart(i, $event)"
                    @dragover="onDragOver(i, $event)"
                    @dragend="onDragEnd"
                  >
                    <span class="prio-grip" aria-hidden="true">⠿</span>
                    <span class="prio-rank">{{ i + 1 }}</span>
                    <span class="prio-name">{{ MATCH_FIELD_LABEL[f] ?? f }}</span>
                    <span class="prio-move">
                      <button type="button" class="tiny" :disabled="i === 0" aria-label="上移" @click="moveMatchField(i, -1)">↑</button>
                      <button
                        type="button"
                        class="tiny"
                        :disabled="i === draft.matchOrder.length - 1"
                        aria-label="下移"
                        @click="moveMatchField(i, 1)"
                      >↓</button>
                    </span>
                  </li>
                </ul>
                <p v-if="matchPatternHint()" class="prio-hint">{{ matchPatternHint() }}</p>
              </div>

              <!-- 「多个都匹配时选」不再给用户选：同时命中多个窗口时一律取面积
                   最大的那个（看得最多的那个）。要求用户理解 foreground /
                   oldest 的区别、还要自己猜为什么某个窗口没被选中，不值得。 -->
            </template>

            <label>
              录制帧率
              <select v-model.number="draft.fps">
                <option v-for="f in fpsOptions" :key="f" :value="f">{{ f }} fps</option>
              </select>
            </label>

            <!-- 这里不再显示录像尺寸：显示器源就用显示器自己的宽高，窗口源就用
                 窗口自己的宽高，服务端按源的原始尺寸采集和编码。分辨率对用户
                 是个纯展示的数字，改不了也不用管，占着地方反而分心。 -->

            <label>
              比特率（Mbps）
              <input v-model.number="draft.bitrateMbps" type="number" min="1" max="200" />
            </label>
            <label>
              回放时长（秒）
              <input v-model.number="draft.replaySeconds" type="number" min="5" max="3600" />
            </label>

            <!-- 「启用当前配置」不再放在编辑面板里：侧栏每一行右边本来就有开关，
                 同一个开关摆两处只会让人搞不清哪次点击才算数。 -->

            <div class="actions">
              <button type="submit" class="primary" :disabled="saving">
                {{ saving ? '保存中…' : '保存' }}
              </button>
              <button type="button" class="mini" @click="editing = false">取消</button>
            </div>
            <p v-if="saveError" class="bad">{{ saveError }}</p>
            <p v-if="saveNotice" class="hint">{{ saveNotice }}</p>
          </form>
        </div>
      </aside>

      <!-- 预览区 -->
      <main class="stage">
        <canvas ref="canvasEl" class="surface" />

        <div v-if="!stats.rendered" class="overlay">
          <template v-if="fatal">
            <p class="bad">连接失败</p>
            <pre class="detail">{{ fatal }}</pre>
          </template>
          <template v-else-if="waitingService">
            <p>等待采集服务…</p>
            <pre class="detail">{{ serviceHint }}</pre>
          </template>
          <template v-else>
            <p>正在连接…</p>
          </template>
        </div>

        <!-- 当前配置没在出画面时，整个画面铺一层"无信号"。
             以前这里只在"一帧都没画过"时显示遮罩，于是窗口被关掉之后
             画面就静静停在最后一帧、状态还是"录制中"，看起来像卡死。
             现在只要选中的配置不是 live，画面就被这层不透明的 NO SIGNAL 盖掉：
             老画面一点都透不出来，用户一眼就知道"这里没画面"，不用读原因。 -->
        <div v-if="staleReason" class="nosignal">
          <div class="ns-bars" aria-hidden="true">
            <span v-for="n in 7" :key="n" :style="{ '--i': n }" />
          </div>
          <div class="ns-card">
            <p class="ns-title">NO SIGNAL</p>
          </div>
        </div>

        <!-- 服务没在跑时给一键启动 -->
        <div v-if="waitingService && !fatal" class="overlay-actions">
          <button class="primary" @click="launchService">启动采集服务</button>
        </div>
      </main>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'
import { usePreview } from '@/composables/usePreview'
import { startServiceBus, stopServiceBus, useServiceBus, clearExportNotice } from '@/lib/serviceBus'

const { canvas, running, fatal, waitingService, serviceHint, stats, start, stop, retry, switchSession, fpsOf, seedLedgerFromService, reportStats, setupHistory, getHistory } =
  usePreview()

const { live, lastExport, exportBusy } = useServiceBus()

// 自检钩子：主进程 --selftest 通过它核对渲染状态（推送模式）。
;(window as unknown as { __shadowStats?: () => unknown }).__shadowStats = reportStats
;(window as unknown as { __shadowHistory?: () => unknown }).__shadowHistory = getHistory

/**
 * 界面侧接线的自检快照（需求 2 的配置表单 + 需求 6 的导出按钮）。
 *
 * 它会被塞进页面每 500ms 推给主进程的那笔采样里（见下面 setupHistory 的入参），
 * 所以取值必须**只读标量**、不能有副作用、不能扫磁盘。
 */
function guiSnapshot(): Record<string, unknown> {
  const p = selectedProfile.value
  const s = activeLive.value
  // 注意 `live` 是 ServiceLiveSnapshot（不是数组），会话在 live.sessions 里。
  // 之前误当数组用 lives.filter(...)，被 setupHistory 的 try/catch 吞掉，
  // 现象是"这一整块字段全是空的"，看不出是取数炸了。
  const sessions = live.value.sessions ?? []
  return {
    profileCount: profiles.value.length,
    enabledCount: profiles.value.filter((x) => x.enabled).length,
    selectedId: selectedId.value,
    selectedName: p?.name ?? null,
    hasForm: Boolean(p),
    // 需求 (2) 点名要能配的四项：帧率 / 比特率 / 回放时长 / 窗口匹配规则
    formFps: p?.video.fps ?? null,
    formBitrate: p?.video.bitrateMbps ?? null,
    formReplaySeconds: p?.video.replaySeconds ?? null,
    // 自检要用"回放窗口 / 单片秒数"算够不够录满，别拿魔法数 3 段去催导出。
    formSegmentSeconds: p?.video.segmentSeconds ?? 2,
    matchBy: p?.match?.by ?? null,
    matchPattern: p?.match?.pattern ?? null,
    liveOwned: live.value.owned === true,
    // "服务到底在不在跑"必须带上：这正是 cmd 起服务时界面显示"没启动"的那条。
    liveRunning: live.value.running === true,
    liveSource: live.value.source ?? 'none',
    liveMirrorFile: live.value.mirrorFile ?? '',
    liveCount: sessions.length,
    // 注意：这里**没有**"连上几路"这个数。服务每条 session 的状态里根本没有
    // connected 字段（它只在 Session.cpp 的采集循环内部用），所以从 live 里数
    // 只能数出错的东西；真正的连接计数在 usePreview 的 snapshot() 里
    // （readySessions / connectedSessions），别在这儿重算一遍。
    liveRecording: sessions.filter((x) => x.recording).length,
    // `liveSegments/liveBytes` 是**所有路加起来**。自检脚本曾经拿它当"够录了"
    // 的判据，于是三路配置下 p-win 出 2 段就把总数顶到 3，导出的其实是只录了
    // 2 秒的主路 —— 20 秒回放窗口里 18 秒"还没录到"，补出一整段 NO SIGNAL。
    // 要判"这一路可以导了"必须看下面这对按选中路取值的字段。
    liveSegments: sessions.reduce((n, x) => n + (x.recordSegments ?? 0), 0),
    liveBytes: sessions.reduce((n, x) => n + (x.recordBytes ?? 0), 0),
    selectedSegments: s?.recordSegments ?? 0,
    selectedBytes: s?.recordBytes ?? 0,
    // 服务用的配置和界面这份是不是同一个文件。不是的话，上面那两个数属于
    // 另一个服务的录制目录，界面要显式标出来（顶栏琥珀色"（配置不同）"）。
    profilesMismatch: live.value.profilesMismatch === true,
    wantedProfilesPath: live.value.wantedProfilesPath ?? null,
    runningProfilesPath: live.value.runningProfilesPath ?? null,
    canExport: canExport.value,
    exportTitle: exportTitle.value,
    recordDir: s?.recordDir ?? null,
    exportNotice: exportNotice.value?.text ?? null,
    // 自检用的失败原因：界面读配置出错时不会抛异常，只会写进 saveError，
    // 不把它带出去就只看到"配置列表是空的"，查不出是接口没接还是文档没读进来。
    guiError: saveError.value || null,
    apiHasProfiles: typeof api()?.serviceProfiles === 'function',
  }
}
;(window as unknown as { __shadowGui?: () => unknown }).__shadowGui = guiSnapshot
// 自检用：真的走一次"点导出"（页面 → preload → 主进程 → 服务 stdin）
;(window as unknown as { __shadowExport?: () => Promise<unknown> }).__shadowExport = () =>
  doExport().then(() => guiSnapshot())

setupHistory(guiSnapshot)

const canvasEl = useTemplateRef<HTMLCanvasElement>('canvasEl')

// ---- 配置文档 -------------------------------------------------------------
interface WindowItem {
  hwnd: number | string
  title?: string
  class?: string
  process?: string
  width?: number
  height?: number
}
interface MonitorItem {
  index: number
  name?: string
  left?: number
  top?: number
  width?: number
  height?: number
  primary?: boolean
}
interface Profile {
  id: string
  /**
   * 录像目录用的数字编号（1、2、3…），对应 `records/<num>`。
   *
   * 只在新建时分配一次，之后永不改动 —— 改名字、换采集源都不会让它变，
   * 所以主人的录像不会因为改了配置名就"找不到"。
   */
  num?: number
  name: string
  enabled: boolean
  source: { kind: string; monitorIndex: number; clientAreaOnly: boolean; wgc?: boolean }
  video: {
    fps: number
    bitrateMbps: number
    replaySeconds: number
    /** 分片时长固定 2 秒，界面上没有入口；保留字段只为了读写老配置不丢信息 */
    segmentSeconds?: number
    /**
     * 录像缩放目标。0 = 不缩放，用源的原始尺寸。
     *
     * 界面不再给用户填这两项（显示器源用显示器宽高、窗口源用窗口宽高）。
     * 保留字段只为了读写老 profiles.json 不丢信息 —— 保存时原样带过去，
     * 不主动清零。
     */
    scaleWidth?: number
    scaleHeight?: number
  }
  match?: {
    by: string
    /**
     * 有序的匹配字段，第一个最优先。用户在编辑面板里上下拖动改这个顺序。
     * 服务端（WindowEnum.cpp 的 MatchWindows）按序逐级降级：
     * 第一个能匹到窗口的字段说了算，匹不到才试下一个。
     */
    byOrder?: string[]
    mode: string
    pattern: string
    /**
     * 同时命中多个窗口时挑哪个。
     *
     * 界面上已经没有这个选项了（永远是 `largest`），字段保留只为读写老配置
     * 原样带过去 —— C++ 侧的 WindowMatch::pick 仍支持 largest/foreground/
     * first/oldest，别在这一步把老配置的值默默改成 largest。
     */
    pick?: string
    /**
     * 只匹配可见窗口。UI 上也删了，永远按 true 走（隐藏/最小化的窗口不录），
     * 字段同样只用于读写老配置。
     */
    requireVisible?: boolean
    /** 从下拉里选窗口时记下的三份指纹，重排优先级时用来重填 pattern。 */
    title?: string
    className?: string
    process?: string
  }
  createdAt?: string
}

const api = () => (window as unknown as { shadowPreview?: Record<string, unknown> }).shadowPreview

const profiles = ref<Profile[]>([])
const recordRoot = ref('')
const exportDir = ref('')
const selectedId = ref<string | null>(null)
const editing = ref(false)
const saving = ref(false)
const saveError = ref('')
const saveNotice = ref('')
const windows = ref<WindowItem[]>([])
const loadingWindows = ref(false)
const monitors = ref<MonitorItem[]>([])
const loadingMonitors = ref(false)

const selectedProfile = computed(() => profiles.value.find((p) => p.id === selectedId.value) ?? null)

const liveOf = (id: string) => live.value.sessions.find((s) => s.id === id) ?? null
const activeLive = computed(() => (selectedId.value ? liveOf(selectedId.value) : null))
const selectedLive = computed(() => activeLive.value)

/**
 * 选中的配置没在出画面时，NO SIGNAL 画面上该写什么。
 *
 * 判据只看服务端上报的 state —— 那是唯一知道"窗口还在不在"的地方。
 * live 之外的几种都要说清楚，否则用户看到的只是画面定格：
 *   rebuilding 正在重建（窗口关了在等它回来）
 *   disabled   匹配不到窗口
 *   failed     采集失败
 * 服务本身没起来（waitingService / 没有 live 数据）时返回空，
 * 交给上面那层"等待采集服务"的遮罩去讲，别两句话叠一起。
 */
const staleReason = computed<{ title: string; detail: string } | null>(() => {
  if (waitingService.value || fatal.value) return null
  const l = activeLive.value
  if (!l) return null
  const state = String(l.state ?? '')
  // 没有环形缓冲的活跃会话也要盖住：源刚回来、帧还没到的那一瞬间，
  // 服务端 state 已经是 live，但画面还是一张都没有（canvas 上什么都没有）。
  // 不看这个的话会出现"遮罩已经撤了、铺面而来一片空白"的观感。
  const hasFrame = (l.width ?? 0) > 0 && (l.height ?? 0) > 0
  if (state === 'live') return hasFrame ? null : { title: '当前配置没有画面', detail: '正在等第一帧' }
  if (state === 'waiting') return null
  const detail = l.detail || '没有更多信息'
  if (state === 'disabled') return { title: '当前配置没有画面', detail }
  if (state === 'failed') return { title: '当前配置采集失败', detail }
  return { title: '当前配置正在重连…', detail }
})

// 把服务端每秒上报的每一路实测帧率喂给按路账本当兜底。
//
// 见 usePreview 里 seedLedgerFromService 的注释：一路刚被第一次打开时页面
// 自己还没攒出历史，靠这个先把数字垫上，切换时才不会先显示 0 再等一两秒。
watch(
  () => live.value.sessions,
  (sessions) => seedLedgerFromService(sessions ?? []),
  { immediate: true },
)

const exporting = computed(
  () => exportBusy.value !== null || activeLive.value?.exportRunning === true,
)

const canExport = computed(() => {
  const s = activeLive.value
  if (!s) return false
  // exportBusy 是界面自己进程里那份导出状态（见 serviceBus.ts）；
  // s.exportRunning 是**服务**侧那条老通道，服务不在跑时永远是 false，
  // 所以两个都要看，否则复用外部服务时按钮会被错误地锁上/解锁。
  return !exporting.value && s.recordSegments > 0
})

const exportTitle = computed(() => {
  const s = activeLive.value
  if (!s) return '先选一路配置'
  if (exporting.value) return exportBusy.value?.phase || '上一轮还在导'
  if (!s.recordSegments) return '还没有可导的分片（服务没在录，或界面读不到服务状态）'
  return `把最近 ${selectedProfile.value?.video.replaySeconds ?? 0} 秒合并成一个 MP4`
})

const recordTitle = computed(() => {
  const s = activeLive.value
  if (!s) return ''
  if (s.recordFailed) return s.recordDetail || '录制失败'
  return s.recording ? '正在录制，内容会在回放窗口内滚动覆盖' : '未在录制'
})

/**
 * 顶栏那枚"服务运行中 / 服务未启动"的悬停说明。
 *
 * 要写清**为什么**是运行中或不是：这两条通道（stdout / 状态文件）是这次修的
 * 重点，出问题时用户第一眼就该看出是"服务真没起"还是"状态没接上"。
 */
const serviceStatusTitle = computed(() => {
  const l = live.value
  const file = l.mirrorFile || '（状态文件路径未知）'
  if (l.running) {
    // 服务在跑，但它加载的配置和界面这份**不是同一份文件** —— 这时它报的分片数、
    // 录制目录全属于另一个服务的记录，导出按钮也会去别处找分片。必须明说，
    // 否则用户看到"有 7 段"却导不出来，只能靠猜。
    if (l.profilesMismatch) {
      return (
        '服务在运行，但它用的配置文件和界面这份不是同一个：\n' +
        `  界面：${l.wantedProfilesPath ?? '（未知）'}\n` +
        `  服务：${l.runningProfilesPath ?? '（未知）'}\n` +
        '下面的分片数 / 录制目录都属于那个服务，导出会去它的 recordRoot 找分片。\n' +
        '要让界面自己起服务，先停掉命令行里的那个。'
      )
    }
    if (l.source === 'file') {
      const age = l.mirrorAgeMs === null ? '刚刚' : `${(l.mirrorAgeMs / 1000).toFixed(1)} 秒前`
      return `服务在运行（不是本界面启动的，状态来自状态文件，${age}更新）\n${file}`
    }
    if (!l.owned) return `服务在运行（外部启动，状态来自它的输出）\n${file}`
    return '服务在运行（本界面启动，状态来自它的输出）'
  }
  return `没有读到服务状态。\n状态文件：${file}`
})

// ---- 预览帧率通知 ---------------------------------------------------------
//
// 主进程会在两处推这个值：设置页保存时，以及服务启动后把存下来的设置下发
// 一遍。所以界面要能收到"外部改的"这个值，不能只信自己那次调用。
onMounted(() => {
  const pp = api() as {
    onPreviewFps?: (cb: (fps: number) => void) => () => void
    getSettings?: () => Promise<{ ok: boolean; settings?: { previewMaxFps: number } }>
  }
  const off = pp?.onPreviewFps?.((fps) => {
    if (typeof fps === 'number' && Number.isFinite(fps)) previewMaxFps.value = fps
  })
  onBeforeUnmount(() => off?.())

  // 启动时**主动问一次**存下来的值。
  //
  // 只靠上面那条推送是不够的：主进程把设置下发给页面的那一步挂在
  // 「服务命令通道通了」之后（见 app/main.cjs 的 pushSettingsToService），
  // 服务没起 / 管道没建好的时候它压根不会推 —— 于是界面一直停在
  // ref(60) 的默认值，用户上次设的 30 或 120 全丢了，看起来就是"没保存"。
  // 这里直接读文件里的值，跟推送谁先谁后都无所谓。
  void (async () => {
    try {
      const res = (await pp?.getSettings?.()) ?? null
      const fps = res?.settings?.previewMaxFps
      if (typeof fps === 'number' && Number.isFinite(fps)) previewMaxFps.value = fps
    } catch {
      /* 主进程还没就绪，保持默认值 */
    }
  })()
})

// ---- 设置 -----------------------------------------------------------------
const settingsOpen = ref(false)
const settingsSaving = ref(false)
const settingsError = ref('')

/** 当前生效的预览帧率上限（0 = 不限）。启动时从主进程读一次。 */
const previewMaxFps = ref(60)

/**
 * 预览帧率滑杆可停的位置。
 *
 * 和编辑页的录制帧率共用 FPS_CHOICES：两处口径一致，用户在编辑页看到的
 * 「有哪几档」，设置页照样是那几档，不会出现"这里能选 165、那里只能到 144"。
 * 最前面补一个 0 = 不限。
 *
 * 滑杆的 value 用的是**下标**而不是帧率本身：帧率档位不是等距的（5→15 差 10，
 * 144→165 差 21），直接拿帧率当 value 拖起来会一格大一格小、手感很怪。
 */
const previewFpsSteps = computed(() => [0, ...FPS_CHOICES])

/** 草稿值 → 滑杆下标。落不到具体档位时（老配置里的怪值）取不超过它的最近一档。 */
const previewStepIndex = computed(() => {
  const steps = previewFpsSteps.value
  const v = normaliseFps(previewMaxFpsDraft.value)
  if (v <= 0) return 0
  let best = 1
  for (let i = 1; i < steps.length; i++) {
    if (steps[i] <= v) best = i
    else break
  }
  return best
})

/** 弹窗里那份草稿值：只有点"保存"才生效，中途取消不影响正在跑的预览。 */
const previewMaxFpsDraft = ref(60)

/** 把滑杆/输入来的值夹到合法范围（0..240 的整数）。 */
function normaliseFps(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n)) return 0
  return Math.max(0, Math.min(240, Math.round(n)))
}

/**
 * 滑杆拖动：value 是**档位下标**，换算成真实帧率存进草稿。
 *
 * 这层换算放在脚本里而不是模板表达式里，是因为模板里写
 * `$event.target as HTMLInputElement` 这种 TS 断言在 Vue 的模板编译器里
 * 并不可靠（构建能过，运行时可能拿到 undefined 静默不生效），放这儿能直接
 * 断言、也能写注释解释为什么要减下标。
 */
function onPreviewStepInput(e: Event) {
  const el = e.target as HTMLInputElement | null
  if (!el) return
  const i = Number(el.value)
  if (!Number.isFinite(i)) return
  previewMaxFpsDraft.value = previewFpsSteps.value[i] ?? 0
}

async function openSettings() {
  settingsError.value = ''
  previewMaxFpsDraft.value = previewMaxFps.value
  settingsOpen.value = true
  // 打开时再问一次主进程：设置文件可能被手动改过，也可能主进程这边还记着
  // 一个界面没收到过的值。
  try {
    const res = (await (api() as { getSettings?: () => Promise<{ ok: boolean; settings?: { previewMaxFps: number } }> })
      ?.getSettings?.()) ?? null
    const fps = res?.settings?.previewMaxFps
    if (typeof fps === 'number') {
      previewMaxFps.value = fps
      previewMaxFpsDraft.value = fps
    }
  } catch {
    /* 读不到就用内存里的值，不影响保存 */
  }
}

function closeSettings() {
  if (settingsSaving.value) return
  settingsOpen.value = false
}

async function saveSettings() {
  const fps = normaliseFps(previewMaxFpsDraft.value)
  previewMaxFpsDraft.value = fps
  settingsSaving.value = true
  settingsError.value = ''
  try {
    const res = (await (
      api() as { setSettings?: (p: { previewMaxFps: number }) => Promise<{ ok: boolean; error?: string; applied?: boolean }> }
    )?.setSettings?.({ previewMaxFps: fps })) ?? null
    if (!res || !res.ok) {
      settingsError.value = res?.error ?? '保存失败：界面版本和服务对不上。'
      return false
    }
    previewMaxFps.value = fps
    // 主人要的是"点确认就关"：不再弹一句"已生效"再等半秒，成功即关。
    // 真失败（res.ok 为假）才留在弹窗里显示错误，让人能重试。
    settingsOpen.value = false
    return true
  } catch (err) {
    settingsError.value = `保存出错：${(err as Error).message}`
    return false
  } finally {
    settingsSaving.value = false
  }
}

/** Esc 关弹窗。挂在 document 上，因为焦点可能停在滑杆上。 */
function settingsKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape' && settingsOpen.value) closeSettings()
}
onMounted(() => document.addEventListener('keydown', settingsKeydown))
onBeforeUnmount(() => document.removeEventListener('keydown', settingsKeydown))

// ---- 导出 -----------------------------------------------------------------
const exportNotice = ref<{ text: string; bad: boolean } | null>(null)

const dismissExportNotice = () => {
  exportNotice.value = null
  clearExportNotice()
}

async function doExport() {
  const s = activeLive.value
  if (!s || !canExport.value) return { ok: false, error: '导出按钮当前不可用', title: exportTitle.value }
  const fn = api()?.exportReplay as
    | ((id: string, seconds?: number) => Promise<{ ok: boolean; error?: string }>)
    | undefined
  if (typeof fn !== 'function') {
    exportNotice.value = { text: 'preload 没有导出接口，界面版本和服务对不上。', bad: true }
    return { ok: false, error: 'preload 没有导出接口' }
  }
  exportNotice.value = {
    text: `正在导出「${s.name}」最近 ${selectedProfile.value?.video.replaySeconds} 秒…`,
    bad: false,
  }
  const res = (await fn(s.id, selectedProfile.value?.video.replaySeconds)) ?? { ok: false }
  if (!res.ok) {
    exportNotice.value = { text: res.error ?? '导出命令没能发出去。', bad: true }
  }
  // 成功时就别再写提示了：导出是异步的，立刻改成"正在导出…"会把刚写的那句
  // 覆盖成一句没信息的话。完成/失败由下面的 export_status 事件来报。
  return { ...res, profileId: s.id, replaySeconds: selectedProfile.value?.video.replaySeconds ?? null }
}

// 导出结束由主进程广播的 export_status 通知（见 app/main.cjs 的 startExportStatusPoll）。
// 不用轮询，也不依赖服务 —— 导出跑在界面进程自己的插件里。
// 这里**不能**在 evt.running === true 时动提示：那句"正在导出…"是 doExport 写的，
// 每 400ms 重写一次会让用户以为界面在抽风。
watch(lastExport, (evt) => {
  if (!evt) return
  if (evt.running) return
  if (evt.failed) {
    exportNotice.value = { text: `导出失败：${evt.detail || '原因不明'}`, bad: true }
  } else if (evt.ok) {
    const mb = ((evt.bytes ?? 0) / 1024 / 1024).toFixed(1)
    exportNotice.value = {
      text: `导出完成：${evt.outPath}（${(evt.durationSeconds ?? 0).toFixed(1)} 秒 / ${mb} MB）`,
      bad: false,
    }
  }
  clearExportNotice()
})

// ---- 载入 / 保存配置 ------------------------------------------------------
async function loadProfiles() {
  const p = api()
  if (!p || typeof p.serviceProfiles !== 'function') {
    saveError.value = 'preload 没有配置接口，界面版本和服务对不上。'
    return
  }
  try {
    const doc = await fetchDoc()
    profiles.value = doc.profiles
    recordRoot.value = doc.recordRoot
    exportDir.value = doc.exportDir
    if (!selectedId.value || !profiles.value.some((x) => x.id === selectedId.value)) {
      selectedId.value = profiles.value[0]?.id ?? null
    }
  } catch (err) {
    saveError.value = `读配置失败：${String(err)}`
  }
}

/**
 * 拿配置文档。
 *
 * 注意这里问的是主进程的 `service:profiles` 而不是自己读磁盘 —— 主进程会顺手
 * 把缺省值补齐（normalizeProfiles），界面拿到的就是"服务真正会用的那份"。
 */
async function fetchDoc(): Promise<{ profiles: Profile[]; recordRoot: string; exportDir: string }> {
  const fn = api()?.serviceProfiles as (() => Promise<{
    items?: Profile[]
    recordRoot?: string
    exportDir?: string
  }>) | undefined
  if (typeof fn !== 'function') return { profiles: [], recordRoot: '', exportDir: '' }
  const res = (await fn()) ?? {}
  return {
    profiles: Array.isArray(res.items) ? res.items : [],
    recordRoot: String(res.recordRoot ?? ''),
    exportDir: String(res.exportDir ?? ''),
  }
}

function emptyProfile(): Profile {
  return {
    id: `p-${Math.random().toString(36).slice(2, 8)}`,
    // 录像目录就用这个数字：records/1、records/2…
    //
    // 取"现有最大号 + 1"，**不回收**空出来的号 —— 删掉配置 2 之后再新建，
    // 新配置拿到的是 4 而不是 2，免得它继承配置 2 留下来的旧录像。
    num: nextProfileNum(),
    name: `配置 ${profiles.value.length + 1}`,
    enabled: true,
    source: { kind: 'window', monitorIndex: 0, clientAreaOnly: false },
    video: { fps: 60, bitrateMbps: 20, replaySeconds: 120, segmentSeconds: 2 },
    match: { by: 'title', mode: 'contains', pattern: '', pick: 'largest', pollMs: 1000, requireVisible: true },
    createdAt: new Date().toISOString(),
  }
}

/** 下一个可用的配置编号（现有最大 +1，最小 1）。 */
function nextProfileNum(): number {
  let max = 0
  for (const p of profiles.value) {
    const n = Number(p.num ?? 0)
    if (Number.isFinite(n) && n > max) max = n
  }
  return max + 1
}

function addProfile() {
  const p = emptyProfile()
  profiles.value = [...profiles.value, p]
  selectedId.value = p.id
  startEdit()
}

/**
 * 「录制帧率上限」可选值。
 *
 * 录制帧率 = 采集器实际的抓帧节奏（定时间隔 = 1000/fps ms），
 * **和界面预览的帧率没有任何关系** —— 预览是另一条路，有它自己的上限。
 * 所以这里给到 240 是有意义的：录 240 帧的回放，预览可以只画 60 帧。
 *
 * 用下拉而不是数字框，是因为帧率得能整除到定时器上；随手填 37 会得到一个
 * 27.03ms 的间隔，实际帧率既不是 37 也不是 60，没人能解释得清。
 *
 * 5 / 15 是给「省磁盘」和「看幻灯片式回放」准备的：录 5fps 一小时也占不了
 * 多少空间，15fps 则够看懂操作过程。
 *
 * 设置页的预览帧率滑杆也用这一份列表（见 settingsFpsSteps），两处口径一致，
 * 不各维护一套数字。
 */
const FPS_CHOICES = [5, 15, 30, 60, 90, 120, 144, 165, 240]

/** 当前配置的帧率不在 FPS_CHOICES 里时，把原值临时挂上，免得下拉把值改掉 */
const extraFps = ref<number | null>(null)

/** 下拉里实际显示的帧率选项 */
const fpsOptions = computed(() => {
  const extra = extraFps.value
  if (extra === null || FPS_CHOICES.includes(extra)) return FPS_CHOICES
  return [...FPS_CHOICES, extra].sort((a, b) => a - b)
})

// 编辑草稿：直接编辑副本，取消就丢掉，避免"改了没保存但界面已经变了"
//
// 这里没有 matchPollMs 和 segmentSeconds：窗口轮询间隔由服务端自己定，
// 分片时长固定 2 秒，两个都不该让用户配。字段删掉之后 saveEdits 里也不能
// 再引用它们（存下来的老配置里还有这两个键，服务端会保留原值）。
const draft = ref({
  name: '',
  enabled: true,
  sourceKind: 'monitor',
  monitorIndex: 0,
  /**
   * 匹配优先级：有序字段列表，第一个最优先。用户在编辑面板里上下拖动改它。
   *
   * 默认「标题 > 窗口类名 > 进程 exe」：标题最精确（同一程序开两个窗口只有
   * 标题能分开），类名稳定但一个 exe 的多个窗口常同类，exe 名最稳也最粗。
   */
  matchOrder: ['title', 'class', 'process'] as string[],
  /**
   * 从下拉里选窗口时记下的三份指纹。用户重排优先级时，pattern 要跟着换成
   * 对应那份指纹 —— 否则把「标题」拖到后面（此时 pattern 还是标题）就会拿
   * 标题去比类名，永远匹不到。
   */
  matchTitle: '',
  matchClass: '',
  matchProcess: '',
  matchMode: 'contains',
  matchPattern: '',
  fps: 60,
  bitrateMbps: 20,
  replaySeconds: 120,
})

/**
 * 当前配置会录成多大 —— **不再展示给用户**。
 *
 * 显示器源就用显示器自己的宽高，窗口源就用窗口自己的宽高，服务端按源的原始
 * 尺寸采集，用户既改不了也不用关心。函数留着是因为"列出当前窗口"那个列表
 * 里还要显示窗口尺寸（帮用户认出哪个是目标窗口）。
 */

function startEdit() {
  const p = selectedProfile.value
  if (!p) return
  const order =
    Array.isArray(p.match?.byOrder) && p.match!.byOrder!.length
      ? [...p.match!.byOrder!]
      : ['title', 'class', 'process']
  draft.value = {
    name: p.name,
    enabled: p.enabled,
    sourceKind: p.source.kind,
    monitorIndex: p.source.monitorIndex,
    matchOrder: order,
    matchTitle: p.match?.title ?? '',
    matchClass: p.match?.className ?? '',
    matchProcess: p.match?.process ?? '',
    matchMode: p.match?.mode ?? 'contains',
    matchPattern: p.match?.pattern ?? '',
    // 老配置里可能存着不在 FPS_CHOICES 里的帧率（比如手改过 json）。
    // 直接塞进 <select> 会得到一个没有对应 <option> 的值 —— 浏览器会
    // 静默选第一个选项，用户一打开编辑就"被改了帧率"，保存下去就真写坏了。
    // 所以把它临时并进候选里，保持原值可选中。
    fps: p.video.fps,
    bitrateMbps: p.video.bitrateMbps,
    replaySeconds: p.video.replaySeconds,
  }
  extraFps.value = FPS_CHOICES.includes(p.video.fps) ? null : p.video.fps
  saveError.value = ''
  editing.value = true
  // 打开编辑面板就把当前显示器和窗口列表拉一遍，下拉框是空的用户没法选
  if (p.source.kind !== 'window') void loadMonitors()
  if (p.source.kind === 'window') void loadWindows()
}

function select(id: string) {
  selectedId.value = id
  editing.value = false
  exportNotice.value = null
}

function toggleEnabled(p: Profile) {
  p.enabled = !p.enabled
  // 只存当前配置。以前这里调 saveProfiles() 会把整份配置发过去，
  // 隔壁正在录的那一路会被界面这份旧快照回退。
  void saveOneProfile(p.id, p)
}

async function saveEdits() {
  const p = selectedProfile.value
  if (!p) return
  const d = draft.value
  const next: Profile = {
    ...p,
    name: d.name,
    enabled: d.enabled,
    source: { ...p.source, kind: d.sourceKind, monitorIndex: d.monitorIndex },
    video: {
      ...p.video,
      fps: d.fps,
      bitrateMbps: d.bitrateMbps,
      replaySeconds: d.replaySeconds,
      // 分片时长不再由用户配（固定 2 秒）；scaleWidth/scaleHeight 界面上
      // 也没有入口了，用 ...p.video 原样带过去，别在这一步把老配置里的
      // 非 0 缩放悄悄清掉。
    },
  }
  if (d.sourceKind === 'window') {
    next.match = {
      // by 保留成首选字段，老版本服务只认这一个键还能凑合用
      by: d.matchOrder[0] ?? 'title',
      byOrder: [...d.matchOrder],
      mode: d.matchMode,
      pattern: d.matchPattern,
      // 同时命中多个窗口时固定取面积最大的那个，界面上不再给选项。
      // 老配置里存的其它值（foreground/first/oldest）在这里统一改成 largest ——
      // 那四个选项本来就只是"挑哪个窗口"的启发式，面积最大最符合直觉。
      pick: 'largest',
      // 轮询间隔不再让用户配，沿用这个配置原来那份（老配置没有就用 1 秒）。
      pollMs: p.match?.pollMs ?? 1000,
      // 只匹配可见窗口：开关删了，固定 true（最小化/隐藏的窗口不录）。
      requireVisible: true,
      // 三份指纹原样留着：下次打开编辑面板重排优先级时要靠它们重填 pattern
      title: d.matchTitle,
      className: d.matchClass,
      process: d.matchProcess,
    }
  } else {
    delete next.match
  }
  profiles.value = profiles.value.map((x) => (x.id === p.id ? next : x))
  const ok = await saveOneProfile(p.id, next)
  if (ok) editing.value = false
}

/**
 * 保存**一路**配置。
 *
 * 【为什么不再发整份数组】
 *
 * 以前 `saveProfiles()` 把 `profiles.value` 整个深拷贝发过去，主进程整份写回
 * profiles.json。这份数组是界面打开那一刻的快照 —— 只要中间任何一路被别处
 * 改过（服务端重载、另一个窗口编辑、手工改 json），保存 A 路就会把 B 路顺手
 * 回退成旧值。B 路正在录制时，下一拍 reload 就换了帧率，用户看到的是"我只改
 * 了当前配置，隔壁怎么也抖了"。
 *
 * 现在只把当前配置发过去，主进程从磁盘读现配、只替换这一条，**别的分路根本
 * 不经过界面**，界面那份数据再旧也影响不到它们。
 */
async function saveOneProfile(id: string, patch: Profile): Promise<boolean> {
  const fn = api()?.saveProfile as
    | ((id: string, patch: unknown) => Promise<{
        ok: boolean
        error?: string
        path?: string
        changed?: boolean
        reloaded?: string[]
        failed?: { id: string; error?: string }[]
      }>)
    | undefined
  if (typeof fn !== 'function') {
    saveError.value = 'preload 没有单路保存接口。'
    return false
  }
  saving.value = true
  saveError.value = ''
  saveNotice.value = ''
  try {
    // 必须先转成纯数据：Profile 是 reactive Proxy，结构化克隆不认。
    const plain = JSON.parse(JSON.stringify(patch))
    const res = (await fn(id, plain)) ?? { ok: false }
    if (!res.ok) {
      saveError.value = res.error ?? '保存失败'
      return false
    }
    if (res.changed === false) {
      saveNotice.value = '已保存。当前配置没有改动，服务未做任何变更。'
    } else if (Array.isArray(res.failed) && res.failed.length > 0) {
      // 存盘成功但没送到服务。**不重启服务**，如实告诉用户再存一次即可。
      saveNotice.value = `已存盘，但这一拍没送到服务（${res.failed[0].error ?? '通道忙'}）。再点一次保存即可，不会重启服务。`
    } else if (!Array.isArray(res.reloaded) || res.reloaded.length === 0) {
      saveNotice.value = '已保存（服务当前没在运行，下次启动按新配置走）。'
    }
    // 改动生效时不再弹提示：那一行字只是复述"已经按新配置跑了"，
    // 用户要的是保存完就能直接用，不需要被告知成功。
    return true
  } catch (err) {
    saveError.value = String(err)
    return false
  } finally {
    saving.value = false
  }
}

async function loadWindows() {
  const fn = api()?.listWindows as (() => Promise<{ items?: WindowItem[] }>) | undefined
  if (typeof fn !== 'function') return
  loadingWindows.value = true
  try {
    const res = (await fn()) ?? {}
    windows.value = Array.isArray(res.items) ? res.items : []
  } finally {
    loadingWindows.value = false
  }
}

async function loadMonitors() {
  const fn = api()?.listMonitors as (() => Promise<{ items?: MonitorItem[] }>) | undefined
  if (typeof fn !== 'function') return
  loadingMonitors.value = true
  try {
    const res = (await fn()) ?? {}
    monitors.value = Array.isArray(res.items) ? res.items : []
  } finally {
    loadingMonitors.value = false
  }
}

/** 窗口下拉里的当前值：按首选字段的指纹反查 hwnd，查不到就留空。 */
const windowPickValue = computed(() => {
  const d = draft.value
  const primary = d.matchOrder[0] ?? 'title'
  const fp = fingerprintFor(primary, d.matchTitle, d.matchClass, d.matchProcess)
  if (!fp) return ''
  const hit = windows.value.find((w) => {
    if (primary === 'class') return String(w.class ?? '') === fp
    if (primary === 'process') return String(w.process ?? '') === fp
    return String(w.title ?? '') === fp
  })
  return hit ? String(hit.hwnd) : ''
})

/** 窗口下拉里的一行显示什么：标题 + exe，跟 OBS 的窗口列表一个路子。 */
function windowLabel(w: WindowItem): string {
  const t = String(w.title ?? '').trim() || '(无标题)'
  const exe = String(w.process ?? '').trim()
  return exe ? `${t}  —  ${exe}` : t
}

/**
 * 用户从下拉里挑了一个窗口。
 *
 * 跟 OBS 一样，选一次就把这个窗口的**三项身份**全记下来（标题 / 类名 / exe），
 * 然后按当前优先级填 pattern。之后用户拖动重排优先级时，pattern 会跟着换成
 * 对应那一项 —— 比如把「进程 exe」拖到第一位，pattern 就从标题变成 explorer.exe，
 * 这样同一个程序换了文档标题也还能录到。
 */
function useWindow(hwnd: string | number) {
  const w = windows.value.find((x) => String(x.hwnd) === String(hwnd))
  if (!w) return
  const title = String(w.title ?? '').trim()
  const cls = String(w.class ?? '').trim()
  const exe = String(w.process ?? '').trim()
  draft.value.matchTitle = title
  draft.value.matchClass = cls
  draft.value.matchProcess = exe
  // 标题可能很长（浏览器会带一堆后缀），截一段最前面的当匹配内容更稳
  const primary = draft.value.matchOrder[0] ?? 'title'
  draft.value.matchPattern = fingerprintFor(primary, title.slice(0, 32), cls, exe)
  // 挑窗口这件事本身就是「包含」语义（标题会变），正则留着手写
  draft.value.matchMode = 'contains'
}

/** 按字段取对应的指纹（供 pattern 用）。 */
function fingerprintFor(field: string, title: string, cls: string, exe: string): string {
  if (field === 'class') return cls
  if (field === 'process') return exe
  return title
}

/** 重排优先级：把 pattern 换成新首选字段的指纹。 */
function applyMatchOrder() {
  const d = draft.value
  const primary = d.matchOrder[0] ?? 'title'
  const fp = fingerprintFor(primary, d.matchTitle.slice(0, 32), d.matchClass, d.matchProcess)
  // 只在用户从下拉里选过窗口（三份指纹至少有一份）时才自动改 pattern；
  // 手写的正则/自定义内容不能被覆盖掉。
  if (fp) d.matchPattern = fp
}

const MATCH_FIELD_LABEL: Record<string, string> = {
  title: '窗口标题',
  class: '窗口类名',
  process: '程序名称',
}

/** 优先级行上显示的当前匹配内容（跟着顺序变，用户一眼看出在比什么）。 */
function matchPatternHint(): string {
  const d = draft.value
  const primary = d.matchOrder[0] ?? 'title'
  const fp = fingerprintFor(primary, d.matchTitle, d.matchClass, d.matchProcess)
  return fp ? `当前按${MATCH_FIELD_LABEL[primary] ?? primary}匹配：${fp}` : ''
}

// ---- 优先级拖动 -----------------------------------------------------------
//
// 用原生 HTML5 拖放（draggable + dragstart/dragover/drop），不引第三方库：
// 只有三行，手写比装一个 sortable 依赖划算，也不会多打一份包。
const dragIndex = ref<number | null>(null)

function onDragStart(i: number, ev: DragEvent) {
  dragIndex.value = i
  if (ev.dataTransfer) {
    ev.dataTransfer.effectAllowed = 'move'
    // 某些浏览器不带 data 就不肯触发 drop
    ev.dataTransfer.setData('text/plain', String(i))
  }
}

function onDragOver(i: number, ev: DragEvent) {
  ev.preventDefault() // 不阻止默认行为就收不到 drop
  const from = dragIndex.value
  if (from === null || from === i) return
  const list = [...draft.value.matchOrder]
  const [moved] = list.splice(from, 1)
  list.splice(i, 0, moved!)
  draft.value.matchOrder = list
  dragIndex.value = i
}

function onDragEnd() {
  dragIndex.value = null
  applyMatchOrder()
}

/** 上下按钮：触摸板/不习惯拖的人也能调顺序。 */
function moveMatchField(i: number, dir: -1 | 1) {
  const list = [...draft.value.matchOrder]
  const j = i + dir
  if (j < 0 || j >= list.length) return
  ;[list[i], list[j]] = [list[j]!, list[i]!]
  draft.value.matchOrder = list
  applyMatchOrder()
}

async function launchService() {
  const fn = api()?.openServiceConsole as (() => Promise<unknown>) | undefined
  if (typeof fn !== 'function') return
  await fn()
  // 服务起来到建环有几百毫秒，稍等一下再重连
  setTimeout(() => void retry(), 900)
}

// ---- 其它 -----------------------------------------------------------------
const statusClass = computed(() => {
  if (fatal.value) return 'bad'
  if (running.value && stats.value.rendered > 0) return 'ok'
  return 'wait'
})

// ---- 顶栏帧率显示 ---------------------------------------------------------
//
// 显示的是**当前选中那一路**自己的帧率（usePreview 里按路各存一份账本），
// 而且取的是那一路算好的**滑动中位数**，不是瞬时值 —— 瞬时值每 500ms 现算
// 一次，60fps 下一个窗口多收一帧就成 62、少收一帧就成 58，数字一直跳，看着
// 像"帧数不稳"，其实采集侧是稳的。
//
// 以前这里是"跟着 stats.renderFpsShown 走 + 差不到 1.5 就不改"的滞后闸门：
// 那个闸门本来是为了挡抖动，副作用是切配置时要等新数字和旧数字差够 1.5 才
// 肯更新，看起来就是"切过去了还在慢慢过渡"。现在帧率按路存，切换只是换一个
// 指针（watch selectedId），所以直接取那一路的值就行，不需要任何过渡。
const fpsShown = computed(() => Math.round(fpsOf(selectedId.value)))

function describeSource(p: Profile): string {
  if (p.source.kind === 'desktop') return '整屏桌面'
  if (p.source.kind === 'monitor') return `显示器 ${p.source.monitorIndex + 1}`
  const m = p.match
  if (!m) return '窗口（没写匹配规则）'
  const order = Array.isArray(m.byOrder) && m.byOrder.length ? m.byOrder : [m.by]
  const nameOf = (f: string) =>
    f === 'title' ? '标题' : f === 'process' ? 'exe' : f === 'class' ? '类名' : f
  const mode = m.mode === 'contains' ? '包含' : m.mode === 'exact' ? '等于' : m.mode === 'prefix' ? '开头' : '正则'
  // 只有一个字段时保持老样子「标题包含「x」」；多字段时把优先级排出来，
  // 用户一眼能看出"先按标题找，找不到才降级用类名"
  if (order.length <= 1) return `${nameOf(order[0] ?? 'title')}${mode}「${m.pattern || '（空）'}」`
  return `${order.map(nameOf).join(' › ')} ${mode}「${m.pattern || '（空）'}」`
}

function describeProfile(p: Profile): string {
  return `${describeSource(p)} · ${p.video.fps}fps · ${p.video.bitrateMbps}Mbps · ${p.video.replaySeconds}s`
}

// 选中哪一路 -> 预览就接哪一路的帧环
watch(selectedId, (id) => {
  void switchSession(id)
})

// 编辑按钮和 sidebar 的联动
watch(editing, (v) => {
  if (v) startEdit()
})

onMounted(() => {
  canvas.value = canvasEl.value
  startServiceBus()
  void loadProfiles()
  void start()
})

onBeforeUnmount(() => {
  stopServiceBus()
  stop()
})
</script>

<style scoped>
.app {
  display: flex;
  flex-direction: column;
  height: 100vh;
  margin: 0;
  background: #05070c;
  color: #e7ecf5;
  font: 13px/1.5 'Segoe UI', system-ui, sans-serif;
  overflow: hidden;
}

.bar {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 0 14px;
  height: 32px;
  flex: 0 0 auto;
  background: #0d1119;
  border-bottom: 1px solid #1b2231;
}

.name {
  font-size: 12.5px;
  font-weight: 600;
}

.meta {
  color: #7c879b;
  font-size: 12px;
  font-variant-numeric: tabular-nums;
}
.meta b {
  color: #cbd5e6;
}
.meta.warn {
  color: #e0a83c;
}

/* 顶栏那枚"服务运行中"。绿色代表"我确实读到了服务状态" —— 这跟
   "预览有没有画面"是两件事，所以颜色刻意和左边那颗连接灯分开用。 */
.meta.svc {
  color: #6fdc9f;
}
.meta.svc em {
  color: #7c879b;
  font-style: normal;
}
.svcdot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  margin-right: 4px;
  background: #35d07f;
  box-shadow: 0 0 6px #35d07f88;
}

/* 服务在跑，但用的不是界面这份配置。这时候"运行中"是**真的**，
   可它报的数字不是这个界面的 —— 用琥珀色把它跟正常绿区分开，
   免得用户只看颜色就以为一切正常。 */
.meta.svc.offcfg,
.meta.svc.offcfg .svcdot {
  color: #e0a83c;
}
.meta.svc.offcfg .svcdot {
  background: #e0a83c;
  box-shadow: 0 0 6px #e0a83c88;
}

.rec .recdot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  margin-right: 4px;
  background: #35d07f;
  box-shadow: 0 0 6px #35d07f88;
  animation: pulse 1.6s ease-in-out infinite;
}
.rec.off .recdot {
  background: #48526a;
  box-shadow: none;
  animation: none;
}
.rec.bad .recdot {
  background: #ef5350;
  box-shadow: none;
  animation: none;
}
.rec.bad {
  color: #ff8a80;
}
@keyframes pulse {
  50% {
    opacity: 0.35;
  }
}

.spacer {
  margin-left: auto;
}

/* 齿轮：贴在"服务运行中"左边。
   做成低调的图标按钮 —— 它平时不该抢视线，但悬停时要有明确反馈。 */
.gear {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 22px;
  height: 22px;
  padding: 0;
  border: 1px solid transparent;
  border-radius: 5px;
  background: transparent;
  color: #8b95ab;
  font-size: 14px;
  line-height: 1;
  cursor: pointer;
  transition: color 0.12s, background 0.12s, border-color 0.12s;
}
.gear:hover {
  color: #dfe6f5;
  background: #1c2436;
  border-color: #2a3550;
}
.gear:active {
  background: #232d44;
}

/* 设置弹窗 ---------------------------------------------------------------- */
.sheet-mask {
  position: fixed;
  inset: 0;
  z-index: 50;
  display: flex;
  align-items: center;
  justify-content: center;
  /* 半透明黑压住视频画面。preview 是 canvas，模糊滤镜在整屏尺寸上太贵，不用。 */
  background: #05070dcc;
}

.sheet {
  width: min(560px, calc(100vw - 48px));
  max-height: calc(100vh - 64px);
  display: flex;
  flex-direction: column;
  background: #12161f;
  border: 1px solid #26304a;
  border-radius: 10px;
  box-shadow: 0 18px 48px #00000099;
  color: #dfe6f5;
  font-size: 13px;
}

.sheet-head {
  display: flex;
  align-items: center;
  padding: 12px 16px;
  border-bottom: 1px solid #1e2636;
}
.sheet-head strong {
  font-size: 13px;
  letter-spacing: 0.02em;
}
.sheet-x {
  margin-left: auto;
  width: 24px;
  height: 24px;
  border: 0;
  border-radius: 5px;
  background: transparent;
  color: #8b95ab;
  font-size: 16px;
  line-height: 1;
  cursor: pointer;
}
.sheet-x:hover {
  background: #1c2436;
  color: #dfe6f5;
}

.sheet-body {
  padding: 16px;
  overflow: auto;
}

.row {
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.row-label {
  display: block;
  color: #cdd6e8;
}
.row-hint {
  display: block;
  margin-top: 4px;
  color: #7c869c;
  font-size: 11.5px;
  font-style: normal;
  line-height: 1.5;
}
.row-ctl {
  display: flex;
  align-items: center;
  gap: 12px;
}
.row-ctl input[type='range'] {
  flex: 1 1 auto;
  /* accent-color 让滑轨/滑块跟着主题走，省得自己写 webkit 伪元素。 */
  accent-color: #4a9eff;
  cursor: pointer;
}
.row-val {
  flex: 0 0 auto;
  min-width: 62px;
  text-align: right;
  color: #9fd0ff;
  font-variant-numeric: tabular-nums;
}


.sheet-note {
  margin: 14px 0 0;
  padding: 8px 10px;
  border-radius: 6px;
  background: #171d29;
  color: #7c869c;
  font-size: 11.5px;
  line-height: 1.6;
}

.sheet-foot {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 12px 16px;
  border-top: 1px solid #1e2636;
}
.sheet-ok {
  color: #6fd39a;
  font-size: 12px;
}
.sheet-err {
  color: #ef7f7c;
  font-size: 12px;
}
.sheet-btn {
  padding: 5px 14px;
  border: 1px solid #2a3550;
  border-radius: 5px;
  background: #1a2130;
  color: #cdd6e8;
  font-size: 12px;
  cursor: pointer;
}
.sheet-btn:hover:not(:disabled) {
  border-color: #3a4a6d;
}
.sheet-btn.primary {
  background: #1c4e80;
  border-color: #2b6cb0;
  color: #fff;
}
.sheet-btn.primary:hover:not(:disabled) {
  background: #215a94;
}
.sheet-btn:disabled {
  opacity: 0.5;
  cursor: default;
}

.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #48526a;
  flex: 0 0 auto;
}
.dot.ok {
  background: #35d07f;
  box-shadow: 0 0 8px #35d07f88;
}
.dot.wait {
  background: #e0a83c;
  box-shadow: 0 0 8px #e0a83c88;
}
.dot.bad {
  background: #ef5350;
}

.retry,
.export {
  padding: 4px 12px;
  border: 1px solid #2b6cb0;
  border-radius: 5px;
  background: #1c4e80;
  color: #fff;
  cursor: pointer;
  font-size: 12px;
}
.export {
  background: #17603c;
  border-color: #2f8f5b;
}
.export:disabled {
  background: #1a2030;
  border-color: #26304a;
  color: #5d677d;
  cursor: default;
}

.toast {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 5px 12px;
  background: #12331f;
  color: #9ae6b4;
  font-size: 12px;
  border-bottom: 1px solid #1d4a30;
  flex: 0 0 auto;
}
.toast.bad {
  background: #3a1616;
  color: #ffb3ad;
  border-bottom-color: #5a2222;
}
.toast-text {
  flex: 1 1 auto;
  word-break: break-all;
}
.toast-x {
  background: none;
  border: none;
  color: inherit;
  cursor: pointer;
  font-size: 15px;
  line-height: 1;
}

.body {
  display: flex;
  flex: 1 1 auto;
  min-height: 0;
}

.side {
  width: 320px;
  flex: 0 0 auto;
  display: flex;
  flex-direction: column;
  background: #0a0e16;
  border-right: 1px solid #1b2231;
  overflow-y: auto;
}

.side-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 10px;
  font-size: 11px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: #616d84;
  border-bottom: 1px solid #141a26;
}

.mini {
  padding: 2px 8px;
  border: 1px solid #26304a;
  border-radius: 4px;
  background: #131a28;
  color: #9fb0cc;
  cursor: pointer;
  font-size: 11px;
  text-transform: none;
}
.mini:disabled {
  opacity: 0.5;
  cursor: default;
}
.mini.wide {
  width: 100%;
  padding: 5px;
}

.empty {
  padding: 14px 10px;
  color: #5d677d;
  font-size: 12px;
}

.plist {
  list-style: none;
  margin: 0;
  padding: 0;
}

.pitem {
  display: flex;
  gap: 8px;
  padding: 8px 10px;
  border-bottom: 1px solid #10151f;
  cursor: pointer;
}
.pitem:hover {
  background: #0e1420;
}
.pitem.active {
  background: #101a2b;
  box-shadow: inset 2px 0 0 #2b6cb0;
}
.pitem.off .pname,
.pitem.off .pdesc {
  opacity: 0.45;
}

/* ---------------------------------------------------------------------------
   iPhone 那种滑动开关（.switch 包一个原生 checkbox）
   ---------------------------------------------------------------------------
   为什么还留着原生 input：键盘能聚焦、空格能切、读屏能念 aria-label。
   它被视觉隐藏（不是 display:none，那样会丢掉可聚焦性），轨道和圆钮另画。

   颜色刻意选 iOS 的 #34c759：这是用户点名要的那个样子。
   --------------------------------------------------------------------------- */
.switch {
  display: inline-flex;
  align-items: center;
  flex: 0 0 auto;
  cursor: pointer;
  /* 点击区域比轨道大一圈，免得对着 51×31 的轨道点不准 */
  padding: 3px;
}
.switch input {
  position: absolute;
  width: 1px;
  height: 1px;
  opacity: 0;
  margin: 0;
  pointer-events: none;
}
.switch-track {
  position: relative;
  display: block;
  width: 42px;
  height: 24px;
  border-radius: 12px;
  background: #39415a;
  transition: background 0.22s ease;
}
.switch-knob {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 20px;
  height: 20px;
  border-radius: 50%;
  background: #fff;
  box-shadow: 0 1px 3px #0006;
  transition:
    transform 0.22s cubic-bezier(0.4, 0, 0.2, 1),
    width 0.15s ease;
}
.switch input:checked + .switch-track {
  background: #34c759;
}
.switch input:checked + .switch-track .switch-knob {
  transform: translateX(18px);
}
/* 按下去的时候圆钮拉宽一点 —— iOS 上就是这个手感 */
.switch input:active + .switch-track .switch-knob {
  width: 24px;
}
.switch input:checked:active + .switch-track .switch-knob {
  transform: translateX(14px);
}
.switch input:focus-visible + .switch-track {
  box-shadow: 0 0 0 2px #2b6cb0;
}
/* 停用状态：整行已经压暗了，开关本身再淡一点，避免看着像"已启用" */
.pitem.off .switch-track {
  opacity: 0.6;
}

.pmeta {
  min-width: 0;
  flex: 1 1 auto;
}
.pname {
  font-size: 12.5px;
  font-weight: 600;
  display: flex;
  gap: 6px;
  align-items: baseline;
}
.pid {
  font-size: 10.5px;
  color: #4c5670;
  font-weight: 400;
}
.pdesc {
  font-size: 11.5px;
  color: #7c879b;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.plive {
  font-size: 11px;
  color: #5f6b82;
  font-variant-numeric: tabular-nums;
  margin-top: 2px;
}
.plive .recdot {
  display: inline-block;
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #48526a;
  margin-right: 4px;
}
.plive .recdot.on {
  background: #35d07f;
}

.editor {
  border-top: 1px solid #1b2231;
}

.kv {
  display: grid;
  grid-template-columns: 76px 1fr;
  gap: 3px 8px;
  margin: 0;
  padding: 8px 10px;
  font-size: 11.5px;
}
.kv dt {
  color: #616d84;
}
.kv dd {
  margin: 0;
  color: #c3cede;
  word-break: break-all;
  font-variant-numeric: tabular-nums;
}
.kv dd.path {
  color: #7c879b;
  font-family: Consolas, monospace;
  font-size: 11px;
}

.form {
  display: flex;
  flex-direction: column;
  gap: 7px;
  padding: 8px 10px 14px;
  border-top: 1px solid #141a26;
}
.form label {
  display: flex;
  flex-direction: column;
  gap: 3px;
  font-size: 11.5px;
  color: #7c879b;
}
.form label.row {
  flex-direction: row;
  align-items: center;
  gap: 6px;
  color: #c3cede;
}
.form input[type='text'],
.form input[type='number'],
.form select {
  padding: 4px 6px;
  border: 1px solid #26304a;
  border-radius: 4px;
  background: #0e1420;
  color: #e7ecf5;
  font: 12px/1.4 inherit;
}
.actions {
  display: flex;
  gap: 6px;
  margin-top: 2px;
}
.primary {
  flex: 1 1 auto;
  padding: 5px 10px;
  border: 1px solid #2b6cb0;
  border-radius: 4px;
  background: #1c4e80;
  color: #fff;
  cursor: pointer;
  font-size: 12px;
}
.primary:disabled {
  opacity: 0.6;
  cursor: default;
}

.wlist {
  list-style: none;
  margin: 0;
  padding: 0;
  max-height: 180px;
  overflow-y: auto;
  border: 1px solid #1b2231;
  border-radius: 4px;
}
.wlist li {
  padding: 4px 6px;
  cursor: pointer;
  border-bottom: 1px solid #10151f;
  font-size: 11px;
  display: flex;
  flex-direction: column;
}
.wlist li:hover {
  background: #101a2b;
}
.wlist li span {
  color: #616d84;
}

/* ---- 下拉 + 隔壁的刷新按钮 ------------------------------------------------ */
.pickrow {
  display: flex;
  align-items: center;
  gap: 5px;
}
.pickrow select {
  flex: 1 1 auto;
  min-width: 0; /* 让长标题截断而不是把按钮挤出面板 */
}
.iconbtn {
  flex: 0 0 auto;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 24px;
  padding: 0;
  border: 1px solid #26304a;
  border-radius: 4px;
  background: #0e1420;
  color: #8fa2bd;
  cursor: pointer;
}
.iconbtn:hover:not(:disabled) {
  border-color: #2b6cb0;
  color: #e7ecf5;
  background: #142033;
}
.iconbtn:disabled {
  opacity: 0.6;
  cursor: default;
}
/* 刷新中让图标转起来，用户能看出点到了。只在请求那一小段时间转 */
.iconbtn.spin svg {
  animation: iconspin 0.9s linear infinite;
}
@keyframes iconspin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}

/* ---- 匹配优先级（可拖动排序） -------------------------------------------- */
.prio {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.prio-head {
  display: flex;
  align-items: baseline;
  gap: 6px;
  font-size: 11.5px;
  color: #7c879b;
}
.prio-tip {
  font-size: 10.5px;
  color: #4f5a6e;
}
.prio-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 3px;
}
.prio-item {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 6px;
  border: 1px solid #1f2738;
  border-radius: 4px;
  background: #0e1420;
  color: #c3cede;
  font-size: 11.5px;
  cursor: grab;
  user-select: none;
}
.prio-item:hover {
  border-color: #2b3550;
}
/* 拖动中的那一行淡化，作为"它正在被搬走"的反馈。
   为什么不用半透明占位+插入线：只有三行，简单淡化已经够明白，
   多做的视觉反馈反而让这几行看起来很重。 */
.prio-item.dragging {
  opacity: 0.45;
  cursor: grabbing;
  border-style: dashed;
  border-color: #2b6cb0;
}
.prio-grip {
  color: #4f5a6e;
  font-size: 12px;
  line-height: 1;
}
.prio-rank {
  flex: 0 0 auto;
  min-width: 14px;
  text-align: center;
  color: #4f5a6e;
  font-variant-numeric: tabular-nums;
}
.prio-name {
  flex: 1 1 auto;
}
.prio-move {
  display: flex;
  gap: 2px;
}
.tiny {
  width: 18px;
  height: 18px;
  padding: 0;
  border: 1px solid #26304a;
  border-radius: 3px;
  background: #0b1018;
  color: #8fa2bd;
  font-size: 10px;
  line-height: 1;
  cursor: pointer;
}
.tiny:hover:not(:disabled) {
  border-color: #2b6cb0;
  color: #e7ecf5;
}
.tiny:disabled {
  opacity: 0.35;
  cursor: default;
}
.prio-hint {
  margin: 0;
  font-size: 10.5px;
  color: #616d84;
  word-break: break-all;
}

.bad {
  color: #ff8a80;
  font-size: 11.5px;
  margin: 0 10px 8px;
}
.hint {
  color: #616d84;
  font-size: 11px;
  margin: 0 10px 8px;
}

.stage {
  position: relative;
  flex: 1 1 auto;
  min-height: 0;
  display: flex;
}

.surface {
  width: 100%;
  height: 100%;
  display: block;
  background: #05070c;
}

.overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  pointer-events: none;
  color: #8f9bb1;
  text-align: center;
  padding: 24px;
}
.overlay .bad {
  color: #ff8a80;
  margin: 0;
}
.overlay .detail {
  margin: 0;
  max-width: 640px;
  white-space: pre-wrap;
  word-break: break-word;
  font: 12px/1.6 Consolas, 'Cascadia Mono', monospace;
  color: #5d677d;
}

.overlay-actions {
  position: absolute;
  bottom: 22px;
  left: 50%;
  transform: translateX(-50%);
  width: 180px;
}
.overlay-actions .primary {
  width: 100%;
}

/* ---- 无信号画面 ----------------------------------------------------------
   照老电视那套：深色底 + 斜滚动的粗条纹 + 一层扫描线 + 中间一块标题。
   全部用 CSS 画，不引图片：动画只动 transform / background-position / opacity，
   不会因为画一张 1440p 的背景图把预览的帧率拖下去。 */
.nosignal {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  /* 底色必须是不透明的：这层是盖在 canvas 上的，要是半透明，
     上一路残留的画面会从条纹缝里透出来，看着像花屏。 */
  background:
    radial-gradient(ellipse 70% 60% at 50% 44%, rgba(27, 41, 64, 0.55), transparent 72%),
    #04060a;
  pointer-events: none;
}

/* 斜条纹：一组粗灰条整体斜着飘，比"竖条各自上下"更像雪花信号 */
.ns-bars {
  position: absolute;
  inset: -40%;
  display: flex;
  gap: 58px;
  opacity: 0.19;
  transform: rotate(-18deg);
  filter: blur(11px);
}
.ns-bars span {
  flex: 1 1 0;
  min-width: 44px;
  background: linear-gradient(
    180deg,
    rgba(255, 255, 255, 0) 0%,
    rgba(226, 240, 255, 0.78) 32%,
    rgba(205, 228, 255, 0.85) 50%,
    rgba(150, 190, 240, 0.42) 68%,
    rgba(255, 255, 255, 0) 100%
  );
  /* --i 是序号，让每条纹的起点错开，不会齐步走 */
  animation: ns-drift 4.6s ease-in-out infinite;
  animation-delay: calc(var(--i) * -0.47s);
}
@keyframes ns-drift {
  0% { transform: translateY(-46%); opacity: 0.12; }
  50% { transform: translateY(46%); opacity: 1; }
  100% { transform: translateY(-46%); opacity: 0.12; }
}

/* 扫描线：每 3px 一道暗线，叠在条纹之上，是最能"一眼认出老电视"的那层 */
.nosignal::before {
  content: '';
  position: absolute;
  inset: 0;
  background: repeating-linear-gradient(
    180deg,
    rgba(0, 0, 0, 0.34) 0px,
    rgba(0, 0, 0, 0.34) 1px,
    transparent 1px,
    transparent 3px
  );
  opacity: 0.55;
}

/* 扫过整屏的一道亮带，慢一点，制造"信号在找台"的感觉 */
.nosignal::after {
  content: '';
  position: absolute;
  inset: 0;
  background: linear-gradient(
    180deg,
    transparent 0%,
    rgba(205, 230, 255, 0.085) 46%,
    transparent 100%
  );
  background-size: 100% 42%;
  background-repeat: no-repeat;
  animation: ns-sweep 3.2s linear infinite;
}
@keyframes ns-sweep {
  0% { background-position: 0 -42%; }
  100% { background-position: 0 142%; }
}

.ns-card {
  position: relative;
  text-align: center;
  max-width: min(600px, 84%);
  padding: 30px 60px 28px;
  border-radius: 16px;
  background: linear-gradient(180deg, rgba(14, 21, 34, 0.82), rgba(7, 11, 18, 0.88));
  border: 1px solid rgba(125, 165, 220, 0.2);
  box-shadow:
    0 20px 60px rgba(0, 0, 0, 0.5),
    inset 0 1px 0 rgba(180, 210, 245, 0.08);
}

.ns-title {
  margin: 0;
  font: 700 clamp(22px, 4.2vw, 42px)/1.1 'Segoe UI', system-ui, sans-serif;
  letter-spacing: 0.24em;
  /* 字距会把最后一个字母往右顶一截，补回来让它视觉居中 */
  text-indent: 0.24em;
  color: #d6e6fb;
  text-shadow:
    0 0 30px rgba(120, 175, 255, 0.42),
    0 0 2px rgba(200, 225, 255, 0.7);
  /* 老电视那种轻微抖动，幅度压得很小，不然看久了眼睛累 */
  animation: ns-flicker 5.2s steps(1, end) infinite;
}
@keyframes ns-flicker {
  0%, 92%, 100% { opacity: 1; }
  94% { opacity: 0.68; }
  95% { opacity: 1; }
  97% { opacity: 0.86; }
  98% { opacity: 1; }
}

/* 系统里关了动画就只留静态一帧，别硬撑着刷 */
@media (prefers-reduced-motion: reduce) {
  .ns-bars span,
  .nosignal::after,
  .ns-title {
    animation: none;
  }
}
</style>
