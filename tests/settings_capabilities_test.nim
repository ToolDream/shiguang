import leaf
import leaf/backend
import leaf/backends/win32/capabilities
import ../src/settings_view

proc captions(node: Node): seq[string] =
  if node.text.len > 0: result.add(node.text)
  for child in node.children: result.add(captions(child))

let win32 = win32Capabilities(shadcnLight)
for (chord, recording, status, trayReady, expected, absent) in [
    (0x0653'u32, false, "", true, @["Ctrl", "Shift", "S", "更改"], @["未注册", "取消录入"]),
    (0x0f20'u32, false, "", true, @["Ctrl", "Alt", "Shift", "Win", "Space", "更改"], @["未注册"]),
    (0'u32, false, "", false, @["未注册", "●  通知区域图标不可用", "更改"], @["Ctrl"]),
    (0x0653'u32, true, "", true, @["请按下新组合键  ·  Esc 取消", "取消录入"], @["Ctrl", "更改"]),
    (0x0653'u32, false, "此组合键无效或已被占用，未更改。", true,
      @["Ctrl", "Shift", "S", "此组合键无效或已被占用，未更改。"], @["取消录入"])]:
  let view = settingsView(chord, recording, status, trayReady, nil, nil)
  let tree = mount(view)
  discard checkCapabilities(tree, win32, compatible)
  discard checkCapabilities(tree, win32, strict)
  let labels = captions(view)
  for text in expected: doAssert text in labels, text
  for text in absent: doAssert text notin labels, text
echo "5 Settings views passed the Leaf Win32 capability check"
