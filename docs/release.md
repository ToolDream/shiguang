# 构建与发布流水线

唯一工作流是 `.github/workflows/build.yml`，三个 job：Windows x64 测试与构建、开发版预发布、正式版本。

## 触发

| 事件 | 测试与构建 | 可下载构件 | 开发版预发布 | 正式版 |
| --- | --- | --- | --- | --- |
| 任意分支 push（非删除） | 完整运行 | 该 run 的 Artifacts | 仅 `main` 且成功 | 不产生 |
| 正式 tag push | 完整运行 | 该 run 的 Artifacts | 不产生 | 核实后公开 |
| `dev-` 前缀 tag push | 跳过 | 不产生 | 不产生 | 不产生 |
| 手动运行 | 完整运行 | 该 run 的 Artifacts | 不产生 | 不产生 |

`dev-` 前缀是流水线自用的机器 tag 保留名；删除事件不构建。构建 job 不做并发取消，新推送不会打断正在运行的旧 run。

## 构建 job

在 `windows-2022` 上依次执行：

1. 检出本仓库，并把 Leaf 检出到 `leaf/`，核对提交确为 `df53b244383bc79abd3d8a035d4ac29636602444`。
2. `scripts/prepare_windows_toolchain.ps1` 准备并校验 MinGW-w64，`setup-nim-action` 安装 Nim 2.2.6，记录提交、完整 ref 与 ref 对象。
3. 跑 C 逻辑测试 9 项、设置视图能力检查 10 项，再以 `--path:leaf/src` 构建 `build/shiguang.exe`。
4. 跑平台回归 8 项，用 `objdump` 递归解析导入表：只接受 System32 与 api-set 中的系统 DLL，其余依赖从工具链取出随包并继续递归。
5. 打包 `dist/shiguang-windows-x64.zip`，写入 `SHA256SUMS.txt` 与 `BUILD-MANIFEST.json`。
6. 先用 Microsoft Defender 扫描待分发的文件；不通过则停止。把 zip 解到空目录，用只含 System32 的 `PATH` 启动，等待主窗口 `Shiguang.Controller` 出现后结束进程。
7. 上传构件 `shiguang-windows-x64`，保留 14 天。

## 开发版预发布

`main` 构建成功后，以 `dev-<提交前 12 位>` 创建预发布，只挂一个 zip 资产。开发版按提交不可变：同一提交重跑时，已公开的版本只做只读核对，不覆盖；只留 draft 的会续做。并发按提交隔离，不同提交的发布互不挤占。若同名机器 tag 已存在，必须指向同一提交，发布成功后还会复核一次实际指向。

## 正式版发布

正式 tag 必须事先存在：自行 `git tag` 并推送即可触发，流水线只接受已存在的 tag，不创建、不删除、也不移动任何 tag。发布分三步：

1. 在不可见的 draft 中上传 zip。
2. 用 REST 接口回读资产：数值资产 id、文件名、大小与 SHA-256 必须与本地一致。
3. 按完整 ref（`refs/tags/...`）重新解析远端 tag，要求仍指向本次构建的提交且 ref 对象未被替换，然后才公开。

发布身份写在发布说明第一行，是一条只有本流水线会写的机器记录（仓库、模式、tag、提交、ref 对象、ZIP 摘要）。只有它与经验证的构件完全一致时才认作自己的发布；记录缺失或不一致一律零写拒绝。已公开的同身份版本只做只读核对，新构建产出不同 ZIP 时绝不替换。多次 API 调用之间没有原子保证，但过程只会停在「不可见 draft」或「已核实公开」两种状态。

tag 命名要求：正式 tag 以字母数字开头，最多 100 个字符，只含字母数字与 `.` `_` `+` `-`。删除或重建 tag（ref 对象变化）不会被当成新版本。lightweight 与 annotated tag 都按完整 ref 解析。

## 包内容

| 文件 | 内容 |
| --- | --- |
| `shiguang.exe` | Windows x64 可执行文件，解压即用 |
| `SHA256SUMS.txt` | EXE 与随包 DLL 的 SHA-256 |
| `BUILD-MANIFEST.json` | 仓库、完整 ref 与 ref 对象、提交、事件、run 链接、Leaf 提交、Nim 与 gcc 版本、构建时间，以及每个文件的大小与 SHA-256 |

只有 exe 依赖非系统 DLL 时，包里才会多出对应 DLL，校验值同样记在清单里。判断包对应哪份源码只看清单的 `commit`；ZIP 自身的 SHA-256 写在发布说明里，也可从资产元数据查到。

## 校验已下载的包

以下命令在解压后的目录内执行：

- Linux：`sha256sum -c SHA256SUMS.txt`
- macOS：`shasum -a 256 -c SHA256SUMS.txt`
- PowerShell：`Get-FileHash -Algorithm SHA256 .\shiguang.exe`，与 `SHA256SUMS.txt` 对照。

仓库脚本 `scripts/verify_dist.sh` 校验整包：ZIP 摘要、包内清单、每个文件的大小与 SHA-256、无清单外文件，并复核来源。

~~~sh
# 开发版：只认本次构建的提交
MODE=commit COMMIT=<完整提交> GH_REPO=<owner/repo> bash scripts/verify_dist.sh dist

# 正式版：连远端 ref 一起复核（需要 GH_TOKEN）
MODE=ref REF=refs/tags/v1.2.3 GH_REPO=<owner/repo> GH_TOKEN=<token> bash scripts/verify_dist.sh dist
~~~

## 发布脚本

`scripts/publish.sh dev|release` 由流水线调用，也可手工运行。需要 `GH_TOKEN`、`GH_REPO`、`RUN_URL`；`dev` 模式另需 `SHA`，`release` 模式另需完整 `REF`；可选 `DIST`（默认 `dist`）与 `ZIP_NAME`。发布 job 是唯一持有 `contents: write` 的环节，其余只有读权限，使用默认 `GITHUB_TOKEN`，不需要 PAT。
