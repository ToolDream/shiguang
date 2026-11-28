import leaf
import leaf/native_theme
import std/[options, strutils, times]
import updater, update_policy
import leaf/ui as standard

proc mainKey(vk: uint32): string =
  case vk
  of 0x30..0x39, 0x41..0x5a: $char(vk)
  of 0x70..0x87: "F" & $(vk - 0x6f)
  of 0x20: "Space"
  of 0x21: "PgUp"
  of 0x22: "PgDn"
  of 0x23: "End"
  of 0x24: "Home"
  of 0x25: "←"
  of 0x26: "↑"
  of 0x27: "→"
  of 0x28: "↓"
  of 0x2d: "Ins"
  of 0x2e: "Del"
  else: "?"

proc chordNames(chord: uint32): seq[string] =
  for (bit, label) in [(2'u32, "Ctrl"), (1'u32, "Alt"), (4'u32, "Shift"), (8'u32, "Win")]:
    if ((chord shr 8) and bit) != 0: result.add(label)
  result.add(mainKey(chord and 255))

proc keycapWidth(name: string): int =
  case name.len
  of 0..1: 32
  of 2: 36
  of 3: 42
  of 4: 48
  else: 56

proc caps(chord: uint32): seq[Node] =
  if chord == 0:
    return @[text("未注册", key = "unregistered", styles = style({"height": "20"}))]
  for i, name in chordNames(chord):
    if i > 0:
      result.add(text("+", key = "plus-" & $i, styles = style({"width": "10", "height": "20"})))
    # Keep standard.kbd semantics while sizing each keycap to match the mockup proportions.
    let keycap = standard.kbd(name, key = "cap-" & $i)
    result.add(ui(NodeKind.tag, key = keycap.key, text = name,
      styles = style({"width": $keycapWidth(name), "height": "32"}),
      attributes = keycap.attributes))

proc shortcutFeedback*(outcome: cint, active: uint32): string =
  let retained = if active == 0: "当前没有可用快捷键。"
    else: "仍使用 " & chordNames(active).join(" + ") & "。"
  case outcome
  of 0: ""
  of 1: "新组合键已被占用，" & retained
  of 2: "无法保存新组合键，" & retained
  of 3: "新组合键无效，" & retained
  else: "当前无法更改，" & retained

type
  SettingsTab* = enum
    stGeneral, stUpdate
  UpdateAction* = enum
    uaStable, uaTest, uaAutoOff, uaAutoOn, uaDownload, uaRestart, uaCheck,
    uaCancelAutoInstall
  UpdateFeedback* = object
    action: UpdateAction
    message*: string
  UpdateUi* = object
    snapshot*: UpdateSnapshot
    feedback*: string
    channelOpen*: bool
    onChannelOpen*: Callback
    onAction*: proc(action: UpdateAction)

func updateActionFeedback*(action: UpdateAction, accepted: bool; busy = false): UpdateFeedback =
  # Accepted actions are described by the snapshot, not by a second success message.
  if accepted: return
  result.action = action
  result.message = case action
    of uaStable, uaTest, uaAutoOff, uaAutoOn: "无法保存更新设置，已保留原设置。"
    of uaDownload: "暂时无法开始下载，请稍后再试。"
    of uaCheck: "暂时无法检查更新，请稍后再试。"
    of uaCancelAutoInstall: "当前没有待取消的自动安装。"
    of uaRestart:
      if busy: "请先完成截图、编辑、保存或快捷键录入，再重启更新。"
      else: "暂时无法开始重启更新，请稍后再试。"

func nextUpdateFeedback*(current: UpdateFeedback; previous, snapshot: UpdateSnapshot;
                         replacement = none(UpdateFeedback)): UpdateFeedback =
  if replacement.isSome: return replacement.get
  if current.message.len == 0: return
  # Save errors survive unrelated checks; download/restart rejections apply only
  # to the candidate and phase the user tried to act on.
  let stale = case current.action
    of uaStable, uaTest: previous.settings.channel != snapshot.settings.channel
    of uaAutoOff, uaAutoOn: previous.settings.autoDownload != snapshot.settings.autoDownload
    of uaDownload, uaRestart, uaCheck, uaCancelAutoInstall:
      previous.phase != snapshot.phase or previous.candidate != snapshot.candidate or
        previous.settings.channel != snapshot.settings.channel or
        previous.autoInstall != snapshot.autoInstall
  if not stale: result = current

