<template>
  <div class="app">
    <!-- 顶栏：只留状态，不放日志 -->
    <header class="bar">
      <!-- 录制状态：只留一个圆点 + 是否在录。分片数、磁盘占用这些是给排查用的
           内部数字，对用户没有意义，所以不再展示（需要时看服务日志或导出按钮）。 -->
      <span
        class="meta rec"
        :class="{ bad: activeLive && activeLive.recordFailed, off: !activeLive || !activeLive.recording }"
        :title="recordTitle"
      >
        <span class="recdot" />
        {{ activeLive && activeLive.recording ? '录制中' : activeLive && activeLive.recordFailed ? '录制失败' : '未录制' }}
      </span>

      <span v-if="running" class="meta">
        <b>{{ fpsShown }}</b> fps
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
      </span>
      <span v-else class="meta warn" :title="serviceStatusTitle">服务未启动</span>

      <button
        class="export"
        :disabled="!canExport"
        :title="exportTitle"
        @click="doExport"
      >
        {{ exportButtonText }}
      </button>

      <button v-if="fatal" class="retry" @click="retry">重试连接</button>
    </header>

    <!-- 导出结果/失败的提示条 -->
    <div v-if="exportNotice" class="toast" :class="{ bad: exportNotice.bad }">
      <span class="toast-text">{{ exportNotice.text }}</span>
      <button class="toast-x" @click="dismissExportNotice">×</button>
    </div>

    <!-- 设置弹窗 ------------------------------------------------------------
         右上角 × 关闭；Esc 也能关（见 settingsKeydown）。点击框外不关闭（防误触）。
         刻意做成"覆盖层"而不是新窗口：新开 BrowserWindow 要另写一份 preload
         和 IPC，为一个滑杆不值当。 -->
    <div v-if="settingsOpen" class="sheet-mask">
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

          <label class="row row-inline">
            <span class="row-label">点击关闭窗口时的行为</span>
            <span class="row-ctl">
              <select
                :value="closeBehaviorDraft ?? 'ask'"
                :disabled="settingsSaving"
                class="row-select"
                @change="onCloseBehaviorChange"
              >
                <option value="ask">每次询问</option>
                <option value="minimize">最小化到托盘</option>
                <option value="quit">退出程序</option>
              </select>
            </span>
          </label>

          <!-- 「从上次保存处继续」：改变"保存"的内容范围（录制照常滚动，不受影响）。
               开关样式复用侧栏那套 CSS 画出来的滑块，两处长得一样用户才不会愣。 -->
          <div class="row">
            <span class="row-label">
              从上次保存处继续
              <em class="row-hint">
                打开后，每次保存都会把那一刻当作新的起点：下次保存只包含这之后录到的画面。
              </em>
            </span>
            <span class="row-ctl">
              <label class="switch" title="从上次保存处继续">
                <input
                  type="checkbox"
                  :checked="continueAfterSaveDraft"
                  :disabled="settingsSaving"
                  @change="onContinueChange"
                />
                <span class="switch-track"><span class="switch-knob" /></span>
              </label>
            </span>
          </div>

          <!-- 重启服务：改了 native 代码、或者服务状态不对劲时用它。
               分片留在磁盘上不会丢，重启后会接着录（回放窗口是滚动覆盖的）。
               守护进程会把服务拉回来，所以就算重启失败也不用手动去命令行起。 -->
          <div class="row danger-row">
            <span class="row-label">
              重启采集服务
              <em class="row-hint">当前服务会停掉再重新拉起，录制短暂中断</em>
            </span>
            <span class="row-ctl">
              <button class="sheet-btn danger" :disabled="restarting" @click="restartService">
                {{ restarting ? '重启中…' : '重启服务' }}
              </button>
            </span>
          </div>
          <p v-if="restartNotice" class="hint">{{ restartNotice }}</p>

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
          <!-- 编辑在左、新建在右。两个动作挨着，把"会多出一路"的新建
               放远一点，手滑点到的不会是它 -->
          <span class="head-btns">
            <button class="mini" :disabled="!selectedProfile || saving" @click="startEdit()">
              <svg class="pencil" viewBox="0 0 16 16" aria-hidden="true">
                <path
                  d="M12.146.146a.5.5 0 0 1 .708 0l3 3a.5.5 0 0 1 0 .708l-10 10a.5.5 0 0 1-.168.11l-5 2a.5.5 0 0 1-.65-.65l2-5a.5.5 0 0 1 .11-.168l10-10zM11.207 2.5 13.5 4.793 12.5 5.793 10.207 3.5l1-1zm1.586 3L10.5 3.207 4 9.707V10h.5a.5.5 0 0 1 .5.5v.5h.5a.5.5 0 0 1 .5.5v.5h.293l6.5-6.5zm-9.761 5.175-.106.106-1.528 3.821 3.821-1.528.106-.106A.5.5 0 0 1 5 12.5V12h-.5a.5.5 0 0 1-.5-.5V11h-.5a.5.5 0 0 1-.468-.325z"
                  fill="currentColor"
                />
              </svg>
              {{ editing ? '编辑中' : '编辑' }}
            </button>
            <button class="mini" :disabled="saving" @click="addProfile">＋ 新建</button>
          </span>
        </div>

        <!-- 配置列表单独一层：它自己滚，"当前配置"是常驻信息，
             不该跟着列表一起被滚出去 -->
        <div class="plist-scroll">
          <div v-if="!profiles.length" class="empty">
            还没有配置。点"＋ 新建"加一路。
          </div>

          <ul class="plist" @dragover.prevent @drop.prevent="onDrop">
            <li
              v-for="p in profiles"
              :key="p.id"
              class="pitem"
              :class="{
                active: p.id === selectedId,
                off: !p.enabled,
                dragging: dragId === p.id,
                'drop-on': dragOverId === p.id && dragId !== null && dragId !== p.id,
              }"
              draggable="true"
              @click="select(p.id)"
              @dragstart="onDragStart(p.id, $event)"
              @dragend="onDragEnd"
              @dragover="onDragOver(p.id, $event)"
              @dragleave="onDragLeave(p.id)"
              @drop.prevent="onDrop"
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
        </div>

        <!-- 选中配置的详情：钉在侧栏最底下，不跟着上面的列表滚 -->
        <div v-if="selectedProfile" class="cur">
          <div class="side-head">
            <span>当前配置</span>
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

        </div>
      </aside>

      <!-- 编辑弹窗：参数单独一个窗口调。侧栏只留列表和"当前配置"，
           列表一多也不会把编辑表单顶到视野外 -->
      <div v-if="editing && (selectedProfile || editingNew)" class="sheet-mask editor-mask">
        <div class="sheet editor-sheet" role="dialog" aria-modal="true">
          <div class="sheet-head">
            <strong>{{ editingNew ? '新建配置' : '编辑配置' }}</strong>
            <span class="sheet-sub">{{ editingNew ? '还没加进列表' : selectedProfile?.name || selectedProfile?.id }}</span>
            <button class="sheet-x" title="关闭" @click="cancelEdit">×</button>
          </div>
          <div class="sheet-body">
            <form class="form in-modal" @submit.prevent="saveEdits">
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
                      <!-- 服务手里握着的那个窗口如果已经不在枚举列表里（关掉重开换了
                           hwnd / 被最小化），单独补一行，别让下拉只剩"请选择"。 -->
                      <option v-if="capturedOption" :value="capturedOption.hwnd">
                        当前正在录制：{{ capturedOption.title }}（已不在窗口列表中）
                      </option>
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

                <!-- 匹配方式：下拉选四个组合，**应用程序（exe）永远是必要条件**。
                     以前的"拖动排优先级"整个删掉了 —— 它是"标题对不上就降级到
                     类名，于是录到同类的另一个窗口"的根源。现在没有降级：
                     条件全部 AND，少一条就不匹配，这一路保持未采集。 -->
                <label>
                  匹配方式
                  <select v-model="draft.matchRule">
                    <option v-for="o in MATCH_RULE_OPTIONS" :key="o.value" :value="o.value">
                      {{ o.label }}
                    </option>
                  </select>
                </label>

                <!-- 当前这一档具体在比什么，逐条列出来 -->
                <p v-if="matchRuleHint" class="rule-hint">{{ matchRuleHint }}</p>

                <!-- 「列表里没有窗口满足条件」不再额外警告：这一路会保持未采集，
                     下拉里就停在「选择一个窗口」，用户看得见，不需要一句话再说一遍。 -->

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
                  {{ saving ? '保存中…' : editingNew ? '确定' : '保存' }}
                </button>
                <button type="button" class="mini" @click="cancelEdit">取消</button>
                <!-- 删除：删的是**这一路配置**，已录好的分片不动（那是用户的素材）。
                     放在最右边，跟保存/取消拉开距离，免得手滑点到。
                     新建模式没有这一路可删，不显示。 -->
                <button
                  v-if="!editingNew"
                  type="button"
                  class="mini danger"
                  :disabled="deleting"
                  :title="`删除配置「${selectedProfile?.name || selectedProfile?.id}」`"
                  @click="deleteProfile"
                >
                  {{ deleting ? '删除中…' : '删除' }}
                </button>
              </div>
              <p v-if="saveError" class="bad">{{ saveError }}</p>
              <p v-if="saveNotice" class="hint">{{ saveNotice }}</p>
            </form>
          </div>
        </div>
      </div>

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
// 只借它的类型：`recordActualText()` 的入参要跟服务状态那张表对上。
import type { SessionLive } from '@/lib/serviceBus'

