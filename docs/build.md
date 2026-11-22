# 环境、架构与构建

## 环境

- 运行：Windows x64（Windows 10 1703 或更新）。
- 编译：Nim **2.2.6+**，PATH 中可用的 x64 MinGW-w64 GCC 和 windres，以及 [Leaf](https://github.com/SatoriTours/Leaf) 源码。
- Leaf 固定为 `0.1.0` 提交 `df53b244383bc79abd3d8a035d4ac29636602444`，无需修改。本地开发把 Leaf 放在相邻目录 `../leaf`；流水线检出到仓库内 `leaf/`，对应命令用 `--path:leaf/src`。

## 架构

| 文件 | 职责 |
| --- | --- |
| `src/shiguang.nim` | 应用入口、设置窗口状态与消息循环接线 |
| `src/settings_view.nim` | 设置界面视图：常规、快捷键、登录启动和软件更新 |
| `src/desktop_win.c` | 托盘、全局热键、设置窗口标题栏、桌面遮罩、选区交互 |
| `src/editor.c` / `src/editor_win.c` | 标注模型与历史、工具栏布局、Win32 编辑窗口 |
| `src/export_win.c` | 剪贴板 DIB 与 WIC PNG 导出、原子落盘 |
| `src/crop.c` | 选区几何换算与多屏拼接（纯 C，无平台依赖） |

一次截图的数据流：读取各屏冻结位图 → 换算物理像素矩形并拼接 → 在编辑层叠加标注 → 合成导出位图 → 写入剪贴板，或经 WIC 编码为 PNG。PNG 先写同目录临时文件，再以不覆盖的方式原子改名，目标已存在则安全失败。

设置界面由 Leaf 绘制，与托盘、热键、采集和标注共用同一个 Win32 消息循环。

## 本地构建

在仓库根目录的 PowerShell 中：

~~~powershell
New-Item -ItemType Directory -Force build | Out-Null
windres -I src -i src/shiguang.rc -o build/shiguang-icon.o
if ($LASTEXITCODE -ne 0) { throw "拾光图标资源构建失败" }
nim c -d:release --cc:gcc --app:gui --passL:build/shiguang-icon.o --path:../leaf/src --nimcache:build/nimcache --out:build/shiguang.exe src/shiguang.nim
if ($LASTEXITCODE -ne 0) { throw "拾光截图构建失败" }
.\build\shiguang.exe
~~~

产物是 `build\shiguang.exe`，不需要安装程序。若本机工具链把非系统 DLL 动态链接进来，需一并复制；流水线会自动解析依赖并随包。构建前先确认 Leaf 提交：

~~~powershell
git -C ../leaf rev-parse HEAD
~~~

输出应为 `df53b244383bc79abd3d8a035d4ac29636602444`。

## 光点图标

窗口、托盘和 EXE 使用资源 `src/shiguang.ico`。常规构建直接嵌入此资源，不需要 SVG 工具。设计更新时，从仓库根目录运行 `python3 scripts/generate_icon.py`，用 `rsvg-convert` 将 `docs/images/logo.svg` 导出为 16–256 px 的九个尺寸。复现当前资源使用 librsvg 2.62.3 / Cairo 1.18.4；不同渲染器版本可能改变 PNG 编码。

没有传入构建身份的本地产物显示「当前版本 未知」。本地构建不会自动获得发布签名信任，见 [更新后端接线](update_backend.md) 与 [发布流水线](release.md)。

## 与流水线一致的构建环境

CI 用两个固定步骤准备编译链：

- `.github/workflows/build.yml` 通过 `setup-nim-action` 安装 Nim 2.2.6。
- `scripts/prepare_windows_toolchain.ps1` 下载官方 `mingw64.7z` 与 `7zr.exe`，校验固定 SHA-256 后解压，确认 `gcc -dumpmachine` 以 `x86_64` 开头，再把其 `bin` 追加到 `GITHUB_PATH`。脚本依赖 `RUNNER_TEMP` 与 `GITHUB_PATH`，面向 CI；本地可自行安装等价的 x64 MinGW-w64 GCC。

打包、构件上传与发布由流水线完成，步骤见 [构建与发布流水线](release.md)；测试命令与现场验收见 [测试与验收](testing.md)。