proc send(update: UpdateUi, action: UpdateAction) =
  if update.onAction != nil: update.onAction(action)

func channelLabel(channel: UpdateChannel): string =
  if channel == ucTest: "测试版" else: "正式版"

func buildLabel(build: UpdateBuild): string =
  if build.version.len == 0: "未知"
  else: build.version & " " & channelLabel(build.channel)

proc lastCheckLabel(stamp: int64): string =
  # Persisted input need not be a date the local calendar can represent.
  if stamp <= 0: "尚未检查"
  elif stamp >= 253402214400'i64: "时间不可用"
  else: fromUnix(stamp).local.format("yyyy-MM-dd HH:mm")

const
  settingsTitle* = "拾光截图 / 设置"
  settingsWidth* = 680
  settingsHeight* = 436
  settingsSidebarWidth* = 180

proc settingsCard(key: string, children: seq[Node], gap = "12"): Node =
  ui(NodeKind.column, key = key, children = children, attributes = standard.card(@[]).attributes,
    styles = style({"width": "full", "padding": "16", "gap": gap}))

proc generalPage(chord: uint32, recording: bool, status: string,
                 toggleRecording: Callback, startup: cint, startupError: bool,
                 onStartup: Callback): seq[Node] =
  let entry = if recording:
    @[text("请按下新组合键  ·  Esc 取消", key = "recording", styles = style({"height": "22"}))]
  else: caps(chord)
  let changeLabel = if recording: "取消录入" elif status.len != 0: "重新录入" else: "更改"
  let startupStatus = case startup
    of 0: "未注册"
    of 1: "已注册，实际启动由 Windows 设置或策略决定"
    of 2: "已有其他路径的同名启动项，未改动"
    else: "无法读取启动项状态"
  let startupFeedback =
    if startupError or startup > 1:
      @[text((if startupError: "无法更新启动项。当前：" else: "") & startupStatus,
        key = "startup-status", styles = style({"width": "full", "height": "24"}))]
    else: @[]
  let shortcutFeedback =
    if status.len > 0: @[text("ⓘ  " & status, key = "feedback",
      styles = style({"width": "full", "height": "24"}))]
    else: @[]
  let generalCard = settingsCard("general-settings", @[
    column(@[
      text("区域截图快捷键", key = "heading", styles = style({"width": "full", "height": "20"})),
      text("按下当前组合键，即可开始框选。", key = "description",
        styles = style({"width": "full", "height": "18"}))
    ], key = "shortcut-labels", styles = style({"width": "full", "gap": "2"})),
    row(@[
      row(entry, key = "caps", styles = style({"width": "full", "flex_grow": "1",
        "gap": "4", "align": "center"})),
      leaf.button(changeLabel, onClick = toggleRecording, key = "change",
        styles = style({"width": "88", "height": "34"}))
    ], key = "shortcut", styles = style({"width": "full", "height": "34", "gap": "10",
      "justify": "between", "align": "center"}))
  ] & shortcutFeedback & @[
    standard.separator(key = "general-divider"),
    row(@[
      column(@[
        text("登录 Windows 时启动拾光", key = "startup-label",
          styles = style({"width": "full", "height": "20"})),
        text("开机自动静默驻留托盘，不弹出设置窗口", key = "startup-description",
          styles = style({"width": "full", "height": "18"}))
      ], key = "startup-labels", styles = style({"width": "full", "flex_grow": "1", "gap": "2"})),
      leaf.switch("登录 Windows 时启动拾光", checked = startup == 1, key = "startup",
        disabled = startup > 1, onChange = onStartup,
        styles = style({"width": "44", "height": "28"}))
    ], key = "startup-row", styles = style({"width": "full", "height": "40", "gap": "12",
      "justify": "between", "align": "center"}))
  ] & startupFeedback, gap = "10")
  @[
    column(@[
      text("常规", key = "general", styles = style({"width": "full", "height": "26"})),
      text("快捷键与系统启动项设置，关闭本窗口后拾光仍在托盘待命。", key = "general-subtitle",
        styles = style({"width": "full", "height": "18"}))
    ], key = "general-header", styles = style({"width": "full", "gap": "2"})),
    generalCard,
    column(@[], key = "general-spacer", styles = style({"width": "full", "height": "full", "flex_grow": "1"})),
    text("提示：在截图编辑中按 Enter 直接复制，Ctrl + S 另存为 PNG。", key = "general-tip",
      styles = style({"width": "full", "height": "18"}))
  ]

