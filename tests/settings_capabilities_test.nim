import leaf
import leaf/[backend, backend_session, native_theme]
import leaf/styles
import std/[options, os, strutils, tables, tempfiles, times]
import ../src/[updater, update_policy]
import leaf/native_layout
import leaf/backends/win32/capabilities
import ../src/settings_view
when defined(windows) and defined(amd64):
  import std/widestrs
  import leaf/ui_thread
  import leaf/backends/win32/[api, backend as win32backend, calls, host]
  import ../src/shiguang as app_host

proc captions(node: Node): seq[string] =
  if node.text.len > 0: result.add(node.text)
  for child in node.children: result.add(captions(child))

proc find(node: Node, key: string): Node =
  if node.key == key: return node
  for child in node.children:
    let match = find(child, key)
    if match != nil: return match

proc requireSettingsGeometry(view: Node) =
  var entities = initTable[NodeHandle, MountedEntity]()
  var ids = initTable[string, NodeHandle]()
  proc visit(node: Node, parent: NodeHandle, index: int) =
    let id = NodeHandle(entities.len + 1)
    ids[node.key] = id
    entities[id] = MountedEntity(handle: id, parent: parent, index: index,
      nodeId: node.key, kind: node.kind, props: normalizeProps(node))
    for i, child in node.children: visit(child, id, i)
  visit(view, NoHandle, 0)
  let boxes = layoutTree(entities, DipSize(width: settingsWidth, height: settingsHeight),
    proc(entity: MountedEntity, available: DipSize): DipSize =
      # Intrinsic labels/keycaps use ample placeholder widths; the layout
      # assertions concern only the fixed control slots and clipping.
      DipSize(width: (if entity.nodeId == "tray": 260 else: 32),
              height: (if entity.kind == NodeKind.separator: 1 else: 20)))
  proc box(key: string): LayoutBox =
    for b in boxes:
      if b.handle == ids[key]: return b
    doAssert false, key
  for dpi in [96, 144]:
    let quit = pixelEdges(box("quit").rect, dpi)
    let sidebarRight = (settingsSidebarWidth - 14) * dpi div 96
    let cardRight = (settingsWidth - 36) * dpi div 96
    doAssert quit.right == sidebarRight
    doAssert quit.bottom - quit.top == 34 * dpi div 96
    for key in ["nav-general", "nav-update"]:
      let nav = pixelEdges(box(key).rect, dpi)
      doAssert nav.right == sidebarRight, "sidebar nav right edge: " & key
    if ids.hasKey("change"):
      let change = pixelEdges(box("change").rect, dpi)
      doAssert change.right == cardRight
      doAssert change.bottom - change.top == 34 * dpi div 96
      doAssert quit.bottom - quit.top == change.bottom - change.top
    for i in 0..4:
      let key = "cap-" & $i
      if ids.hasKey(key):
        let b = box(key)
        let cap = pixelEdges(b.rect, dpi)
        let clip = pixelEdges(b.clip, dpi)
        doAssert cap.right - cap.left >= 32 * dpi div 96, "keycap too narrow: " & key
        doAssert cap.left >= clip.left and cap.top >= clip.top and
          cap.right <= clip.right and cap.bottom <= clip.bottom, "clipped keycap: " & key
    if ids.hasKey("software-update") and ids.hasKey("installed-version"):
      let title = pixelEdges(box("software-update").rect, dpi)
      let version = pixelEdges(box("installed-version").rect, dpi)
      doAssert title.right <= version.left, "update title and version must not overlap"
      doAssert version.right - version.left >= 200 * dpi div 96, "version slot too narrow for dev tag"
    for key in ["startup", "update-channel-trigger", "auto-download", "update-progress"]:
      if ids.hasKey(key):
        doAssert pixelEdges(box(key).rect, dpi).right == cardRight, "right edge: " & key
    if ids.hasKey("update-check"):
      let checkButton = pixelEdges(box("update-check").rect, dpi)
      doAssert checkButton.left >= (settingsSidebarWidth + 36) * dpi div 96, "check button inside the card"
      doAssert checkButton.right <= cardRight, "check button right edge"
      for key in ["update-action", "update-cancel"]:
        if not ids.hasKey(key): continue
        let other = pixelEdges(box(key).rect, dpi)
        doAssert other.left >= checkButton.right, "update buttons must not overlap: " & key
        doAssert other.right <= cardRight, "update button right edge: " & key
    if ids.hasKey("startup-label") and ids.hasKey("startup"):
      doAssert pixelEdges(box("startup-label").rect, dpi).left <
        pixelEdges(box("startup").rect, dpi).left
    if ids.hasKey("auto-download-label") and ids.hasKey("auto-download"):
      doAssert pixelEdges(box("auto-download-label").rect, dpi).left <
        pixelEdges(box("auto-download").rect, dpi).left
    for key in ["nav-general", "nav-update", "tray", "quit", "change", "unregistered",
        "general-divider", "startup", "startup-status", "general-tip",
        "software-update", "installed-version", "channel-divider",
        "update-channel-trigger", "auto-download", "update-action", "update-progress",
        "update-check", "update-cancel", "update-hint", "update-candidate",
        "last-check", "update-feedback"]:
      if not ids.hasKey(key): continue
      let b = box(key)
      let pixels = pixelEdges(b.rect, dpi)
      let clip = pixelEdges(b.clip, dpi)
      doAssert pixels.right > pixels.left and pixels.bottom > pixels.top, "empty control: " & key
      doAssert pixels.left >= clip.left and pixels.top >= clip.top and
        pixels.right <= clip.right and pixels.bottom <= clip.bottom, "clipped control: " & key

