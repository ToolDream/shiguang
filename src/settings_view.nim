import leaf
import std/strutils
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

const settingsTitle* = "拾光截图 / 设置"

proc settingsView*(chord: uint32, recording: bool, status: string, trayReady: bool,
                   toggleRecording, quit: Callback, minimize: Callback = nil,
                   close: Callback = nil, startup: cint = 0, startupError = false,
                   onStartup: Callback = nil): Node =
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
  let startupDetail = (if startupError: "无法更新启动项。当前：" else: "") & startupStatus
  let contentButton = style({"width": "108", "height": "40"})
  let captionButton = style({"width": "46", "height": "48"})
  column(@[
    row(@[
      text(settingsTitle, key = "caption", styles = style({"height": "48", "flex_grow": "1"})),
      leaf.button("最小化", key = "minimize", onClick = minimize, styles = captionButton),
      leaf.button("关闭", key = "close", onClick = close, styles = captionButton)
    ], key = "caption-row", styles = style({"width": "full", "height": "48", "align": "center"})),
    standard.separator(key = "caption-divider"),
    column(@[
      text("区域截图快捷键", key = "heading", variant = "primary"),
      standard.cardDescription("按下当前组合键，即可开始框选。", key = "description"),
      row(@[
        row(entry, key = "caps", styles = style({"gap": "7", "align": "center"})),
        leaf.button(changeLabel, onClick = toggleRecording, key = "change", styles = contentButton)
      ], key = "shortcut", styles = style({"width": "full", "height": "48",
        "justify": "between", "align": "center"})),
      standard.fieldError(if status.len == 0: " " else: "ⓘ  " & status, key = "feedback"),
      standard.separator(key = "startup-divider"),
      standard.switch("登录 Windows 时启动拾光", checked = startup == 1,
        key = "startup", disabled = startup > 1, onChange = onStartup),
      standard.cardDescription(startupDetail, key = "startup-status"),
      standard.separator(key = "divider"),
      row(@[
        standard.cardDescription(trayLabel, key = "tray"),
        leaf.button("退出拾光", key = "quit", onClick = quit, styles = contentButton)
      ], key = "footer", styles = style({"width": "full", "justify": "between", "align": "center"}))
    ], key = "body", styles = style({"padding": "28", "gap": "12", "width": "full"}))
  ], key = "settings", styles = style({"width": "full", "height": "full"}))
