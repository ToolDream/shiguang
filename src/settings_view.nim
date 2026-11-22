import leaf
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

proc caps(chord: uint32): seq[Node] =
  if chord == 0:
    return @[standard.cardDescription("未注册", key = "unregistered")]
  for i, name in chordNames(chord):
    if i > 0:
      result.add(text("+", key = "plus-" & $i, styles = style({"width": "10"})))
    # Win32 measures kbd from the complete text, including longer key names.
    let keycap = standard.kbd(name, key = "cap-" & $i)
    result.add(ui(NodeKind.tag, key = keycap.key, text = name,
      styles = style({"height": "34"}), attributes = keycap.attributes))

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
  UpdateAction* = enum
    uaStable, uaTest, uaAutoOff, uaAutoOn, uaDownload, uaRestart
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
    of uaDownload, uaRestart:
      previous.phase != snapshot.phase or previous.candidate != snapshot.candidate or
        previous.settings.channel != snapshot.settings.channel
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
  settingsWidth* = 704
  settingsHeight* = 678

proc settingsCard(key: string, children: seq[Node]): Node =
  ui(NodeKind.column, key = key, children = children, attributes = standard.card(@[]).attributes,
    styles = style({"width": "full", "padding": "12", "gap": "6"}))

proc updateView(update: UpdateUi): seq[Node] =
  let snapshot = update.snapshot
  let restarting = snapshot.phase == upRestarting
  let (status, detail, actionLabel) = case snapshot.phase
    of upIdle:
      ((if snapshot.settings.lastCheck == 0: "等待自动检查" else: "暂无可用更新"), "", "")
    of upChecking: ("正在检查更新", "", "")
    of upCheckFailed: ("暂时无法检查更新", "本次检查未能完成，系统会自动重试。", "")
    of upAvailable: ("发现新版本", "", "下载更新")
    of upDownloading: ("正在下载更新", "", "")
    of upDownloadFailed: ("下载未成功", "可以重试下载。", "重试下载")
    of upReady: ("新版本已就绪", "重启后生效", "重启更新")
    of upRestarting: ("正在准备重启", "", "")
  var detailText = detail
  if snapshot.candidate.isSome:
    detailText = buildLabel(snapshot.candidate.get) &
      (if detail.len == 0: "" else: "  ·  " & detail)
  let action = if snapshot.phase == upReady: uaRestart else: uaDownload
  var stateRow = @[column(@[
    text(status, key = "update-status", styles = style({"height": "24"})),
    text(detailText, key = "update-candidate", styles = style({"height": "20"}))
  ], key = "update-state-labels", styles = style({"flex_grow": "1"}))]
  if actionLabel.len > 0:
    stateRow.add(leaf.button(actionLabel, onClick = proc(e: Event) = update.send(action),
      key = "update-action", styles = style({"width": "116", "height": "40"})))
  elif snapshot.phase == upDownloading:
    # The service currently reports only transfer phase, not downloaded bytes.
    stateRow.add(ui(NodeKind.spinner, key = "update-progress", styles = style({"width": "116", "height": "8"})))
  let rowStyle = style({"width": "full", "height": "46", "justify": "between", "align": "center"})
  let switchStyle = style({"width": "40", "height": "32"})
  @[
    row(@[
      text("软件更新", key = "software-update", styles = style({"height": "26"})),
      text("当前版本 " & buildLabel(snapshot.installed), key = "installed-version",
        styles = style({"height": "22"}))
    ], key = "update-header", styles = style({"width": "full", "justify": "between", "align": "center"})),
    settingsCard("update-settings", @[
      row(@[
        text("更新通道", key = "channel-label"),
        standard.select(standard.selectTrigger(disabled = restarting, key = "update-channel-trigger"),
          standard.selectContent(@[standard.selectItem("stable", "正式版"),
            standard.selectItem("test", "测试版")]),
          value = (if snapshot.settings.channel == ucTest: "test" else: "stable"),
          open = update.channelOpen, key = "update-channel", label = "更新通道", width = 152,
          onOpenChange = update.onChannelOpen,
          onChange = proc(e: Event) =
            case e.value
            of "stable":
              if snapshot.settings.channel != ucStable: update.send(uaStable)
            of "test":
              if snapshot.settings.channel != ucTest: update.send(uaTest)
            else: discard)
      ], key = "channel-row", styles = rowStyle),
      standard.separator(key = "channel-divider"),
      row(@[
        column(@[
          text("自动下载更新", key = "auto-download-label", styles = style({"height": "24"})),
          text("每天检查一次，下载完成后由你决定何时重启", key = "auto-download-description",
            styles = style({"height": "20"}))
        ], key = "auto-download-labels"),
        leaf.switch("自动下载更新", checked = snapshot.settings.autoDownload, key = "auto-download",
          disabled = restarting, onChange = proc(e: Event) =
            update.send(if e.checked: uaAutoOn else: uaAutoOff), styles = switchStyle)
      ], key = "auto-download-row", styles = rowStyle)
    ]),
    settingsCard("update-state-card", @[row(stateRow, key = "update-state", styles = rowStyle)]),
    text("最近检查：" & lastCheckLabel(snapshot.settings.lastCheck), key = "last-check",
      styles = style({"height": "20"}))
  ] & (if update.feedback.len == 0: @[] else: @[
    text("ⓘ  " & update.feedback, key = "update-feedback",
      styles = style({"width": "full", "height": "24"}))])