proc updatePage(update: UpdateUi): seq[Node] =
  let snapshot = update.snapshot
  let restarting = snapshot.phase == upRestarting
  # The transfer and its verification are one worker stage: never fake a finer split.
  let waiting = snapshot.phase == upReady and snapshot.installReady
  let (status, detail) = case snapshot.phase
    of upIdle:
      ((if snapshot.settings.lastCheck == 0: "等待自动检查" else: "已是最新版本"),
       (if snapshot.settings.lastCheck == 0: "每天自动检查一次更新通道。" else: ""))
    of upChecking:
      ("正在检查更新",
       (if snapshot.autoInstall: "会接着下载并验证；取消只停止自动重启。" else: ""))
    of upCheckFailed: ("暂时无法检查更新", "")
    of upAvailable:
      ("发现新版本",
       (if snapshot.autoInstall: "会自动下载并验证；取消只停止自动重启。" else: ""))
    of upDownloading:
      ("正在下载并验证",
       (if snapshot.autoInstall: "下载会继续；取消只停止自动重启。" else: ""))
    of upDownloadFailed: ("下载未成功", "可以重试下载。")
    of upReady:
      ("新版本已就绪", (if waiting: "已下载，完成当前操作后自动安装。" else: "重启后生效"))
    of upRestarting: ("正在准备重启", "")
  # The round's own outcome stays visible next to a cached ready stage and is never
  # hidden by the old candidate; it comes first so a long identity cannot truncate it.
  let resultText = case snapshot.result
    of rrCheckFailed: "ⓘ  本次检查未能完成，没有安装任何更新。"
    of rrInstallFailed: "ⓘ  更新未能启动，已验证的安装包仍在，可稍后手动「重启更新」。"
    of rrUpToDate: (if snapshot.phase == upIdle: "" else: "本次检查没有可安装的新版本。")
    else: ""
  var infoParts: seq[string]
  if resultText.len > 0: infoParts.add(resultText)
  if detail.len > 0 and resultText.len == 0: infoParts.add(detail)
  if snapshot.candidate.isSome: infoParts.add(buildLabel(snapshot.candidate.get))
  let infoText = infoParts.join("  ·  ")
  var statusRow = @[text(status, key = "update-status",
    styles = style({"width": "full", "flex_grow": "1", "height": "22"}))]
  if snapshot.phase in {upChecking, upDownloading}:
    # The service reports only check/transfer phase, not downloaded bytes.
    statusRow.add(ui(NodeKind.spinner, key = "update-progress",
      styles = style({"width": "104", "height": "10"})))
  var state: seq[Node] = @[row(statusRow, key = "update-state-status",
    styles = style({"width": "full", "height": "22", "gap": "12", "justify": "between",
      "align": "center"}))]
  if infoText.len > 0:
    state.add(text(infoText, key = "update-candidate",
      styles = style({"width": "full", "height": "18"})))
  var actions: seq[Node]
  if not restarting:
    actions.add(leaf.button("检查并安装更新", key = "update-check",
      onClick = proc(e: Event) = update.send(uaCheck),
      styles = style({"width": "128", "height": "32"})))
    case snapshot.phase
    of upAvailable:
      if not snapshot.autoInstall:
        actions.add(leaf.button("下载更新", key = "update-action",
          onClick = proc(e: Event) = update.send(uaDownload),
          styles = style({"width": "96", "height": "32"})))
    of upDownloadFailed:
      actions.add(leaf.button("重试下载", key = "update-action",
        onClick = proc(e: Event) = update.send(uaDownload),
        styles = style({"width": "96", "height": "32"})))
    of upReady:
      if not snapshot.installReady:
        actions.add(leaf.button("重启更新", key = "update-action",
          onClick = proc(e: Event) = update.send(uaRestart),
          styles = style({"width": "96", "height": "32"})))
    else: discard
    if snapshot.autoInstall:
      actions.add(leaf.button("取消自动安装", key = "update-cancel",
        onClick = proc(e: Event) = update.send(uaCancelAutoInstall),
        styles = style({"width": "104", "height": "32"})))
  if actions.len > 0:
    # The hint stays beside the buttons, so a long candidate or result line never
    # competes with them for width.
    var actionRow = @[text("找到新版本后空闲自动重启", key = "update-hint",
      styles = style({"width": "full", "flex_grow": "1", "height": "18"}))]
    actionRow.add(actions)
    state.add(row(actionRow, key = "update-actions", styles = style({"width": "full",
      "height": "32", "gap": "8", "justify": "start", "align": "center"})))
  let switchStyle = style({"width": "44", "height": "28"})
  @[
    row(@[
      text("软件更新", key = "software-update", styles = style({"width": "104", "height": "26"})),
      text("当前版本 " & buildLabel(snapshot.installed), key = "installed-version",
        styles = style({"width": "full", "flex_grow": "1", "height": "20"}))
    ], key = "update-header", styles = style({"width": "full", "height": "28", "gap": "12",
      "justify": "between", "align": "center"})),
    settingsCard("update-settings", @[
      row(@[
        text("更新通道", key = "channel-label", styles = style({"width": "full", "flex_grow": "1", "height": "22"})),
        standard.select(standard.selectTrigger(disabled = restarting, key = "update-channel-trigger"),
          standard.selectContent(@[standard.selectItem("stable", "正式版"),
            standard.selectItem("test", "测试版")]),
          value = (if snapshot.settings.channel == ucTest: "test" else: "stable"),
          open = update.channelOpen, key = "update-channel", label = "更新通道", width = 148,
          onOpenChange = update.onChannelOpen,
          onChange = proc(e: Event) =
            case e.value
            of "stable":
              if snapshot.settings.channel != ucStable: update.send(uaStable)
            of "test":
              if snapshot.settings.channel != ucTest: update.send(uaTest)
            else: discard)
      ], key = "channel-row", styles = style({"width": "full", "height": "36", "gap": "12",
        "justify": "between", "align": "center"})),
      standard.separator(key = "channel-divider"),
      row(@[
        column(@[
          text("自动下载更新", key = "auto-download-label",
            styles = style({"width": "full", "height": "20"})),
          text("每天检查一次，后台下载完成后由你决定何时重启", key = "auto-download-description",
            styles = style({"width": "full", "height": "18"}))
        ], key = "auto-download-labels", styles = style({"width": "full", "flex_grow": "1", "gap": "2"})),
        leaf.switch("自动下载更新", checked = snapshot.settings.autoDownload, key = "auto-download",
          disabled = restarting, onChange = proc(e: Event) =
            update.send(if e.checked: uaAutoOn else: uaAutoOff), styles = switchStyle)
      ], key = "auto-download-row", styles = style({"width": "full", "height": "36", "gap": "12",
        "justify": "between", "align": "center"}))
    ], gap = "12"),
    settingsCard("update-state-card", state, gap = "4"),
    text("最近检查：" & lastCheckLabel(snapshot.settings.lastCheck), key = "last-check",
      styles = style({"width": "full", "height": "18"}))
  ] & (if update.feedback.len == 0: @[] else: @[
    text("ⓘ  " & update.feedback, key = "update-feedback",
      styles = style({"width": "full", "height": "20"}))])