let win32 = win32Capabilities(shadcnLight)
var selectedTab = stGeneral
let initialView = settingsView(0x0653'u32, false, "", true, nil, nil,
  activeTab = stGeneral, onSelectTab = proc(tab: SettingsTab) = selectedTab = tab)
doAssert find(initialView, "general") != nil, "general tab must show general preferences"
doAssert find(initialView, "nav-general") != nil and find(initialView, "nav-update") != nil,
  "sidebar must expose both navigation tabs"
find(initialView, "nav-update").handler(EventKind.click)(Event(kind: click))
doAssert selectedTab == stUpdate
let initialUpdateView = settingsView(0x0653'u32, false, "", true, nil, nil,
  activeTab = stUpdate, onSelectTab = proc(tab: SettingsTab) = selectedTab = tab)
doAssert find(initialUpdateView, "software-update") != nil, "update tab must show software updates"
find(initialUpdateView, "nav-general").handler(EventKind.click)(Event(kind: click))
doAssert selectedTab == stGeneral

for (chord, recording, status, trayReady, expected, absent) in [
    (0x0653'u32, false, "", true, @["Ctrl", "Shift", "S", "更改"], @["未注册", "取消录入"]),
    (0x0f20'u32, false, "", true, @["Ctrl", "Alt", "Shift", "Win", "Space", "更改"], @["未注册"]),
    (0'u32, false, "", false, @["未注册", "●  通知区域图标不可用", "更改"], @["Ctrl"]),
    (0x0653'u32, true, "", true, @["请按下新组合键  ·  Esc 取消", "取消录入"], @["Ctrl", "更改"]),
    (0x0653'u32, false, shortcutFeedback(1, 0x0653), true,
      @["Ctrl", "Shift", "S", "ⓘ  新组合键已被占用，仍使用 Ctrl + Shift + S。", "重新录入"],
      @["取消录入", "更改"])]:
  let view = settingsView(chord, recording, status, trayReady, nil, nil, activeTab = stGeneral)
  let minimize = find(view, "minimize")
  let close = find(view, "close")
  doAssert minimize != nil and close != nil, "independent accessible caption buttons"
  doAssert minimize.text == "最小化" and close.text == "关闭"
  doAssert minimize.styles == close.styles
  doAssert find(view, "nav-general").styles == find(view, "nav-update").styles
  requireSettingsGeometry(view)
  let tree = mount(view)
  discard checkCapabilities(tree, win32, compatible)
  discard checkCapabilities(tree, win32, strict)
  let labels = captions(view)
  for text in expected: doAssert text in labels, text
  for text in absent: doAssert text notin labels, text
echo "5 Settings views passed the Leaf Win32 capability check"
doAssert shortcutFeedback(1, 0) == "新组合键已被占用，当前没有可用快捷键。"
doAssert shortcutFeedback(2, 0x0653) == "无法保存新组合键，仍使用 Ctrl + Shift + S。"
doAssert shortcutFeedback(3, 0x0653) == "新组合键无效，仍使用 Ctrl + Shift + S。"

for (state, error, expected) in [
    (0.cint, false, ""),
    (1.cint, false, ""),
    (2.cint, false, "已有其他路径的同名启动项，未改动"),
    (3.cint, false, "无法读取启动项状态"),
    (0.cint, true, "无法更新启动项。当前：未注册"),
    (1.cint, true, "无法更新启动项。当前：已注册，实际启动由 Windows 设置或策略决定")]:
  let view = settingsView(0x0653'u32, false, "", true, nil, nil,
    startup = state, startupError = error, activeTab = stGeneral)
  requireSettingsGeometry(view)
  discard checkCapabilities(mount(view), win32, strict)
  let control = find(view, "startup")
  doAssert control.text == "登录 Windows 时启动拾光"
  let props = normalizeProps(control)
  doAssert props.control.checked.get == (state == 1)
  doAssert props.control.enabled.get == (state < 2)
  let detail = find(view, "startup-status")
  if expected.len == 0:
    doAssert detail == nil, "normal startup must not occupy a second row"
  else:
    doAssert detail != nil and detail.text == expected
echo "6 Startup views passed the Leaf Win32 capability check"

block:
  let dir = createTempDir("shiguang-update-ui-", "")
  defer: removeDir(dir)
  let settingsPath = dir / "update.settings"
  let installed = UpdateBuild(order: 1, version: "v9.8.7", channel: ucStable, revision: "base")
  let candidate = UpdateBuild(order: 2, version: "dev-xyz", channel: ucTest, revision: "next")
  var engine = openUpdater(settingsPath, installed)
  var requested: seq[UpdateAction]
  var dropdown = false
  var feedback = ""
  proc render(snapshot: UpdateSnapshot; allFeedback = false; tab = stUpdate): Node =
    settingsView(0x0f20'u32, false, (if allFeedback: "录入失败，仍使用旧快捷键。" else: ""), true,
      nil, nil, startup = (if allFeedback: 2 else: 0), update =
      UpdateUi(snapshot: snapshot, feedback: feedback, channelOpen: dropdown,
        onAction: proc(action: UpdateAction) = requested.add(action),
        onChannelOpen: proc(e: Event) = dropdown = e.open),
      activeTab = tab)
  proc activate(view: Node, key: string, event: Event) =
    let node = find(view, key)
    doAssert node != nil and node.handler(event.kind) != nil, key
    node.handler(event.kind)(event)
  proc selected(view: Node): string =
    find(view, "update-channel-trigger").text
  proc enabled(view: Node): bool =
    normalizeProps(find(view, "auto-download")).control.checked.get
  var view = render(engine.snapshot)
  doAssert find(view, "installed-version").text == "当前版本 v9.8.7 正式版"
  doAssert find(view, "last-check").text == "最近检查：尚未检查"
  doAssert find(view, "nav-update").text == "软件更新"
  doAssert selected(view) == "测试版" and enabled(view)
  activate(view, "update-channel", Event(kind: openChange, open: true))
  doAssert dropdown
  activate(view, "auto-download", Event(kind: change, checked: false))
  doAssert requested == @[uaAutoOff]
  doAssert engine.setAutoDownload(false)
  let job = engine.onTimer(1_765_000_000).get
  doAssert engine.complete(job, some(candidate))
  view = render(engine.snapshot)
  doAssert not enabled(view) and find(view, "update-candidate").text.contains("dev-xyz")
  doAssert find(view, "nav-update").text.contains("新版本")
  doAssert find(view, "last-check").text != "最近检查：尚未检查"
  activate(view, "update-action", Event(kind: click))
  doAssert requested[^1] == uaDownload
  activate(view, "update-check", Event(kind: click))
  doAssert requested[^1] == uaCheck
  activate(view, "update-channel", Event(kind: change, value: "stable"))
  doAssert requested[^1] == uaStable
  let download = engine.requestDownload().get
  for (phase, label, action, command) in [
      (upIdle, "已是最新版本", "", uaCheck), (upChecking, "正在检查更新", "", uaCheck),
      (upCheckFailed, "暂时无法检查更新", "", uaCheck),
      (upDownloading, "正在下载并验证", "", uaCheck),
      (upDownloadFailed, "下载未成功", "重试下载", uaDownload),
      (upReady, "新版本已就绪", "重启更新", uaRestart),
      (upRestarting, "正在准备重启", "", uaCheck)]:
    var snapshot = engine.snapshot
    snapshot.phase = phase
    snapshot.installReady = false
    snapshot.autoInstall = false
    snapshot.result = rrNone
    snapshot.error = "C:/private/internal-test-key"
    let v = render(snapshot)
    doAssert find(v, "update-status").text == label
    doAssert ("C:/private/internal-test-key" notin captions(v).join(" "))
    doAssert (find(v, "update-progress") != nil) == (phase in {upChecking, upDownloading})
    doAssert find(v, "nav-update").text.contains("新版本") == (phase in {upAvailable, upReady})
    discard checkCapabilities(mount(v), win32, strict)
    requireSettingsGeometry(v)
    if phase == upRestarting:
      doAssert find(v, "update-check") == nil and find(v, "update-action") == nil
      doAssert find(v, "update-hint") == nil
    else:
      doAssert find(v, "update-hint").text.contains("空闲自动重启")
      doAssert find(v, "update-check").text == "检查并安装更新"
      activate(v, "update-check", Event(kind: click))
      doAssert requested[^1] == uaCheck
    if action == "":
      doAssert find(v, "update-action") == nil
    else:
      doAssert find(v, "update-action").text == action
      activate(v, "update-action", Event(kind: click))
      doAssert requested[^1] == command
  # From checking to downloading to waiting, an authorized round is cancellable.
  for (phase, expected) in [(upChecking, "取消只停止自动重启"), (upDownloading, "下载会继续"),
                            (upReady, "已下载，完成当前操作后自动安装")]:
    var armed = engine.snapshot
    armed.phase = phase
    armed.autoInstall = true
    armed.installReady = phase == upReady
    armed.result = rrCandidate
    let v = render(armed)
    doAssert find(v, "update-cancel").text == "取消自动安装"
    doAssert find(v, "update-action") == nil
    doAssert find(v, "update-candidate").text.contains(expected)
    discard checkCapabilities(mount(v), win32, strict)
    requireSettingsGeometry(v)
    activate(v, "update-cancel", Event(kind: click))
    doAssert requested[^1] == uaCancelAutoInstall
    if phase == upReady:
      var cancelled = armed
      cancelled.autoInstall = false
      cancelled.installReady = false
      doAssert find(render(cancelled), "update-action").text == "重启更新"
  # A genuinely newer candidate was adopted: the round is authorized, but nothing is
  # downloaded yet, so the page must neither promise an install nor a done download.
  var adopted = engine.snapshot
  adopted.phase = upAvailable
  adopted.autoInstall = true
  adopted.installReady = false
  adopted.result = rrCandidate
  let adoptedView = render(adopted)
  doAssert find(adoptedView, "update-status").text == "发现新版本"
  doAssert find(adoptedView, "update-cancel") != nil
  doAssert find(adoptedView, "update-action") == nil
  doAssert "已下载" notin captions(adoptedView).join(" ")
  discard checkCapabilities(mount(adoptedView), win32, strict)
  requireSettingsGeometry(adoptedView)
  # The round's own outcome is visible even next to a cached ready stage.
  var outcomes: seq[string]
  for (result, expected) in [(rrCheckFailed, "本次检查未能完成"),
                             (rrUpToDate, "本次检查没有可安装的新版本"),
                             (rrInstallFailed, "更新未能启动")]:
    var cached = engine.snapshot
    cached.phase = upReady
    cached.installReady = false
    cached.autoInstall = false
    cached.result = result
    let v = render(cached)
    doAssert find(v, "update-candidate").text.contains(expected)
    outcomes.add(find(v, "update-candidate").text)
    discard checkCapabilities(mount(v), win32, strict)
    requireSettingsGeometry(v)
  doAssert outcomes[0] != outcomes[1] and outcomes[1] != outcomes[2]
  var upToDate = engine.snapshot
  upToDate.phase = upIdle
  upToDate.result = rrUpToDate
  let upToDateView = render(upToDate)
  doAssert find(upToDateView, "update-status").text == "已是最新版本"
  doAssert "没有可安装" notin captions(upToDateView).join(" ")   # the status already says it
  doAssert engine.complete(download)
  doAssert engine.setChannel(ucStable)
  view = render(engine.snapshot)
  doAssert selected(view) == "正式版" and find(view, "update-check").text == "检查并安装更新"
  # An unusable config parent exercises the real persistence failure, not a mock.
  removeFile(settingsPath)
  let blocked = dir / "missing" / "update.settings"
  var broken = openUpdater(blocked, installed)
  var saved = false
  try: saved = broken.setChannel(ucStable)
  except CatchableError: discard
  doAssert not saved
  feedback = updateActionFeedback(uaStable, saved).message
  view = render(broken.snapshot, allFeedback = true)
  requireSettingsGeometry(view)
  requireSettingsGeometry(render(broken.snapshot, allFeedback = true, tab = stGeneral))
  doAssert selected(view) == "测试版"
  doAssert find(view, "update-feedback").text.contains("无法保存")
  let devInstalled = UpdateBuild(order: 99, version: "dev-7fd455b1a2b3", channel: ucTest, revision: "7fd455b1a2b3")
  var devSnapshot = engine.snapshot
  devSnapshot.installed = devInstalled
  let devView = render(devSnapshot)
  doAssert find(devView, "installed-version").text == "当前版本 dev-7fd455b1a2b3 测试版"
  discard checkCapabilities(mount(devView), win32, strict)
  requireSettingsGeometry(devView)
  echo "Update settings state, callbacks and persistence failure passed"

  block:
    let stamp = 1_765_000_000'i64
    let data = dir / "throttle"
    createDir(data)
    let path = data / "update.settings"
    saveUpdateSettings(path, UpdateSettings(channel: ucTest, autoDownload: false, lastCheck: stamp))
    var store = openUpdater(path, installed)
    moveDir(data, dir / "unavailable")
    try:
      discard store.onTimer(stamp + CheckInterval)
      doAssert false, "expected real persistence failure before a check job is created"
    except CatchableError as error:
      # Same failure signal consumed by the view after the service's timer catches it.
      store.checkPersistenceFailed(error.msg)
    feedback = ""
    let failedView = render(store.snapshot)
    doAssert find(failedView, "update-status").text == "暂时无法检查更新"
    doAssert "已是最新版本" notin captions(failedView)
    doAssert find(failedView, "last-check").text ==
      "最近检查：" & fromUnix(stamp).local.format("yyyy-MM-dd HH:mm")
    doAssert find(failedView, "update-check").text == "检查并安装更新"
    doAssert find(failedView, "update-candidate").text.contains("本次检查未能完成")
    doAssert "没有安装任何更新" in find(failedView, "update-candidate").text
    doAssert "update.settings" notin captions(failedView).join(" ")

  block:
    let path = dir / "downloads.settings"
    var downloads = openUpdater(path, installed)
    doAssert downloads.setAutoDownload(false)
    let check = downloads.onTimer(1000).get
    doAssert downloads.complete(check, some(candidate))
    var last = downloads.snapshot
    var notice: UpdateFeedback
    proc publish(replacement = none(UpdateFeedback)) =
      let current = downloads.snapshot
      notice = nextUpdateFeedback(notice, last, current, replacement)
      last = current
      feedback = notice.message
    proc assertStatus(label: string; hasNotice = false) =
      let currentView = render(last)
      doAssert find(currentView, "update-status").text == label
      doAssert (find(currentView, "update-feedback") != nil) == hasNotice
    publish(some(updateActionFeedback(uaDownload, false)))
    assertStatus("发现新版本", true)
    publish()
    assertStatus("发现新版本", true)
    let first = downloads.requestDownload().get
    publish(some(updateActionFeedback(uaDownload, true)))
    assertStatus("正在下载并验证")
    # A second click can be refused while the accepted download is in flight.
    doAssert downloads.requestDownload().isNone
    publish(some(updateActionFeedback(uaDownload, false)))
    assertStatus("正在下载并验证", true)
    doAssert downloads.complete(first, error = "network diagnostic")
    publish()
    assertStatus("下载未成功")
    for succeeds in [false, true]:
      let retry = downloads.requestDownload().get
      publish(some(updateActionFeedback(uaDownload, true)))
      assertStatus("正在下载并验证")
      doAssert downloads.complete(retry, error = (if succeeds: "" else: "test diagnostic"))
      publish()
      assertStatus(if succeeds: "新版本已就绪" else: "下载未成功")
    publish(some(updateActionFeedback(uaRestart, false, busy = true)))
    assertStatus("新版本已就绪", true)
    publish()
    doAssert "请先完成" in find(render(last), "update-feedback").text
    doAssert downloads.setChannel(ucStable)
    publish() # The bridge notifies before the channel command returns.
    assertStatus("已是最新版本")
    publish(some(updateActionFeedback(uaStable, true)))
    assertStatus("已是最新版本")
    doAssert selected(render(last)) == "正式版"

  block:
    let data = dir / "preferences"
    createDir(data)
    let path = data / "update.settings"
    saveUpdateSettings(path, UpdateSettings(channel: ucTest, autoDownload: false, lastCheck: 100))
    var preferences = openUpdater(path, installed)
    let before = preferences.snapshot
    moveDir(data, dir / "preferences-offline")
    var saved = false
    try: saved = preferences.setChannel(ucStable)
    except CatchableError: discard
    doAssert not saved
    let failure = nextUpdateFeedback(default(UpdateFeedback), before, preferences.snapshot,
      some(updateActionFeedback(uaStable, saved)))
    moveDir(dir / "preferences-offline", data)
    let check = preferences.onTimer(100 + CheckInterval).get
    let checking = preferences.snapshot
    let retained = nextUpdateFeedback(failure, before, checking)
    feedback = retained.message
    let stillOld = render(checking)
    doAssert selected(stillOld) == "测试版"
    doAssert "无法保存" in find(stillOld, "update-feedback").text
    doAssert preferences.complete(check)
    let settled = preferences.snapshot
    let stillRetained = nextUpdateFeedback(retained, checking, settled)
    doAssert stillRetained.message == failure.message
    doAssert preferences.setAutoDownload(true)
    feedback = nextUpdateFeedback(stillRetained, settled, preferences.snapshot,
      some(updateActionFeedback(uaAutoOn, true))).message
    let changed = render(preferences.snapshot)
    doAssert enabled(changed) and find(changed, "update-feedback") == nil
  echo "Update feedback lifetime and throttle failure presentation passed"

block:
  # Real Leaf Runtime + BackendSession regression for the production safe-tick coordinator:
  # click events only enqueue during dispatch and reconcile during safeTurn/drainEvents.
  let TabProbe = defineComponent[tuple[], State[SettingsTab]]("TabProbe",
    proc(ctx: MountContext, props: tuple[]): State[SettingsTab] =
      ctx.state(stGeneral),
    proc(ctx: ComponentContext, props: tuple[], tab: State[SettingsTab]): UiDescription =
      let active = ctx.value(tab)
      host(settingsView(0x0653'u32, false, "", true, nil, nil, activeTab = active,
        onSelectTab = proc(next: SettingsTab) =
          discard tab.update(proc(old: SettingsTab): SettingsTab = next))))
  let rt = newRuntime(app(TabProbe))
  defer: rt.closeRuntime()
  var live = initTable[NodeHandle, string]()
  var handles = initTable[string, NodeHandle]()
  proc shortKey(nodeId: string): string =
    let sep = max(nodeId.rfind('/'), nodeId.rfind(':'))
    if sep >= 0: nodeId[sep + 1 .. ^1] else: nodeId
  let probeBackend = Backend(
    capabilities: win32,
    prepare: proc(batch: UpdateBatch) = discard,
    apply: proc(batch: UpdateBatch) =
      for op in batch.ops:
        case op.kind
        of opCreate:
          let key = shortKey(op.nodeId)
          live[op.handle] = key
          handles[key] = op.handle
        of opRemove:
          if op.handle in live:
            handles.del(live[op.handle])
            live.del(op.handle)
        else: discard,
    discardPrepared: proc() = discard,
    restore: proc(entities: Table[NodeHandle, MountedEntity]) = discard,
    dispose: proc() = discard)
  let session = attachBackend(rt, probeBackend,
    HostContext(sessionId: 1, mountGeneration: 1, scale: 1.0,
      contentWidth: float64(settingsWidth), contentHeight: float64(settingsHeight)))
  defer: session.dispose()
  var sync = SettingsStyleSync()
  var styleCalls = 0
  let onStyle = proc() {.raises: [].} = inc styleCalls
  sync.syncSettingsStyle(rt.acceptedRevision, onStyle)
  doAssert styleCalls == 1 and "general" in handles and "software-update" notin handles
  var seqNo = 0'u64
  proc clickTab(key: string) =
    inc seqNo
    doAssert session.enqueue(EventEnvelope(
      session: 1, mountGeneration: 1, observedRevision: rt.acceptedRevision,
      sequence: seqNo, handle: handles[key], bindingGeneration: 1,
      source: sourceUser, event: Event(kind: click)))
  clickTab("nav-update")
  # Enqueue alone must not have reconciled or restyled yet.
  doAssert styleCalls == 1 and "software-update" notin handles
  runSettingsSafeTick(sync, proc() = session.drainEvents(),
    proc(): uint64 = rt.acceptedRevision, onStyle)
  doAssert styleCalls == 2 and "software-update" in handles and "general" notin handles
  # Idle safe tick must not redundantly restyle.
  runSettingsSafeTick(sync, proc() = session.drainEvents(),
    proc(): uint64 = rt.acceptedRevision, onStyle)
  doAssert styleCalls == 2
  clickTab("nav-general")
  runSettingsSafeTick(sync, proc() = session.drainEvents(),
    proc(): uint64 = rt.acceptedRevision, onStyle)
  doAssert styleCalls == 3 and "general" in handles and "software-update" notin handles
  echo "Settings safe-tick runtime drain and style sync passed"

block:
  # Select popup color remap contract (High Contrast system colors + Light/Dark).
  let lightLeaf = palette(shadcnLight)
  let darkLeaf = palette(shadcnDark)
  let hcTarget = SelectPopupColors(
    surface: 0x000000'u32, active: 0x00ffff'u32, border: 0xffffff'u32,
    ink: 0xffffff'u32, activeInk: 0x000000'u32, muted: 0x00ff00'u32)
  for leafPal in [lightLeaf, darkLeaf]:
    var st = SelectPopupRemap()
    doAssert remapSelectBrush(st, colorRef(leafPal.surface), lightLeaf, darkLeaf, hcTarget) == hcTarget.surface
    doAssert not st.rowHighlighted
    doAssert remapSelectInk(st, colorRef(leafPal.ink), lightLeaf, darkLeaf, hcTarget) == hcTarget.ink
    doAssert remapSelectInk(st, colorRef(leafPal.muted), lightLeaf, darkLeaf, hcTarget) == hcTarget.muted
    # Selected/candidate row highlights fill AND switches both text and checkmark ink to COLOR_HIGHLIGHTTEXT.
    doAssert remapSelectBrush(st, colorRef(leafPal.accentSoft), lightLeaf, darkLeaf, hcTarget) == hcTarget.active
    doAssert st.rowHighlighted
    doAssert remapSelectInk(st, colorRef(leafPal.ink), lightLeaf, darkLeaf, hcTarget) == hcTarget.activeInk
    # Border resets row highlight state and uses COLOR_WINDOWTEXT.
    doAssert remapSelectBrush(st, colorRef(leafPal.line), lightLeaf, darkLeaf, hcTarget) == hcTarget.border
    doAssert not st.rowHighlighted
  echo "Select popup high-contrast color remap contract passed"

when defined(windows) and defined(amd64):
  {.push stdcall, dynlib: "user32.dll", importc, raises: [].}
  proc EnumChildWindows(parent: HWND, cb: proc(w: HWND, lp: LPARAM): BOOL {.stdcall.}, lp: LPARAM): BOOL
  proc GetPropW(w: HWND, name: WidePtr): HANDLE
  {.pop.}

  type ChildLookup = object
    wanted: WideCString
    found: HWND

  proc findChildByText(w: HWND, lp: LPARAM): BOOL {.stdcall.} =
    let lookup = cast[ptr ChildLookup](lp)
    var buf: array[128, WCHAR]
    let n = GetWindowTextW(w, addr buf[0], int32(buf.len))
    if n > 0:
      var i = 0
      while i < n and uint16(lookup.wanted[i]) != 0'u16 and buf[i] == uint16(lookup.wanted[i]): inc i
      if i == n and uint16(lookup.wanted[i]) == 0'u16:
        lookup.found = w
        return 0
    1

  proc childByText(root: HWND, text: string): HWND =
    var lookup = ChildLookup(wanted: newWideCString(text))
    discard EnumChildWindows(root, findChildByText, cast[LPARAM](addr lookup))
    lookup.found

  proc settingsFaceProp(w: HWND): uint =
    let key = newWideCString("Shiguang.SettingsFace")
    cast[uint](GetPropW(w, cast[WidePtr](addr key[0])))

  block:
    # Shared-hook probe on Win32Host + Win32Backend + production Settings + bindSettingsSafeTick:
    # verifies BM_CLICK -> WM_HOST_TICK -> safeTurn -> bindSettingsSafeTick restyles newly mounted
    # Update controls without manual sg_style_settings(). Note: this Nim test binary does not embed
    # the production ComCtl32 v6 manifest and does not substitute for real Windows UI verification.
    let application = app(app_host.Settings, title = settingsTitle,
      width = settingsWidth, height = settingsHeight).withNativeTheme(shadcnLight)
    let activity = newUiScopeActivity()
    let guarded = guardUiApplication(application, proc(): uint64 = uint64(GetCurrentThreadId()), activity)
    let rt = newRuntime(guarded)
    defer: rt.closeRuntime()
    const MaxPumpMessages = 64
    const MaxCoalescedHostTicks = 512
    const ProbeQuitMessage = WM_APP + 101'u32
    const ProbePeekNoRemove = 0'u32
    const ExitQueueEmpty = 98
    const ExitBudgetExceeded = 99
    var root: HWND
    var pumpSteps = 0
    var hostTicks = 0
    var coalescedHostTicks = 0
    var quitObserved = false
    var queueEmptyWithoutQuit = false
    var budgetExceeded = false
    var hostCalls = systemWin32Calls()
    let realPeekMessage = hostCalls.peekMessage
    hostCalls.showWindow = proc(window: HWND, command: int32): BOOL = 0
    hostCalls.getMessage = proc(message: ptr MSG, w: HWND, first, last: UINT): BOOL =
      if pumpSteps >= MaxPumpMessages:
        budgetExceeded = true
        message[] = default(MSG)
        message.message = WM_QUIT
        message.wParam = WPARAM(ExitBudgetExceeded)
        return 0
      inc pumpSteps
      if realPeekMessage(message, w, first, last, PM_REMOVE) == 0:
        queueEmptyWithoutQuit = true
        message[] = default(MSG)
        message.message = WM_QUIT
        message.wParam = WPARAM(ExitQueueEmpty)
        return 0
      if root != nil and message.hwnd == root and message.message == ProbeQuitMessage:
        quitObserved = true
        message.message = WM_QUIT
        return 0
      if message.message == WM_QUIT:
        return 0
      if message.message == WM_HOST_TICK:
        inc hostTicks
        if root != nil and message.hwnd == root:
          # Leaf's shadcn controlProc posts an uncoalesced WM_HOST_TICK on every child
          # WM_WINDOWPOSCHANGED during mount/relayout. Coalesce consecutive duplicate
          # ticks at the queue head without crossing the FIFO sentinel or other messages.
          var coalesced = 0
          var nextMsg: MSG
          while coalesced < MaxCoalescedHostTicks and
              realPeekMessage(addr nextMsg, w, first, last, ProbePeekNoRemove) != 0 and
              nextMsg.hwnd == root and nextMsg.message == WM_HOST_TICK:
            if realPeekMessage(addr nextMsg, root, WM_HOST_TICK, WM_HOST_TICK, PM_REMOVE) == 0:
              break
            inc coalesced
            inc coalescedHostTicks
      1
    hostCalls.waitFrameMessage = proc(timer: HANDLE): DWORD =
      raise newException(CatchableError, "unexpected blocking frame wait in bounded settings probe")
    let window = createHost(application.title,
      DipSize(width: float64(application.width), height: float64(application.height)),
      calls = hostCalls, theme = application.nativeTheme)
    defer: window.destroyHost()
    window.setStartupScope(proc(): bool = activity.busy or rt.installingUpdate or rt.installingHook)
    var backendCalls = systemWin32Calls()
    app_host.installSelectPopupCalls(backendCalls)
    let native = newWin32Backend(window, calls = backendCalls, renderer = application.canvasRenderer)
    let session = native.attachWin32Backend(rt)
    native.bindSession(session)
    window.bindSession(session, proc(): bool = activity.busy)
    var styleSync = SettingsStyleSync()
    app_host.bindSettingsSafeTick(window, native, rt, addr styleSync)
    doAssert app_host.sg_bind_settings(window.windowHandle()) != 0
    root = window.windowHandle()
    proc pumpBounded(exitCode: int32, label: string) =
      pumpSteps = 0
      hostTicks = 0
      coalescedHostTicks = 0
      quitObserved = false
      queueEmptyWithoutQuit = false
      budgetExceeded = false
      doAssert PostMessageW(root, ProbeQuitMessage, WPARAM(exitCode), 0) != 0,
        label & " failed to post probe sentinel"
      let actual = window.pump()
      doAssert actual == int(exitCode) and quitObserved and
        not queueEmptyWithoutQuit and not budgetExceeded,
        label & " bounded pump failed: exit=" & $actual & " expected=" & $exitCode &
        " steps=" & $pumpSteps & " hostTicks=" & $hostTicks &
        " coalescedHostTicks=" & $coalescedHostTicks &
        " quitObserved=" & $quitObserved &
        " queueEmpty=" & $queueEmptyWithoutQuit &
        " budgetExceeded=" & $budgetExceeded
    # Initial turn runs through window.pump() + bindSettingsSafeTick (no manual sg_style_settings call).
    pumpBounded(11, "initial-mount")
    let navGeneral = childByText(root, "常规")
    let navUpdate = childByText(root, "软件更新")
    doAssert navGeneral != nil and navUpdate != nil
    doAssert settingsFaceProp(navGeneral) == 6'u and settingsFaceProp(navUpdate) == 7'u
    # Click "软件更新" through native BM_CLICK -> WM_COMMAND -> WM_HOST_TICK -> safeTurn -> bindSettingsSafeTick.
    let revBefore = rt.acceptedRevision
    discard SendMessageW(navUpdate, BM_CLICK, 0, 0)
    pumpBounded(12, "tab-switch")
    doAssert hostTicks > 0, "expected WM_HOST_TICK after BM_CLICK, got hostTicks=" & $hostTicks
    doAssert rt.acceptedRevision > revBefore,
      "expected Leaf revision advance after BM_CLICK: before=" & $revBefore & " after=" & $rt.acceptedRevision
    doAssert settingsFaceProp(navGeneral) == 7'u and settingsFaceProp(navUpdate) == 6'u
    let channelBtn = childByText(root, "测试版")
    let autoSwitch = childByText(root, "自动下载更新")
    doAssert channelBtn != nil and settingsFaceProp(channelBtn) == 3'u
    doAssert autoSwitch != nil and settingsFaceProp(autoSwitch) == 8'u
    echo "Win32 bounded host safe-tick hook probe passed"
