import leaf
import leaf/backend
import leaf/styles
import std/[options, os, strutils, tables, tempfiles, times]
import ../src/[updater, update_policy]
import leaf/native_layout
import leaf/backends/win32/capabilities
import ../src/settings_view

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
    let change = pixelEdges(box("change").rect, dpi)
    let quit = pixelEdges(box("quit").rect, dpi)
    doAssert change.right == (settingsWidth - 40) * dpi div 96 and
      quit.right == (settingsWidth - 28) * dpi div 96
    doAssert change.bottom - change.top == 40 * dpi div 96
    for key in ["startup", "update-channel-trigger", "auto-download", "update-action", "update-progress"]:
      if ids.hasKey(key):
        doAssert pixelEdges(box(key).rect, dpi).right == change.right, "right edge: " & key
    doAssert pixelEdges(box("startup-label").rect, dpi).left <
      pixelEdges(box("startup").rect, dpi).left
    doAssert pixelEdges(box("auto-download-label").rect, dpi).left <
      pixelEdges(box("auto-download").rect, dpi).left
    doAssert quit.bottom - quit.top == change.bottom - change.top
    for key in ["change", "quit", "startup", "startup-status", "update-channel-trigger",
        "auto-download", "update-action", "update-progress", "update-feedback"]:
      if not ids.hasKey(key): continue
      let b = box(key)
      let pixels = pixelEdges(b.rect, dpi)
      let clip = pixelEdges(b.clip, dpi)
      doAssert pixels.left >= clip.left and pixels.top >= clip.top and
        pixels.right <= clip.right and pixels.bottom <= clip.bottom, "clipped control: " & key

let win32 = win32Capabilities(shadcnLight)
let initialView = settingsView(0x0653'u32, false, "", true, nil, nil)
doAssert find(initialView, "general") != nil, "settings must group general preferences"
doAssert find(initialView, "software-update") != nil, "settings must group software updates"
for (chord, recording, status, trayReady, expected, absent) in [
    (0x0653'u32, false, "", true, @["Ctrl", "Shift", "S", "更改"], @["未注册", "取消录入"]),
    (0x0f20'u32, false, "", true, @["Ctrl", "Alt", "Shift", "Win", "Space", "更改"], @["未注册"]),
    (0'u32, false, "", false, @["未注册", "●  通知区域图标不可用", "更改"], @["Ctrl"]),
    (0x0653'u32, true, "", true, @["请按下新组合键  ·  Esc 取消", "取消录入"], @["Ctrl", "更改"]),
    (0x0653'u32, false, shortcutFeedback(1, 0x0653), true,
      @["Ctrl", "Shift", "S", "ⓘ  新组合键已被占用，仍使用 Ctrl + Shift + S。", "重新录入"],
      @["取消录入", "更改"])]:
  let view = settingsView(chord, recording, status, trayReady, nil, nil)
  let minimize = find(view, "minimize")
  let close = find(view, "close")
  doAssert minimize != nil and close != nil, "independent accessible caption buttons"
  doAssert minimize.text == "最小化" and close.text == "关闭"
  doAssert minimize.styles == close.styles
  doAssert find(view, "change").styles == find(view, "quit").styles
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
    startup = state, startupError = error)
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
  proc render(snapshot: UpdateSnapshot; allFeedback = false): Node =
    settingsView(0x0f20'u32, false, (if allFeedback: "录入失败，仍使用旧快捷键。" else: ""), true,
      nil, nil, startup = (if allFeedback: 2 else: 0), update =
      UpdateUi(snapshot: snapshot, feedback: feedback, channelOpen: dropdown,
        onAction: proc(action: UpdateAction) = requested.add(action),
        onChannelOpen: proc(e: Event) = dropdown = e.open))
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
  doAssert find(view, "last-check").text != "最近检查：尚未检查"
  activate(view, "update-action", Event(kind: click))
  doAssert requested[^1] == uaDownload
  activate(view, "update-channel", Event(kind: change, value: "stable"))
  doAssert requested[^1] == uaStable
  let download = engine.requestDownload().get
  for (phase, label, action) in [
      (upIdle, "暂无可用更新", ""), (upChecking, "正在检查更新", ""),
      (upCheckFailed, "暂时无法检查更新", ""), (upDownloading, "正在下载更新", ""),
      (upDownloadFailed, "下载未成功", "重试下载"), (upReady, "新版本已就绪", "重启更新"),
      (upRestarting, "正在准备重启", "")]:
    var snapshot = engine.snapshot
    snapshot.phase = phase
    snapshot.error = "C:/private/internal-test-key"
    let v = render(snapshot)
    doAssert find(v, "update-status").text == label
    doAssert ("C:/private/internal-test-key" notin captions(v).join(" "))
    doAssert (find(v, "update-progress") != nil) == (phase == upDownloading)
    discard checkCapabilities(mount(v), win32, strict)
    requireSettingsGeometry(v)
    if action == "":
      doAssert find(v, "update-action") == nil
    else:
      doAssert find(v, "update-action").text == action
      activate(v, "update-action", Event(kind: click))
      doAssert requested[^1] == (if phase == upReady: uaRestart else: uaDownload)
  doAssert engine.complete(download)
  doAssert engine.setChannel(ucStable)
  view = render(engine.snapshot)
  doAssert selected(view) == "正式版" and find(view, "update-action") == nil
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
  doAssert selected(view) == "测试版"
  doAssert find(view, "update-feedback").text.contains("无法保存")
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
    doAssert "暂无可用更新" notin captions(failedView)
    doAssert find(failedView, "last-check").text ==
      "最近检查：" & fromUnix(stamp).local.format("yyyy-MM-dd HH:mm")
    doAssert find(failedView, "update-action") == nil
    doAssert find(failedView, "update-candidate").text == "本次检查未能完成，系统会自动重试。"
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
    assertStatus("正在下载更新")
    # A second click can be refused while the accepted download is in flight.
    doAssert downloads.requestDownload().isNone
    publish(some(updateActionFeedback(uaDownload, false)))
    assertStatus("正在下载更新", true)
    doAssert downloads.complete(first, error = "network diagnostic")
    publish()
    assertStatus("下载未成功")
    for succeeds in [false, true]:
      let retry = downloads.requestDownload().get
      publish(some(updateActionFeedback(uaDownload, true)))
      assertStatus("正在下载更新")
      doAssert downloads.complete(retry, error = (if succeeds: "" else: "test diagnostic"))
      publish()
      assertStatus(if succeeds: "新版本已就绪" else: "下载未成功")
    publish(some(updateActionFeedback(uaRestart, false, busy = true)))
    assertStatus("新版本已就绪", true)
    publish()
    doAssert "请先完成" in find(render(last), "update-feedback").text
    doAssert downloads.setChannel(ucStable)
    publish() # The bridge notifies before the channel command returns.
    assertStatus("暂无可用更新")
    publish(some(updateActionFeedback(uaStable, true)))
    assertStatus("暂无可用更新")
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