const { canvas, running, fatal, waitingService, serviceHint, stats, start, stop, retry, switchSession, fpsOf, seedLedgerFromService, reportStats, setupHistory, getHistory } =
  usePreview()

const { live, lastExport, exportStates } = useServiceBus()

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
    // 「立刻切片」的完成计数（服务端 Session::flushSeq）。
    //
    // 【为什么自检要盯这个】点导出前必须先让服务把正在写的那一片收尾落盘，
    // 否则那一片（最多 2 秒，含"点按钮那一刻"的画面）导不出来 —— 用户报的
    // 「导出的视频都是点击前一秒或两秒就停止了」（m12714）就是这个。
    // 主进程发完 flush 命令后要等这个数变了才能启动导出，自检脚本同样要能读到它。
    selectedFlushSeq: s?.flushSeq ?? 0,
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
  /** 最小化。平时枚举会把它排掉，但正在被录的那个会被强制收录（--include-hwnd）。 */
  minimized?: boolean
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
    /**
     * 匹配方式（编辑面板里那个下拉），四个取值见 MATCH_RULE_OPTIONS。
     *
     * 服务端（Profiles.h 的 MatchRule）按这个值决定"同时满足哪几个字段"，
     * **应用程序（exe）永远是必要条件**，其余字段是叠加条件，全部 AND。
     * 老配置没有这个键，读取时由 `deriveMatchRule()` 从 by/byOrder 推断。
     */
    rule?: string
    by: string
    /**
     * 老模型的产物：按优先级降级的字段顺序（界面以前可以拖动改）。
     *
     * 现在**不再参与匹配** —— 降级正是"标题对不上却录了别的窗口"的根源。
     * 只在读老配置时用来推断 matchRule，保存时不再写。
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
    /**
     * 重新枚举窗口的间隔（毫秒）。
     *
     * C++ 侧 `WindowMatch::pollMs` 一直有这个字段、服务端也真的按它轮询
     * （`Profiles.h` 的 `WindowMatch` 结构体会读它），但界面类型里长期漏写，
     * 于是新建配置（本文件的 `newProfile()`）和编辑页的草稿都只能靠对象字面量
     * 绕过类型检查 —— vue-tsc 会报 TS2353。这里补全，保存时原样带给服务端。
     */
    pollMs?: number
    /** 从下拉里选窗口时记下的三份指纹，按匹配方式组合使用。 */
    title?: string
    className?: string
    process?: string
    /**
     * 用户挑中的那个窗口（hwnd，无符号 32 位；0 = 没挑过）。
     *
     * 服务端（WindowMatch::preferredHwnd）只在**已经满足 rule 的候选**里优先
     * 它 —— 是偏好不是条件。"只匹配应用程序"下同一程序的多个窗口都命中，
     * 没有这一项就永远只会录面积最大的那个，换另一个同程序的窗口切不过去。
     */
    hwnd?: number
  }
  createdAt?: string
}

const api = () => (window as unknown as { shadowPreview?: Record<string, unknown> }).shadowPreview

const profiles = ref<Profile[]>([])
const recordRoot = ref('')
const exportDir = ref('')
const selectedId = ref<string | null>(null)
const editing = ref(false)
/** 新建模式：弹窗里编的是一路还没加进列表的新配置，点确定才入列 */
const editingNew = ref(false)
const saving = ref(false)
/** 正在删配置（删除按钮的禁用/文案） */
const deleting = ref(false)
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
  // 选中的配置被左侧开关关掉了：直接盖 NO SIGNAL，不等服务端把 state 切到
  // disabled。否则服务端回执之前的几百毫秒里画面会定格在最后一帧，像卡死。
  if (selectedProfile.value && !selectedProfile.value.enabled) {
    return { title: '当前配置没有画面', detail: '该配置已停用，打开左侧开关即恢复录制。' }
  }
  const l = activeLive.value
  if (!l) return null
  // 服务不在报状态时（崩了 / 卡住 / 被杀了），手里这份 live 是**过期**的：
  // 下面几行看到的 state 永远是它最后说的那个 live，画面就停在那儿，
  // NO SIGNAL 永远盖不上来 —— 和"窗口关了但状态还是 live"是同一个观感。
  // 所以先确认服务还活着，再去相信它说的状态。
  if (!live.value.running) {
    return {
      title: '采集服务没有响应',
      detail: '最后一份状态已经过期，预览停在最后一帧。服务恢复后会自动接上，不用手动刷新。',
    }
  }
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

/**
 * 当前选中这一路的导出状态（null = 空闲）。
 *
 * **按 id 取**，不是全局一份：导出现在走队列，A 路在导的时候 B 路照样能点，
 * 只是排在后面。按钮的禁用因此只看"我这一路"有没有在排队/在跑。
 */
const selectedExport = computed(() =>
  selectedId.value ? (exportStates.value[selectedId.value] ?? null) : null,
)

/**
 * 导出按钮上那行字：空闲 / 导出中。
 *
 * 队列对用户是不可见的：点了就是"导出中…"，不管现在是真在导还是排在前一路
 * 后面。把"排队中"露出来只会让人以为卡住了、或者以为自己点错了地方。
 */
const exportButtonText = computed(() => (selectedExport.value ? '保存中…' : '保存'))