proc settingsView*(chord: uint32, recording: bool, status: string, trayReady: bool,
                   toggleRecording, quit: Callback, minimize: Callback = nil,
                   close: Callback = nil, startup: cint = 0, startupError = false,
                   onStartup: Callback = nil,
                   update = UpdateUi(snapshot: UpdateSnapshot(settings: defaultUpdateSettings()))): Node =
  let entry = if recording:
    @[text("请按下新组合键  ·  Esc 取消", key = "recording")]
  else: caps(chord)
  let changeLabel = if recording: "取消录入" elif status.len != 0: "重新录入" else: "更改"
  let trayLabel = if trayReady: "●  托盘运行中 · 关闭窗口不影响截图"
    else: "●  通知区域图标不可用"
  let startupStatus = case startup
    of 0: "未注册"
    of 1: "已注册，实际启动由 Windows 设置或策略决定"
    of 2: "已有其他路径的同名启动项，未改动"
    else: "无法读取启动项状态"
  let startupFeedback =
    if startupError or startup > 1:
      @[text((if startupError: "无法更新启动项。当前：" else: "") & startupStatus,
        key = "startup-status", styles = style({"width": "full", "height": "30"}))]
    else: @[]
  let actionRow = style({"width": "full", "height": "44", "gap": "14", "justify": "between", "align": "center"})
  let contentButton = style({"width": "108", "height": "40"})
  let captionButton = style({"width": "46", "height": "48"})
  let shortcutFeedback =
    if status.len > 0: @[text("ⓘ  " & status, key = "feedback",
      styles = style({"width": "full", "height": "26"}))]
    else: @[]
  let generalCard = settingsCard("general-settings", @[
      row(@[
        column(@[
          text("区域截图快捷键", key = "heading", styles = style({"height": "24"})),
          text("按下当前组合键，即可开始框选。", key = "description", styles = style({"height": "20"}))
        ], key = "shortcut-labels", styles = style({"flex_grow": "1"})),
        row(entry, key = "caps", styles = style({"gap": "7", "align": "center"})),
        leaf.button(changeLabel, onClick = toggleRecording, key = "change", styles = contentButton)
      ], key = "shortcut", styles = actionRow)
    ] & shortcutFeedback & @[
      standard.separator(key = "startup-divider"),
      row(@[
        text("登录 Windows 时启动拾光", key = "startup-label"),
        leaf.switch("登录 Windows 时启动拾光", checked = startup == 1, key = "startup",
          disabled = startup > 1, onChange = onStartup, styles = style({"width": "40", "height": "32"}))
      ], key = "startup-row", styles = actionRow)
    ] & startupFeedback)
  column(@[
    row(@[
      text(settingsTitle, key = "caption", styles = style({"height": "48", "flex_grow": "1"})),
      leaf.button("最小化", key = "minimize", onClick = minimize, styles = captionButton),
      leaf.button("关闭", key = "close", onClick = close, styles = captionButton)
    ], key = "caption-row", styles = style({"width": "full", "height": "48", "align": "center"})),
    standard.separator(key = "caption-divider"),
    column(@[
      text("常规", key = "general", styles = style({"height": "26"})),
      generalCard
    ] & updateView(update) & @[
      standard.separator(key = "footer-divider"),
      row(@[
        standard.cardDescription(trayLabel, key = "tray"),
        leaf.button("退出拾光", key = "quit", onClick = quit, styles = contentButton)
      ], key = "footer", styles = actionRow)
    ], key = "body", styles = style({"padding": "28", "gap": "7", "width": "full"}))
  ], key = "settings", styles = style({"width": "full", "height": "full"}))