proc settingsView*(chord: uint32, recording: bool, status: string, trayReady: bool,
                   toggleRecording, quit: Callback, minimize: Callback = nil,
                   close: Callback = nil, startup: cint = 0, startupError = false,
                   onStartup: Callback = nil,
                   update = UpdateUi(snapshot: UpdateSnapshot(settings: defaultUpdateSettings())),
                   activeTab: SettingsTab = stGeneral,
                   onSelectTab: proc(tab: SettingsTab) = nil): Node =
  let trayLabel = if trayReady: "●  托盘运行中"
    else: "●  通知区域图标不可用"
  let hasNewUpdate = update.snapshot.phase in {upAvailable, upReady}
  let updateNavLabel = if hasNewUpdate: "软件更新  ·  新版本" else: "软件更新"
  let navButtonStyle = style({"width": "full", "height": "34"})
  let captionButton = style({"width": "42", "height": "36"})
  let selectGeneral: Callback = proc(e: Event) =
    if onSelectTab != nil: onSelectTab(stGeneral)
  let selectUpdate: Callback = proc(e: Event) =
    if onSelectTab != nil: onSelectTab(stUpdate)
  let sidebar = column(@[
    text("拾光截图", key = "brand-title", styles = style({"width": "full", "height": "42"})),
    column(@[
      leaf.button("常规", key = "nav-general", onClick = selectGeneral, styles = navButtonStyle),
      leaf.button(updateNavLabel, key = "nav-update", onClick = selectUpdate, styles = navButtonStyle)
    ], key = "sidebar-nav", styles = style({"width": "full", "gap": "4"})),
    column(@[], key = "sidebar-spacer", styles = style({"width": "full", "height": "full", "flex_grow": "1"})),
    standard.separator(key = "footer-divider"),
    standard.cardDescription(trayLabel, key = "tray"),
    leaf.button("退出拾光", key = "quit", onClick = quit,
      styles = style({"width": "full", "height": "34"}))
  ], key = "sidebar", styles = style({"width": $settingsSidebarWidth, "height": "full",
    "padding": "14", "gap": "8"}))
  let bodyChildren = case activeTab
    of stGeneral:
      generalPage(chord, recording, status, toggleRecording, startup, startupError, onStartup)
    of stUpdate:
      updatePage(update)
  let mainPane = column(@[
    row(@[
      text(settingsTitle, key = "caption", styles = style({"width": "full", "height": "36", "flex_grow": "1"})),
      leaf.button("最小化", key = "minimize", onClick = minimize, styles = captionButton),
      leaf.button("关闭", key = "close", onClick = close, styles = captionButton)
    ], key = "caption-row", styles = style({"width": "full", "height": "36", "align": "center"})),
    column(bodyChildren, key = "body", styles = style({"width": "full", "height": "full", "flex_grow": "1",
      "padding": "20", "gap": "12"}))
  ], key = "main-pane", styles = style({"width": "full", "flex_grow": "1", "height": "full"}))
  row(@[sidebar, mainPane], key = "settings", styles = style({"width": "full", "height": "full"}))

