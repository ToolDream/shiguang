import leaf
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

proc caps(chord: uint32): seq[Node] =
  if chord == 0:
    return @[standard.cardDescription("未注册", key = "unregistered")]
  var names: seq[string]
  for (bit, label) in [(2'u32, "Ctrl"), (1'u32, "Alt"), (4'u32, "Shift"), (8'u32, "Win")]:
    if ((chord shr 8) and bit) != 0: names.add(label)
  names.add(mainKey(chord and 255))
  for i, name in names:
    if i > 0:
      result.add(text("+", key = "plus-" & $i, styles = style({"width": "10"})))
    # Win32 measures kbd from the complete text, including longer key names.
    let keycap = standard.kbd(name, key = "cap-" & $i)
    result.add(ui(NodeKind.tag, key = keycap.key, text = name,
      styles = style({"height": "34"}), attributes = keycap.attributes))

proc settingsView*(chord: uint32, recording: bool, status: string, trayReady: bool,
                   toggleRecording, quit: Callback): Node =
  let entry = if recording:
    @[text("请按下新组合键  ·  Esc 取消", key = "recording")]
  else: caps(chord)
  let trayLabel = if trayReady: "●  托盘运行中 · 关闭窗口不影响截图"
    else: "●  通知区域图标不可用"
  column(@[
    text("区域截图快捷键", key = "heading", variant = "primary"),
    standard.cardDescription("在任意窗口按下这组键，即可拖动框选。", key = "description"),
    row(@[
      row(entry, key = "caps", styles = style({"gap": "7", "align": "center"})),
      leaf.button(if recording: "取消录入" else: "更改", onClick = toggleRecording,
        key = "change", variant = "secondary", styles = style({"width": "86", "height": "38"}))
    ], key = "shortcut", styles = style({"width": "full", "height": "48",
      "justify": "between", "align": "center"})),
    standard.fieldError(if status.len == 0: " " else: status, key = "feedback"),
    standard.separator(key = "divider"),
    row(@[
      standard.cardDescription(trayLabel, key = "tray"),
      standard.button("退出拾光", key = "quit", variant = UiVariant.ghost, onClick = quit)
    ], key = "footer", styles = style({"width": "full", "justify": "between", "align": "center"}))
  ], key = "settings", styles = style({"padding": "28", "gap": "12",
    "width": "full", "height": "full"}))
