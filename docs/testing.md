# 测试与验收

自动测试分三层，共 33 项：C 逻辑 14 项、设置视图能力检查 11 项（快捷键 5 项、登录启动 6 项）、Windows 平台回归 8 项。命令都在仓库根目录执行（Windows 命令用 PowerShell）。

## 1. C 逻辑测试（14 项）

平台无关，Linux、macOS、WSL 或 MinGW 都可运行。裁切 3 项覆盖反向拖选、跨屏拼接、空选区与异常尺寸拒绝；编辑 8 项覆盖历史、工具栏布局、热键编码、选区边界与调整、八把手命中及窄框标注、短边对比；调色板 3 项检查主要按钮在明暗两套配色下（常态与按下）的对比度都不低于 4.5:1，并确认调色板每个字段变化都会让缓存失效。

调色板测试用到 `pow` / `fmax` / `fmin`，因此在 glibc 与 macOS 上要加 `-lm`；mingw-w64 的 `libm.a` 是空桩，math 来自 msvcrt 与 `libmingwex`，`-lm` 在 Windows 上同样可加。

~~~sh
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror tests/crop_test.c src/crop.c -o "$out/crop-test" &&
  "$out/crop-test" &&
  cc -std=c11 -Wall -Wextra -Werror tests/editor_test.c src/editor.c src/crop.c -o "$out/editor-test" &&
  "$out/editor-test" &&
  cc -std=c11 -Wall -Wextra -Werror tests/ui_palette_test.c -o "$out/ui-palette-test" -lm &&
  "$out/ui-palette-test"
~~~

## 2. 设置视图能力检查（11 项）

需要 Nim 与相邻的 Leaf 源码。用 Leaf 的 Win32 能力表检查五种设置视图：默认键帽、全修饰键组合、未注册且托盘不可用、录入中、冲突反馈。同时检查登录启动的六种视图状态。这是构建期的静态合同检查，不是 Windows 运行结果。

~~~sh
nim c -r --path:../leaf/src --nimcache:build/nimcache --out:build/settings_capabilities_test tests/settings_capabilities_test.nim
~~~

## 3. Windows 平台回归（8 项）

需 Windows x64 交互桌面。用例不改动用户剪贴板或实际启动项；`startup` 只在当前用户的临时测试键内验证默认、启停、权限失败和非自有项保留。另有 `preview`、`edit`、`save-edit` 会短暂显示遮罩，`replace` 会临时注册组合键，`settings-caption` 会临时创建设置窗口并最小化、关闭它。

~~~powershell
New-Item -ItemType Directory -Force build | Out-Null
gcc -std=c11 -D_WIN32_WINNT=0x0a00 tests/regression_win.c src/crop.c src/editor.c src/editor_win.c -o build/regression_win.exe -luser32 -lgdi32 -lmsimg32 -ldwmapi -lcomctl32 -lshell32 -ladvapi32 -lole32 -luuid -lwindowscodecs
if ($LASTEXITCODE -ne 0) { throw "回归程序构建失败" }
foreach ($case in 'save', 'hotkey', 'preview', 'replace', 'edit', 'save-edit', 'settings-caption', 'startup') {
    .\build\regression_win.exe $case
    if ($LASTEXITCODE -ne 0) { throw "回归失败：$case" }
}
~~~

## 4. 现场验收清单（人工）

以下项目依赖真实桌面环境，自动化不覆盖，需人工核对。

1. 用左上方副屏、100% / 150% 混合 DPI 和错位布局截图动态窗口，核对冻结画面、跨屏物理尺寸与 PNG 空洞；改变布局或 DPI 应取消当前截图。
2. 在屏幕中部、底部、贴边和全屏选区检查工具栏下贴右对齐、翻上、内侧回退；确认形状与颜色弹层可点，窄屏仍能触达复制和保存。
3. 绘制五种形状、箭头与自由线条，录入中文并检查颜色和导出像素；核对撤销、重做可用态，中文输入时 Enter 与 Ctrl+S 不触发导出；复制后实际粘贴，退出后再粘贴。
4. 预置剪贴板文本与上次截图，检查拖选、空拖、编辑中 Esc 均不替换这两项；取消另存、抢占同名文件、占用剪贴板后确认编辑保留且可重试；另存对话框期间改变布局或 DPI 应取消本次截图且不生成 PNG，托盘另存仍可用。
5. 两次启动只出现一个通知区域图标；检查窗口与托盘图标、菜单、键帽、「更改」/「重新录入」及标题栏的「最小化」「关闭」；录入冲突键保留旧值并说明原因，录入可用键后重启核持久化，最后从托盘退出。
6. 打开登录启动，关窗重开及退出后重启应用，确认登记状态；关闭开关检查登记移除。核启动命令的引号与后台托盘行为；只有真实重新登录才算验证 Windows 自动调度，不为此注销共享测试机。

## 实测结果

以下为最近一轮现场验证的结果（内部快照 r6），仅在单屏远程桌面环境执行。

- 本机（非 Windows）：裁切 3 项与编辑 3 项在 ASan/UBSan 下通过；快捷键和登录启动视图各 5 项通过能力合同检查；调色板 3 项（本轮新增）在本机实跑通过，明暗两套配色的对比度为 8.66:1 / 7.41:1 / 7.14:1 / 5.64:1。
- Windows 单屏远程桌面：r6 快照的 7 项回归全部退出码 0；随后的 r7 快照补齐 `startup`，8 项回归同样全部退出码 0（应用 SHA-256 `230cded2a5cf458fce0e808f499e01c20da507c385bd84f855e4de120dfe73d`，回归程序 `bd04e944c9d9c8609015d59a0abcc4ebea503c0a9abf01a2f158d9f0329d1564`）。
- 同一会话的实际应用：从托盘打开设置，键帽、「更改」与标题栏按钮可用，最小化后可还原，「关闭」只隐藏并可再次从托盘打开；录入已被占用的组合时保留原快捷键，按钮变为「重新录入」并显示「ⓘ 新组合键已被占用，仍使用 Ctrl + Shift + S。」；Esc 可取消录入；UIA 点位探测能命中「最小化」「关闭」「更改」「退出拾光」，但该会话的无障碍树遍历失败（MSAA 返回 `0x80004002`，UIA 报 `Unexpected caption`），键盘焦点与悬停效果以现场截图留存。
- 退出应用后，在另一程序中粘贴得到 537×199 图像。快捷键持久化路径本轮未改，沿用前序实测（重启后键帽与全局热键仍为所设组合）。
- 另存与 PNG 导出逻辑本轮未改，沿用同一导出逻辑的前序实测：重定向盘与本地磁盘 PNG 均可解码（415×199、419×197），同名目标不被覆盖（34 字节原文件保留）。

## 限制

以上只在单屏远程桌面会话中验证：多屏、混合 DPI、错位布局与窄屏工具栏需在真实硬件上核对，中文输入法组字与五种形状、箭头的实际绘制仍未验证。这些结果不构成对全部硬件、显示配置或其他重定向实现的兼容性结论。