type
  SettingsStyleSync* = object
    styledRevision*: uint64
    hasStyled*: bool
  SelectPopupColors* = object
    surface*, active*, border*, ink*, activeInk*, muted*: uint32
  SelectPopupRemap* = object
    rowHighlighted*: bool

proc syncSettingsStyle*(sync: var SettingsStyleSync, revision: uint64,
                        applyStyle: proc() {.raises: [].}; force = false) {.raises: [].} =
  if force or not sync.hasStyled or sync.styledRevision != revision:
    sync.styledRevision = revision
    sync.hasStyled = true
    applyStyle()

proc runSettingsSafeTick*(sync: var SettingsStyleSync, runTurn: proc(),
                          currentRevision: proc(): uint64,
                          applyStyle: proc() {.raises: [].}) =
  ## Leaf drains button clicks, tab switches, and select updates inside safeListTurn
  ## (outside dispatchMessage/dialogMessage). Re-apply Win32 styling whenever that
  ## turn commits a new revision or before the first styling pass.
  let before = currentRevision()
  runTurn()
  let after = currentRevision()
  sync.syncSettingsStyle(after, applyStyle, force = after != before)

func remapSelectBrush*(state: var SelectPopupRemap, color: uint32,
                       leafLight, leafDark: NativePalette,
                       target: SelectPopupColors): uint32 =
  if color == colorRef(leafLight.accentSoft) or color == colorRef(leafDark.accentSoft):
    state.rowHighlighted = true
    target.active
  elif color == colorRef(leafLight.line) or color == colorRef(leafDark.line):
    state.rowHighlighted = false
    target.border
  else:
    state.rowHighlighted = false
    target.surface

func remapSelectInk*(state: SelectPopupRemap, color: uint32,
                     leafLight, leafDark: NativePalette,
                     target: SelectPopupColors): uint32 =
  if state.rowHighlighted:
    target.activeInk
  elif color == colorRef(leafLight.muted) or color == colorRef(leafDark.muted):
    target.muted
  else:
    target.ink
