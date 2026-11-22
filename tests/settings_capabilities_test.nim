import leaf
import leaf/backend
import leaf/styles
import std/[options, tables]
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
    doAssert change.right == quit.right and quit.right == (settingsWidth - 28) * dpi div 96
    doAssert change.bottom - change.top == 40 * dpi div 96
    doAssert quit.bottom - quit.top == change.bottom - change.top
    for key in ["change", "quit", "startup", "startup-status"]:
      if not ids.hasKey(key): continue
      let b = box(key)
      let pixels = pixelEdges(b.rect, dpi)
      let clip = pixelEdges(b.clip, dpi)
      doAssert pixels.left >= clip.left and pixels.top >= clip.top and
        pixels.right <= clip.right and pixels.bottom <= clip.bottom, "clipped control: " & key

let win32 = win32Capabilities(shadcnLight)
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