const canExport = computed(() => {
  const s = activeLive.value
  if (!s) return false
  // 这一路已经在队列里（或正在跑）就不能再点：主进程那边也会拒绝重复入队，
  // 但按钮先灰掉更直观。
  if (selectedExport.value) return false
  // s.exportRunning 是**服务**侧那条老通道，服务不在跑时永远是 false；
  // 界面自己的队列状态才是现在这条路的判据。
  return s.recordSegments > 0
})

const exportTitle = computed(() => {
  const s = activeLive.value
  if (!s) return '先选一路配置'
  const st = selectedExport.value
  // 排队与在跑都叫"导出中"：队列是实现细节，界面上不区分（见 exportButtonText）。
  if (st) return `正在导出：${st.state === 'running' ? st.phase || '导出中' : '导出中'}`
  if (!s.recordSegments) return '还没有可导的分片（服务没在录，或界面读不到服务状态）'
  return `把最近 ${selectedProfile.value?.video.replaySeconds ?? 0} 秒合并成一个 MP4`
})

const recordTitle = computed(() => {
  const s = activeLive.value
  if (!s) return ''
  if (s.recordFailed) return s.recordDetail || '录制失败'
  if (!s.recording) return '未在录制'
  // 配了 240 帧但硬件只吃到 165 帧时，别只写一句"正在录制" —— 用户会以为在录 240。
  const real = recordActualText(s)
  const head = real ? `正在录制（${real}）` : '正在录制'
  return `${head}，内容会在回放窗口内滚动覆盖`
})

/**
 * 「实际在录多少帧 / 多少码率」的人话，和配置值**不一致时**才返回内容。
 *
 * 服务里 `effectiveFps` 是编码器降档后的真实帧率，`requestedFps` 是配置值。
 * 两者相等（占绝大多数情况）就不啰嗦，只在降档时提示 —— 否则顶栏每天都挂着
 * 一行"60 帧 / 20 Mbps"，纯噪音。
 *
 * 参考 m13697：「为什么切片里面的视频帧率是 165fps？谁让你这么干的」——
 * 用户不是不接受降档，是不能接受**不被告知**。
 */
function recordActualText(s: SessionLive | null): string {
  if (!s) return ''
  const parts: string[] = []
  const rf = s.requestedFps ?? 0
  const ef = s.effectiveFps ?? 0
  if (ef > 0 && rf > 0 && ef !== rf) {
    parts.push(`硬件最高 ${ef} 帧，配置的 ${rf} 帧已自动降档`)
  } else if (ef > 0) {
    parts.push(`${ef} 帧`)
  }
  const rb = s.requestedBitrateMbps ?? 0
  const eb = s.effectiveBitrateMbps ?? 0
  if (eb > 0 && rb > 0 && eb !== rb) {
    parts.push(`码率实际 ${eb} Mbps（配置 ${rb}）`)
  }
  return parts.join('，')
}

/**
 * 顶栏那枚"服务运行中 / 服务未启动"的悬停说明。
 *
 * 要写清**为什么**是运行中或不是：这两条通道（stdout / 状态文件）是这次修的
 * 重点，出问题时用户第一眼就该看出是"服务真没起"还是"状态没接上"。
 */
// 守护进程的状态**不在界面上显示**：它是服务端的实现细节（服务拉守护、守护盯
// 服务，两条边都在服务进程那一侧），用户既不能操作它，也不用关心它在不在岗。
// 以前顶栏挂着一枚"守护在岗"的徽标，纯噪音，已经删掉。

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
      return `服务在运行（不是本界面启动的，${age}更新）\n${file}`
    }
    if (!l.owned) return `服务在运行（外部启动）\n${file}`
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
    getSettings?: () => Promise<{ ok: boolean; settings?: { previewMaxFps: number; closeBehavior?: 'minimize' | 'quit' | null } }>
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
      const cb = res?.settings?.closeBehavior
      if (cb === 'minimize' || cb === 'quit') closeBehavior.value = cb
      else closeBehavior.value = null
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
    // `steps[i]` 在 noUncheckedIndexedAccess 下是 `number | undefined`，
    // 下标循环里用 `?? 0` 兜一下即可（循环边界已经保证 i < length）。
    if ((steps[i] ?? 0) <= v) best = i
    else break
  }
  return best
})

/** 弹窗里那份草稿值：只有点"保存"才生效，中途取消不影响正在跑的预览。 */
const previewMaxFpsDraft = ref(60)

/**
 * 点窗口「×」时的行为：'minimize'（最小化到托盘）| 'quit'（退出程序）。
 * null = 还没选过，第一次叉掉会弹窗问一次（带「下次不再提示」）。
 */
const closeBehavior = ref<'minimize' | 'quit' | null>(null)
const closeBehaviorDraft = ref<'minimize' | 'quit' | null>(null)

/**
 * 「从上次保存处继续」。
 *
 * 打开后每次成功保存都会把那一刻记成新起点：下次保存只包含这之后录到的
 * 画面，也不再补 NO SIGNAL —— 录到多少就存多少，不足设定的回放时长就短一些。
 * 关闭时（默认）每次保存都是完整的回放窗口。
 */
const continueAfterSave = ref(false)
const continueAfterSaveDraft = ref(false)

/**
 * 帧率硬上限：120fps。录制与预览共用这一个数，后端
 * `MfEncoder::kMaxFps` 也是 120（老配置里手写的 240 会被它夹回来）。
 *
 * 为什么不给 144/165/240：1440p 硬件编码器物理上限 172fps（240 从来录不到），
 * 165 那档每帧成本已经顶破帧预算、真实画面只剩一半；120 及以下是实测的
 * 稳定区间（真实画面 89~93%、落盘 100%）。见 FPS_CHOICES 的注释。
 */
const MAX_FPS = 120

/** 把滑杆/输入来的值夹到合法范围（0..120 的整数；上限与录制侧一致）。 */
function normaliseFps(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n)) return 0
  return Math.max(0, Math.min(MAX_FPS, Math.round(n)))
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

/** 关闭行为下拉：'ask' 只存在于界面（= 还没选过），落盘前转成 null。 */
function onCloseBehaviorChange(e: Event) {
  const el = e.target as HTMLSelectElement | null
  if (!el) return
  const v = el.value
  closeBehaviorDraft.value = v === 'ask' ? null : (v as 'minimize' | 'quit')
}

/** 「从上次保存处继续」开关：checkbox 的勾选状态进草稿（点确认才落盘）。 */
function onContinueChange(e: Event) {
  const el = e.target as HTMLInputElement | null
  if (!el) return
  continueAfterSaveDraft.value = el.checked
}

async function openSettings() {
  settingsError.value = ''
  previewMaxFpsDraft.value = previewMaxFps.value
  closeBehaviorDraft.value = closeBehavior.value
  continueAfterSaveDraft.value = continueAfterSave.value
  settingsOpen.value = true
  // 打开时再问一次主进程：设置文件可能被手动改过，也可能主进程这边还记着
  // 一个界面没收到过的值。
  try {
    const res = (await (api() as { getSettings?: () => Promise<{ ok: boolean; settings?: { previewMaxFps: number; closeBehavior?: 'minimize' | 'quit' | null; continueAfterSave?: boolean } }> })
      ?.getSettings?.()) ?? null
    const fps = res?.settings?.previewMaxFps
    if (typeof fps === 'number') {
      previewMaxFps.value = fps
      previewMaxFpsDraft.value = fps
    }
    const cb = res?.settings?.closeBehavior
    if (cb === 'minimize' || cb === 'quit') {
      closeBehavior.value = cb
      closeBehaviorDraft.value = cb
    } else {
      closeBehavior.value = null
      closeBehaviorDraft.value = null
    }
    continueAfterSave.value = res?.settings?.continueAfterSave === true
    continueAfterSaveDraft.value = continueAfterSave.value
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
      api() as { setSettings?: (p: { previewMaxFps: number; closeBehavior: 'minimize' | 'quit' | null; continueAfterSave: boolean }) => Promise<{ ok: boolean; error?: string; applied?: boolean }> }
    )?.setSettings?.({ previewMaxFps: fps, closeBehavior: closeBehaviorDraft.value, continueAfterSave: continueAfterSaveDraft.value })) ?? null
    if (!res || !res.ok) {
      settingsError.value = res?.error ?? '保存失败：界面版本和服务对不上。'
      return false
    }
    previewMaxFps.value = fps
    closeBehavior.value = closeBehaviorDraft.value
    continueAfterSave.value = continueAfterSaveDraft.value
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

// ---- 重启采集服务 ---------------------------------------------------------
//
// 什么时候用：重新编译过 native（改了 C++ 之后要让新 exe 生效）、或者服务
// 状态看着不对劲（某一路卡在 starting、录制不动了）又不想去命令行敲命令。
//
// 录制会短暂中断（几秒），但**分片不丢**：它们已经写在磁盘上，服务起来之后
// 回放窗口继续滚动覆盖。真要是重启失败，守护进程几秒内也会把它拉回来 ——
// 所以这里不需要"重启失败请手动启动"这类兜底提示。
const restarting = ref(false)
const restartNotice = ref('')

async function restartService() {
  if (restarting.value) return
  restarting.value = true
  restartNotice.value = ''
  try {
    const res = (await (
      api() as { restartService?: () => Promise<{ ok: boolean; error?: string; pid?: number | null }> }
    )?.restartService?.()) ?? null
    if (!res) {
      restartNotice.value = '这台界面不支持重启服务（主进程没有这个接口）。'
      return
    }
    if (!res.ok) {
      restartNotice.value = res.error ?? '重启失败。'
      return
    }
    restartNotice.value = res.pid ? `已重启采集服务（新 pid ${res.pid}）。` : '已重启采集服务。'
  } catch (err) {
    restartNotice.value = `重启出错：${(err as Error).message}`
  } finally {
    restarting.value = false
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
    | ((
        id: string,
        seconds?: number,
      ) => Promise<{ ok: boolean; error?: string; queued?: boolean; ahead?: number; position?: number; alreadyQueued?: boolean }>)
    | undefined
  if (typeof fn !== 'function') {
    exportNotice.value = { text: 'preload 没有导出接口，界面版本和服务对不上。', bad: true }
    return { ok: false, error: 'preload 没有导出接口' }
  }
  const name = s.name || s.id
  const res = (await fn(s.id, selectedProfile.value?.video.replaySeconds)) ?? { ok: false }
  if (!res.ok) {
    exportNotice.value = { text: res.error ?? '导出命令没能发出去。', bad: true }
    return { ...res, profileId: s.id }
  }
  // 点一次就开始导：队列对用户不可见（见 exportButtonText），所以这里也不说
  // "排在第几个"。真正的进度/结果全部走 export_status 事件。
  if (res.alreadyQueued) {
    exportNotice.value = { text: `「${name}」正在导出，导完会告诉你。`, bad: false }
  } else {
    exportNotice.value = { text: `正在导出「${name}」最近 ${selectedProfile.value?.video.replaySeconds} 秒…`, bad: false }
  }
  return { ...res, profileId: s.id, replaySeconds: selectedProfile.value?.video.replaySeconds ?? null }
}

// 导出结束由主进程广播的 export_status 通知（见 app/main.cjs 的 runExportJob）。
// 不用轮询，也不依赖服务 —— 导出跑在界面进程自己的插件里。
// 队列里可能排着好几路，所以提示里带上配置名，用户才知道是哪一路导完了。
watch(lastExport, (evt) => {
  if (!evt) return
  if (evt.running) return
  const who = evt.name || evt.id ? `「${evt.name || evt.id}」` : ''
  if (evt.failed) {
    exportNotice.value = { text: `${who}导出失败：${evt.detail || '原因不明'}`, bad: true }
  } else if (evt.ok) {
    const mb = ((evt.bytes ?? 0) / 1024 / 1024).toFixed(1)
    exportNotice.value = {
      text: `${who}导出完成：${evt.outPath}（${(evt.durationSeconds ?? 0).toFixed(1)} 秒 / ${mb} MB）`,
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
    match: {
      rule: 'title+process',
      by: 'title',
      mode: 'contains',
      pattern: '',
      pick: 'largest',
      pollMs: 1000,
      requireVisible: true,
    },
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

/**
 * 新建一路 —— **先不进列表**。
 *
 * 用户点「＋ 新建」只打开一个空白的编辑弹窗，填好点「确定」才真正加进
 * 列表并落盘。中途取消 / 关窗 / Esc，列表纹丝不动，也不会留下磁盘上
 * 根本不存在的幽灵行（上一版的 draft 标记方案就是因为"先加进列表"才
 * 需要那套复杂的草稿回收逻辑，现在从根上就不加）。
 */
function addProfile() {
  editingNew.value = true
  editing.value = true
  const p = emptyProfile()
  draft.value = {
    name: p.name,
    enabled: p.enabled,
    sourceKind: p.source.kind,
    monitorIndex: p.source.monitorIndex,
    matchRule: deriveMatchRule(p.match),
    matchTitle: p.match?.title ?? '',
    matchClass: p.match?.className ?? '',
    matchProcess: p.match?.process ?? '',
    matchHwnd: Number(p.match?.hwnd ?? 0) || 0,
    matchMode: p.match?.mode ?? 'contains',
    matchPattern: p.match?.pattern ?? '',
    fps: p.video.fps,
    bitrateMbps: p.video.bitrateMbps,
    replaySeconds: p.video.replaySeconds,
  }
  extraFps.value = null
  saveError.value = ''
  saveNotice.value = ''
  // 新建默认是窗口捕获还是显示器捕获由 emptyProfile 定（默认窗口源），
  // 打开弹窗就先把对应列表拉一遍，下拉框是空的用户没法选。
  if (p.source.kind !== 'window') void loadMonitors()
  if (p.source.kind === 'window') void loadWindows()
}

/**
 * 「录制帧率上限」可选值。
 *
 * 录制帧率 = 采集器实际的抓帧节奏（定时间隔 = 1000/fps ms），
 * **和界面预览的帧率没有任何关系** —— 预览是另一条路，有它自己的上限。
 *
 * 【上限就是 120，不再有 144/165/240】主人拍板：「直接把 120 以上的帧率
 * 全部删除，不包括 120fps」。理由不是省事，是 120 以上每一档都实测负收益：
 *   * 1440p 的硬件 H.264 编码器物理上限 172fps，240 从来录不到；
 *   * 165 那档每帧成本 6.09ms 已经顶破 6.06ms 的帧预算，真实画面只剩一半
 *     （喂帧端只能补重复帧）；
 *   * 120 及以下：真实画面 89~93%、落盘 100%、PTS 零空洞 —— 稳定区间。
 * 与之配套，后端 `MfEncoder::kMaxFps = 120` 会把老配置里手写的 240 夹回来。
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
const FPS_CHOICES = [5, 15, 30, 60, 90, 120]

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
   * 匹配方式：四个取值之一，见 MATCH_RULE_OPTIONS。
   *
   * 【为什么不再是"有序字段列表"】以前这里是 matchOrder（标题 › 类名 › exe），
   * 服务按序降级。问题是降级会**悄悄换源**：标题一变就退到类名，而
   * Chrome_WidgetWin_1 这种类名满机都是，于是录到了另一个窗口。用户要求
   * 「匹配不到就不要随便选一个」，所以改成固定组合、全部 AND。
   *
   * 默认「名字和应用程序」：同一程序开多个窗口时，只有标题能区分它们。
   */
  matchRule: 'title+process' as string,
  /**
   * 从下拉里选窗口时记下的三份指纹。按 matchRule 决定用其中几份，全部 AND。
   */
  matchTitle: '',
  matchClass: '',
  matchProcess: '',
  /**
   * 用户**明确挑过**的那个窗口（hwnd，0 = 没挑过）。
   *
   * 「只匹配应用程序」这一档里，同一程序的多个窗口全都满足规则，只看指纹
   * 根本分不出用户到底想要哪一个：下拉会永远停在第一个命中的窗口上，看起来
   * 就是"换一个同程序的窗口切不过去"。所以单独记下挑中的 hwnd，一起存进
   * 配置（match.hwnd）—— 服务端把它当**偏好**用：只在已经满足规则的候选里
   * 优先它，它关掉了就退回同程序的另一个窗口。
   */
  matchHwnd: 0,
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
  draft.value = {
    name: p.name,
    enabled: p.enabled,
    sourceKind: p.source.kind,
    monitorIndex: p.source.monitorIndex,
    // 老配置没有 rule 键，按它当年排的首选字段推断一档（和服务端
    // WindowMatch::Normalize 的推断规则保持一致，否则界面显示的和服务实际
    // 在用的会是两回事）。
    matchRule: deriveMatchRule(p.match),
    matchTitle: p.match?.title ?? '',
    matchClass: p.match?.className ?? '',
    matchProcess: p.match?.process ?? '',
    matchHwnd: Number(p.match?.hwnd ?? 0) || 0,
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
  saveNotice.value = ''
  editing.value = true
  editingNew.value = false
  // 打开编辑面板就把当前显示器和窗口列表拉一遍，下拉框是空的用户没法选
  if (p.source.kind !== 'window') void loadMonitors()
  if (p.source.kind === 'window') void loadWindows()
}

/**
 * 关编辑弹窗（取消 / 点遮罩 / Esc 都走这里）。
 *
 * draft 是选中配置的一份副本（见它的声明处），弹窗里改的是副本，
 * 关掉直接丢掉即可，配置本身没动过 —— 所以这里不需要回滚任何东西。
 * 只是顺手把上次保存留下的提示清掉：草稿已经丢了，再显示"已保存"会让人
 * 以为刚才那次取消也存了盘。
 */
function cancelEdit() {
  editing.value = false
  editingNew.value = false
  saveError.value = ''
  saveNotice.value = ''
  // 新建模式：关掉就是放弃，列表本来就还没加这一路，什么都不用回滚。
}

/** Esc 关编辑弹窗。挂在 document 上，因为焦点可能停在表单里某个下拉上。 */
function editKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape' && editing.value) cancelEdit()
}
onMounted(() => document.addEventListener('keydown', editKeydown))
onBeforeUnmount(() => document.removeEventListener('keydown', editKeydown))

function select(id: string) {
  selectedId.value = id
  editing.value = false
  editingNew.value = false
  exportNotice.value = null
}

function toggleEnabled(p: Profile) {
  p.enabled = !p.enabled
  // 只存当前配置。以前这里调 saveProfiles() 会把整份配置发过去，
  // 隔壁正在录的那一路会被界面这份旧快照回退。
  void saveOneProfile(p.id, p)
}

async function saveEdits() {
  const d = draft.value

  // 新建模式：填完点「确定」才真正把这一路加进列表 + 落盘。
  if (editingNew.value) {
    const p = emptyProfile()
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
      },
    }
    if (d.sourceKind === 'window') {
      next.match = {
        rule: d.matchRule,
        by: MATCH_RULE_FIELDS[d.matchRule]?.[0] ?? 'process',
        mode: d.matchMode,
        pattern: d.matchPattern,
        pick: 'largest',
        pollMs: 1000,
        requireVisible: true,
        title: d.matchTitle,
        className: d.matchClass,
        process: d.matchProcess,
        hwnd: d.matchHwnd || 0,
      }
    }
    // 先落盘（拿到服务端 normalize 后的最终形状再入列，避免界面和磁盘两套值），
    // 成功才 push 进列表；失败留在弹窗里让用户改。
    const ok = await saveOneProfile(p.id, next)
    if (!ok) return
    profiles.value = [...profiles.value, next]
    selectedId.value = p.id
    editing.value = false
    editingNew.value = false
    return
  }

  const p = selectedProfile.value
  if (!p) return
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
      // 匹配方式：四个显式选项，服务端按它决定"同时满足哪几项"（全 AND）。
      rule: d.matchRule,
      // by 只为"不认识 rule 的老服务"留个能读的键；新服务优先看 rule。
      by: MATCH_RULE_FIELDS[d.matchRule]?.[0] ?? 'process',
      // byOrder 是降级模型的产物，已经不再参与匹配 —— 不再写它，
      // 免得下次读回来又被误当成"用户排的优先级"。
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
      // 三份指纹都留着（切换匹配方式时按需组合），但任何一份都不会单独
      // 绕过 matchRule 生效。
      title: d.matchTitle,
      className: d.matchClass,
      process: d.matchProcess,
      // 用户挑中的窗口。服务端只在"已经满足 rule 的候选"里优先它 —— 它是偏好，
      // 不是条件，所以不会让匹配变宽（仍不会乱录别的窗口）。
      hwnd: d.matchHwnd || 0,
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
      // `res.failed[0]` 在 noUncheckedIndexedAccess 下是 `… | undefined`，
      // 上面已经判过 length > 0，这里用 `?.` 消掉类型告警。
      saveNotice.value = `已存盘，但这一拍没送到服务（${res.failed[0]?.error ?? '通道忙'}）。再点一次保存即可，不会重启服务。`
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

/**
 * 删除当前这一路配置。
 *
 * 只删配置本身（主进程负责写盘 + 让服务停掉这一路的会话），已经录好的分片
 * 留在磁盘上 —— 删配置不该顺手删用户的素材。
 */
async function deleteProfile() {
  const p = selectedProfile.value
  if (!p) return
  const label = p.name || p.id

  // 删除不可撤销（配置里这一整行参数都会没），所以问一句。
  // Electron 渲染进程里 window.confirm 是可用的原生对话框。
  if (typeof window.confirm === 'function') {
    const ok = window.confirm(`删除配置「${label}」？\n\n只删配置，已经录好的分片不会被删除。`)
    if (!ok) return
  }
  const fn = api()?.deleteProfile as
    | ((id: string) => Promise<{ ok: boolean; error?: string; remaining?: number }>)
    | undefined
  if (typeof fn !== 'function') {
    saveError.value = 'preload 没有删除接口，界面版本和服务对不上。'
    return
  }
  deleting.value = true
  saveError.value = ''
  saveNotice.value = ''
  try {
    const res = (await fn(p.id)) ?? { ok: false }
    if (!res.ok) {
      saveError.value = res.error ?? '删除失败'
      return
    }
    const idx = profiles.value.findIndex((x) => x.id === p.id)
    const rest = profiles.value.filter((x) => x.id !== p.id)
    profiles.value = rest
    editing.value = false
    // 选中项往"删掉那一行的位置"接：删中间一行时停在原来下一行，删最后一行
    // 时退回新的最后一行，不会跳回第一行让用户重新找。
    selectedId.value = rest[Math.min(Math.max(idx, 0), rest.length - 1)]?.id ?? null
  } catch (err) {
    saveError.value = `删除出错：${(err as Error).message}`
  } finally {
    deleting.value = false
  }
}

/**
 * 侧栏拖拽换位。
 *
 * 用原生 HTML5 DnD（不用任何库）：按住某一行拖到另一行上松手，被拖的那路
 * 就插到目标位置。只改数组顺序，`num`（录像目录编号）跟着各自的配置走，
 * 所以换位不影响已经录好的分片目录。
 *
 * 落盘走专门的单向通道 `reorder-profiles`：只发 id 顺序，磁盘上每一路的
 * 内容原样保留 —— 和单路保存同一个安全思路，绝不把界面手里可能过时的
 * 整份快照写回去。
 */
const dragId = ref<string | null>(null)
const dragOverId = ref<string | null>(null)

function onDragStart(id: string, e: DragEvent) {
  dragId.value = id
  if (e.dataTransfer) {
    e.dataTransfer.effectAllowed = 'move'
    // Firefox 没有 setData 不启动拖拽，顺手塞一份
    e.dataTransfer.setData('text/plain', id)
  }
}

function onDragOver(id: string, e: DragEvent) {
  if (dragId.value === null || dragId.value === id) return
  dragOverId.value = id
  if (e.dataTransfer) e.dataTransfer.dropEffect = 'move'
}

function onDragLeave(id: string) {
  if (dragOverId.value === id) dragOverId.value = null
}

function onDragEnd() {
  dragId.value = null
  dragOverId.value = null
}

async function onDrop() {
  // li 和 ul 都挂了 drop，会触发两次；第二次 dragId 已经是 null，直接返回
  const from = dragId.value
  const to = dragOverId.value
  dragId.value = null
  dragOverId.value = null
  if (!from || !to || from === to) return

  const list = profiles.value.slice()
  const fi = list.findIndex((x) => x.id === from)
  const ti = list.findIndex((x) => x.id === to)
  if (fi < 0 || ti < 0) return
  const [moved] = list.splice(fi, 1)
  list.splice(ti, 0, moved as Profile)
  profiles.value = list

  const fn = api()?.reorderProfiles as
    | ((ids: string[]) => Promise<{ ok: boolean; error?: string }>)
    | undefined
  if (typeof fn !== 'function') return
  try {
    const res = (await fn(list.map((p) => p.id))) ?? { ok: false }
    if (!res.ok) saveError.value = res.error ?? '顺序没保存上'
  } catch (err) {
    // 界面上的顺序已经换了；落盘失败的话下次启动会弹回旧顺序，如实提示
    saveError.value = `顺序没保存上：${String(err)}`
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

/**
 * 服务此刻真的挂在哪个窗口上（hwnd + 标题）。
 *
 * 这是"这一路在录什么"的**唯一权威答案**：配置里那几份指纹是用户当初挑窗口
 * 时记下的快照，标题一变就作废了（浏览器换页面、文档改名），而服务手里握着
 * 的是活着的句柄。
 */
const liveCapturedWindow = computed(() => {
  const s = selectedLive.value
  if (!s) return null
  const hwnd = Number(s.hwnd ?? 0)
  if (!hwnd) return null
  return { hwnd, title: String(s.matchedTitle ?? '').trim() }
})

/**
 * 下拉里要补的那一行「当前正在录制：xxx」。
 *
 * 需要它的场合：窗口关掉再打开、或者被最小化，hwnd 就换了一个，枚举列表里
 * 再也找不到服务握着的那个句柄。以前这种情况下拉是空白的，用户点开编辑面板
 * 只看到"（选择一个窗口）"，以为配置丢了 —— 其实服务还在录。补一行如实
 * 反映现状，比留一句"请选择一个窗口"诚实得多。
 */
const capturedOption = computed(() => {
  const live = liveCapturedWindow.value
  if (!live) return null
  if (windows.value.some((w) => Number(w.hwnd) === live.hwnd)) return null
  return { hwnd: String(live.hwnd), title: live.title || `窗口 ${live.hwnd}` }
})

// ---- 匹配方式（四个选项）----------------------------------------------------
//
// 用户要求：「如果匹配不到，不要随便选择一个窗口啊，优先匹配改掉吧」。
// 所以模型从"按优先级降级"换成"固定组合 + 全部 AND"：**应用程序（exe）是
// 每一档的必要条件**，名字/类名是叠加条件。少了任何一条就不算匹配，这一路
// 保持未采集 —— 宁可空着，也不退而求其次抓一个别的窗口来录。
const MATCH_RULE_OPTIONS = [
  { value: 'process', label: '只匹配应用程序' },
  { value: 'title+process', label: '匹配名字和应用程序' },
  { value: 'class+process', label: '匹配类名和应用程序' },
  { value: 'title+class+process', label: '匹配名字、类名和应用程序' },
] as const

/** 每一档要**同时满足**的字段（与服务端 WindowMatch::RequiredFields 一一对应）。 */
const MATCH_RULE_FIELDS: Record<string, string[]> = {
  process: ['process'],
  'title+process': ['title', 'process'],
  'class+process': ['class', 'process'],
  'title+class+process': ['title', 'class', 'process'],
}

const MATCH_FIELD_LABEL: Record<string, string> = {
  title: '名字',
  class: '类名',
  process: '应用程序',
}

/**
 * 老配置没有 `rule` 键时该显示哪一档。
 *
 * 必须和服务端 `WindowMatch::Normalize` 的推断**完全一致**，否则会出现
 * 「界面显示按名字+应用程序，服务实际按只匹配应用程序在跑」这种对不上的情况。
 */
function deriveMatchRule(m?: Profile['match']): string {
  if (!m) return 'title+process'
  const given = String(m.rule ?? '').trim()
  if (given && MATCH_RULE_FIELDS[given]) return given
  const order = Array.isArray(m.byOrder) && m.byOrder.length ? m.byOrder : [String(m.by ?? 'title')]
  const primary = String(order[0] ?? 'title')
  if (primary === 'process') return 'process'
  if (primary === 'class') return 'class+process'
  if (String(m.title ?? '').trim()) return 'title+process'
  if (String(m.className ?? '').trim()) return 'class+process'
  return 'process'
}

/** 某一档规则要匹配哪几个字段。 */
function ruleFields(rule: string): string[] {
  return MATCH_RULE_FIELDS[rule] ?? MATCH_RULE_FIELDS['title+process']!
}

/**
 * 某个窗口的某一项身份是否满足「指纹 + 比较方式」。
 *
 * 语义要和服务端 MatchWindows 对齐：**名字**按 mode（默认"包含"，标题会变），
 * **类名 / 应用程序**只做"相等" —— 它们是标识符，用包含语义只会放宽到莫名其妙。
 */
function fieldMatches(field: string, w: WindowItem, fp: string, mode: string): boolean {
  const want = fp.trim().toLowerCase()
  if (!want) return false
  const subject =
    field === 'class'
      ? String(w.class ?? '')
      : field === 'process'
        ? String(w.process ?? '')
        : String(w.title ?? '')
  const got = subject.toLowerCase()
  if (field !== 'title') return got === want
  switch (mode) {
    case 'exact':
      return got === want
    case 'prefix':
      return got.startsWith(want)
    case 'regex': {
      try {
        return new RegExp(fp.trim(), 'i').test(subject)
      } catch {
        return false
      }
    }
    case 'contains':
    default:
      return got.includes(want)
  }
}

/** 一个窗口是否满足整条规则（全部 AND，缺一不可）。 */
function windowSatisfies(
  w: WindowItem,
  rule: string,
  title: string,
  cls: string,
  exe: string,
  mode: string,
): boolean {
  const fp: Record<string, string> = { title, class: cls, process: exe }
  // 只比"真的有指纹"的字段（和服务端 ActiveFields 一致）：老配置可能只记了
  // 名字；要求它必须有 exe 等于把这个配置永久判死。但填了却没匹上 = 不匹配，
  // 绝不降级去比别的字段。
  const fields = ruleFields(rule).filter((f) => String(fp[f] ?? '').trim() !== '')
  if (!fields.length) return false
  return fields.every((f) => fieldMatches(f, w, String(fp[f] ?? ''), mode))
}

/**
 * 窗口下拉里该选中哪一个 —— 找**同时满足全部条件**的那个窗口。
 *
 * 顺序很重要：
 *   1. 用户明确挑过的那个（draft.matchHwnd）：它也满足规则就选它。
 *      「只匹配应用程序」下同一程序的多个窗口全都满足，只看第一条命中会永远
 *      停在同一项上 —— 用户换另一个同程序的窗口，下拉立刻弹回去，看起来就是
 *      "切不过去"。有了这一条，挑哪个就显示哪个。
 *   2. 没挑过时，优先"服务此刻真的在录的那个"（前提：它也满足规则）。
 *      同样是因为同程序的多个窗口都命中，按列表顺序挑会显示 A、实际录的是 B。
 *      注意这不等于"用正在录的窗口去盖住失效的规则"—— 它必须先满足规则。
 *   3. 否则取列表里第一个满足规则的。
 *   4. 都不满足就返回空（"请选择一个窗口"）：匹配不到就别乱选。
 *
 * 唯一例外：列表还没枚举回来（1~2 秒）时先选服务手里那个，避免这一两秒里
 * 显示"请选择"、看着像配置丢了。
 */
const windowPickValue = computed(() => {
  const d = draft.value
  const list = windows.value
  const live = liveCapturedWindow.value
  if (!list.length) return live ? String(live.hwnd) : ''
  const ok = (w: WindowItem) =>
    windowSatisfies(w, d.matchRule, d.matchTitle, d.matchClass, d.matchProcess, d.matchMode)
  const picked = d.matchHwnd ? list.find((w) => Number(w.hwnd) === d.matchHwnd && ok(w)) : undefined
  if (picked) return String(picked.hwnd)
  const captured = live ? list.find((w) => Number(w.hwnd) === live.hwnd && ok(w)) : undefined
  if (captured) return String(captured.hwnd)
  const hit = list.find(ok)
  return hit ? String(hit.hwnd) : ''
})

/** 当前这一档具体在比什么，逐条列出来。 */
const matchRuleHint = computed(() => {
  const d = draft.value
  if (d.sourceKind !== 'window') return ''
  const fp: Record<string, string> = { title: d.matchTitle, class: d.matchClass, process: d.matchProcess }
  const parts = ruleFields(d.matchRule).map((f) => {
    const v = String(fp[f] ?? '').trim() || '（未填）'
    return `${MATCH_FIELD_LABEL[f] ?? f}「${v}」`
  })
  return `需要同时满足：${parts.join(' + ')}`
})

/**
 * 窗口下拉里的一行显示什么：标题 + exe，跟 OBS 的窗口列表一个路子。
 */
function windowLabel(w: WindowItem): string {
  const t = String(w.title ?? '').trim() || '(无标题)'
  const exe = String(w.process ?? '').trim()
  // 最小化的窗口平时进不了下拉；能出现在这儿的只有"正在被录的那个"，
  // 标注一下，免得用户奇怪为什么列表里有个看不见的窗口。
  const suffix = w.minimized ? '  —  已最小化' : exe ? `  —  ${exe}` : ''
  return `${t}${suffix}`
}

/**
 * 用户从下拉里挑了一个窗口。
 *
 * 选一次就把这个窗口的**三项身份**全记下来（名字 / 类名 / exe），具体用其中
 * 哪几项由 matchRule 决定（全部 AND）。这样切换匹配方式时不用重新选窗口。
 */
function useWindow(hwnd: string | number) {
  const w = windows.value.find((x) => String(x.hwnd) === String(hwnd))
  if (!w) return
  const title = String(w.title ?? '').trim()
  const cls = String(w.class ?? '').trim()
  const exe = String(w.process ?? '').trim()
  // 标题可能很长（浏览器会带一堆后缀），只取前 32 字参与匹配：
  // 后面的部分最容易变（" - 哔哩哔哩"、"▶ 正在播放"），带上只会更难匹配。
  draft.value.matchTitle = title.slice(0, 32)
  draft.value.matchClass = cls
  draft.value.matchProcess = exe
  // 记下挑的是哪一个窗口。「只匹配应用程序」时三份指纹可能和上一个窗口完全
  // 相同，只有它能区分用户想要的是同一个程序的哪个窗口（服务端按偏好处理）。
  draft.value.matchHwnd = Number(w.hwnd) || 0
  // pattern 只是给"不认识 rule 的老服务"留的兼容字段，取当前这一档最看重的那个
  const strong =
    draft.value.matchRule === 'process'
      ? exe
      : draft.value.matchRule === 'class+process'
        ? cls
        : title.slice(0, 32)
  draft.value.matchPattern = strong
  // 挑窗口这件事本身就是「包含」语义（标题会变），正则留着手写
  draft.value.matchMode = 'contains'
}

async function launchService() {
  const fn = api()?.openServiceConsole as (() => Promise<unknown>) | undefined
  if (typeof fn !== 'function') return
  await fn()
  // 服务起来到建环有几百毫秒，稍等一下再重连
  setTimeout(() => void retry(), 900)
}

// ---- 其它 -----------------------------------------------------------------

// ---- 顶栏帧率显示 ---------------------------------------------------------
//
// 显示的是**录制实测帧率**（服务端每秒上报的 actualFps），不是预览渲染帧率。
//
// 【为什么要改】以前这里显示 fpsOf() —— 那是页面**预览**实际画出来的帧率。
// 预览帧率被服务端按"面板大小"做限流（面板大就发全分辨率、顺带把帧率收到 60），
// 于是"设置 120fps 录制"的显示器源，顶栏却显示 60 甚至更低；一旦面板宽度在
// 砍半/不砍半的临界点附近抖动，预览帧率就在 60↔120 之间乱跳，用户看着就像
// "录制的帧率坏了、随机掉"。
//
// 真相是录制一直稳稳跑在 120（actualFps 从未掉），乱跳的只是预览的**显示帧率**。
// 顶栏这个数字该回答"我设置的录制帧率到底跑没跑到"，所以改读 actualFps。
// 预览帧率仍可从 stats.renderFpsShown 拿到，只是不再占顶栏这个最显眼的位置。
const fpsShown = computed(() => {
  const a = selectedLive.value?.actualFps
  if (typeof a === 'number' && Number.isFinite(a) && a > 0) return Math.round(a)
  // 服务还没上报（刚连上）：退回预览账本垫一下，避免显示 0
  return Math.round(fpsOf(selectedId.value))
})

function describeSource(p: Profile): string {
  if (p.source.kind === 'desktop') return '整屏桌面'
  if (p.source.kind === 'monitor') return `显示器 ${p.source.monitorIndex + 1}`
  const m = p.match
  if (!m) return '窗口（没写匹配规则）'
  // 侧栏那一行只求"一眼看出这一路在录谁"：按当前匹配方式把参与比较的值
  // 用「·」串起来，例如「窗口 辣妹的午餐 · 哔哩哔哩.exe」。
  const fp: Record<string, string> = {
    title: String(m.title ?? m.pattern ?? '').trim(),
    class: String(m.className ?? '').trim(),
    process: String(m.process ?? '').trim(),
  }
  const parts = ruleFields(deriveMatchRule(m)).map((f) => fp[f] || '（空）')
  return `窗口 ${parts.join(' · ')}`
}

function describeProfile(p: Profile): string {
  return `${describeSource(p)} · ${p.video.fps}fps · ${p.video.bitrateMbps}Mbps · ${p.video.replaySeconds}s`
}

// 选中哪一路 -> 预览就接哪一路的帧环
watch(selectedId, (id) => {
  void switchSession(id)
})

// 编辑按钮和 sidebar 的联动：editing 变 true 就把当前选中那一路填进草稿。
//
// 【新建必须绕开】addProfile() 自己会把 draft 填成一份**默认值**，watch 是
// 异步触发的（当前同步代码跑完才执行），如果这里不挡住，新建刚填好的默认
// 草稿会被 startEdit() 用**当前选中那一路**的值整个覆盖 —— 用户看到的就是
// 弹窗里全是旧配置的信息，点「确定」还会把原来那一路改掉而不是新增一路。
watch(editing, (v) => {
  if (v && !editingNew.value) startEdit()
})

// 编辑面板里切到"窗口捕获"时补拉一次窗口列表。
//
// 从"显示器捕获"改过来的配置，编辑面板打开时只拉过显示器列表，窗口下拉是空的
// ——这时"自动选中当前录制的窗口"根本无从谈起（没有可选项）。正在加载就不重复发。
watch(
  () => draft.value.sourceKind,
  (k) => {
    if (k !== 'window') return
    if (windows.value.length === 0 && !loadingWindows.value) void loadWindows()
  },
)

// 【这里以前有两个 watch 去"修复"配置里的窗口身份】现在整段删掉了：
// 新模型下窗口下拉（windowPickValue）是 computed，窗口列表一变就自己重算，
// 不需要再**改写草稿**去对齐什么。
// 而且那种"自动把指纹换成当前正在录的窗口"的行为本身就有害 —— 它会让一个
// 早就对不上的规则看起来像是对的，用户永远发现不了配置已经失效。

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
  background: #e0a83c;
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
  display: flex;
  flex-direction: column;
  gap: 18px;
  padding: 16px;
  overflow: auto;
}

/* 编辑弹窗压在设置弹窗之上（两个不会同时开，但真叠了也是编辑在上）。 */
.editor-mask {
  z-index: 60;
}
/* 比设置面板窄一点：里面是一列表单，太宽了输入框拉得没边 */
.editor-sheet {
  width: min(500px, calc(100vw - 48px));
}
/* 标题右边那一行"正在编辑哪一路" */
.sheet-sub {
  margin-left: 8px;
  color: #7c879b;
  font-size: 12px;
  font-weight: 400;
}
/* 表单进了弹窗：分隔线和左右内边距都由 .sheet-body 给了，别再叠一层 */
.form.in-modal {
  border-top: 0;
  padding: 0;
}

.row {
  display: flex;
  flex-direction: column;
  gap: 10px;
}
/* 设置页里 label 和控件并排的行（关闭行为下拉），跟"重启服务"行风格一致 */
.row-inline {
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
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
.row-select {
  flex: 0 0 auto;
  min-width: 150px;
  padding: 5px 8px;
  border: 1px solid #2a3550;
  border-radius: 5px;
  background: #1a2130;
  color: #cdd6e8;
  font-size: 12px;
  cursor: pointer;
}
.row-select:disabled {
  opacity: 0.5;
  cursor: default;
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
/* 重启服务：破坏性操作，用红色和"确认"区分开 */
.sheet-btn.danger {
  background: #4a1f24;
  border-color: #7a3038;
  color: #ffc9cd;
}
.sheet-btn.danger:hover:not(:disabled) {
  background: #5c272d;
  border-color: #9c3d47;
}
/* 横排的一行（左标签 + 右按钮），跟上面纵排的滑杆行区分开 */
.danger-row {
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  margin-top: 4px;
  padding-top: 12px;
  border-top: 1px solid #232c42;
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
  /* 侧栏自己不滚：滚动交给里面那一层 .plist-scroll。
     以前整条侧栏一起滚，"当前配置"会被列表顶到看不见的地方；
     现在它是钉在底部的常驻信息，列表再长也只在自己那层里滚。
     这里必须写 overflow:hidden，否则 flex 子项的 min-height:0 不生效，
     滚动条会撑到整个侧栏上。 */
  overflow: hidden;
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
  /* 头部不参与收缩，也不跟着列表滚 */
  flex: 0 0 auto;
}

/* 头部右边的按钮组。编辑在左、新建在右 —— 顺序在模板里定，这里只管间距。 */
.head-btns {
  display: flex;
  align-items: center;
  gap: 6px;
}

/* 配置列表的滚动层。
   min-height:0 是 flex 布局里让子元素"可以比内容矮"的关键 —— 少了它，
   flex 项默认 min-height:auto，被子元素撑开后 overflow 永远不会触发，
   滚动条会跑到整条侧栏上。 */
.plist-scroll {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
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
.mini .pencil {
  width: 11px;
  height: 11px;
  margin-right: 4px;
  vertical-align: -1px;
}
.mini.wide {
  width: 100%;
  padding: 5px;
}

/* 删除：红色但压暗，跟"保存"的蓝色拉开距离；margin-left:auto 把它推到行尾，
   手滑点"取消"时不会误伤。 */
.mini.danger {
  margin-left: auto;
  border-color: #4a2426;
  background: #241315;
  color: #ff9b95;
}
.mini.danger:hover:not(:disabled) {
  border-color: #7a3336;
  background: #33181a;
  color: #ffb3ad;
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
/* 拖拽中的那一路压暗一点，让"要落到哪一行"的亮框更醒目 */
.pitem.dragging {
  opacity: 0.45;
}
/* 拖拽悬停的落点：亮一圈边框提示松手后会被插到这里 */
.pitem.drop-on {
  box-shadow: inset 0 0 0 1px #2b6cb0;
  background: #12203a;
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

/* 底部"当前配置"：不收缩、不滚动，永远贴在侧栏最底下 */
/* 当前配置：高度钉死在窗口高度的 30%，不随内容长短变。
   内容多时自己内部滚 —— 上面"配置列表"那一层的滚动条是它的，
   这一块是常驻信息，不该跟着列表一起被滚出去。 */
.cur {
  flex: 0 0 30vh;
  height: 30vh;
  min-height: 30vh;
  max-height: 30vh;
  overflow-y: auto;
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

/* 匹配方式的说明行（"需要同时满足：…"）：中性灰，只是补充信息。 */
.rule-hint {
  margin: 4px 0 0;
  font-size: 10.5px;
  line-height: 1.5;
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
