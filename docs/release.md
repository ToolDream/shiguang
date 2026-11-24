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
4. 跑平台回归 9 项，用 `objdump` 递归解析导入表：只接受 System32 与 api-set 中的系统 DLL，其余依赖从工具链取出随包并继续递归。
5. 构建独立更新助手并校验它只依赖系统 DLL。将程序、助手、随包 DLL 与两个清单打包为无压缩的 ZIP32。
6. 先用 Microsoft Defender 扫描待分发的文件；不通过则停止。把 zip 解到空目录，用只含 System32 的 `PATH` 启动，等待主窗口 `Shiguang.Controller` 出现后结束进程。
7. 上传构件 `shiguang-windows-x64`，保留 14 天。

## 开发版预发布

`main` 的新流水线交付后，开发版只使用一个预发布 **`dev-latest`**，固定下载资产名
`shiguang-windows-x64.zip`。开发构建身份仍显示 `dev-<提交前 12 位>`；固定机器 tag 不是版本号。
跨提交的发布 job 串行。切换前核对远端 main、签名源码顺序和资产；旧作业不能覆盖较新的版本。
同提交重跑保留已校验的发布。机器 tag 只允许快进，不接管未知 tag/Release，也不移动正式 tag。

没有旧开发版时首次建 draft。若已有唯一的旧 `dev-<SHA>` 开发 Release，先核旧机器记录、Git tag 与 ZIP，
保持旧包可下载直到新包完成上传回读，再把**同一 Release ID** 改绑到 `dev-latest`。
旧 Git tag 仍保留，不新建第二个公开开发 Release。多个旧开发 Release 或身份不符时停止，不自动批删。
后续沿用同一 Release ID。先准备、验签并回读新包的暂存资产，再将当前包改名为备份、
将新包改为固定下载名、快进机器 tag，最后写入当前签名。读回确认后才删除旧备份。暂存/备份只为切换，
不是历史版本资产。GitHub 的这些操作**不原子**：切换时固定下载名可能短暂不存在，或下载包与旧签名不匹配。
客户端遇到不一致就拒绝安装；上一包在备份名下保留。中断后的重跑依据已签 pending 状态和实际资产归属继续；
遇到更新的 main 且 tag 尚未快进时，先恢复旧固定资产；已快进则完成签名提交再发布更新版本，不回移 tag。
无法证明签名、资产或 tag 归属时停住并保留现场，不删除未知内容。

旧流水线与远端下载不会因本地代码改变；只有部署并成功原地迁移后才启用固定下载。
滚动 job 不删除其他 Release 或 tag。

## 正式版发布

正式 tag 必须事先存在：自行 `git tag` 并推送即可触发，流水线只接受已存在的 tag，不创建、不删除、也不移动任何 tag。发布分三步：

1. 在不可见的 draft 中上传 zip。
2. 用 REST 接口回读资产：数值资产 id、文件名、大小与 SHA-256 必须与本地一致。
3. 按完整 ref（`refs/tags/...`）重新解析远端 tag，要求仍指向本次构建的提交且 ref 对象未被替换，然后才公开。

正式包使用签名更新记录和正式 ref 对象记录绑定身份。已公开同身份版本只读核对；构建内容不同或身份不明则不覆盖。历史版本若仍匹配旧机器记录及资产可只读核验，但只读成功不代表它具有自动更新签名；不会回填旧发布。

新正式版本只接受 `vMAJOR.MINOR.PATCH`（例如 `v1.2.3`，各段最多 10 位，且没有前置零）；删除或重建 tag（ref 对象变化）不会被当成新版本。lightweight 与 annotated tag 都按完整 ref 解析。

## 自动更新发布合同

- **单一 main 更新线。** 正式和测试版都以候选源码提交在 `origin/main` 第一父链中的位置作为更新序号，从根提交开始计数。以候选提交计数，不使用发布时 HEAD、流水线编号或发布时间；同一提交重跑不变。构建和签名分别核对完整 Git 历史，不签浅克隆或不在这条链上的提交。
- **不会倒退。** 测试通道可接收更新的正式版；正式通道只接收正式版。即使旧提交后来才发布，也不会覆盖较新的安装；同提交跨通道去重，已安装正式版还要求正式 SemVer 严格增加。不支持维护分支自动覆盖较新的测试版源码。**禁止对这条更新线重写历史或换仓重建**；否则序号不再可比较，须另行设计迁移，不能自动回退到其他排序。
- **签名是发布门禁。** 构建只读取公开仓库变量 `SG_UPDATE_PUBLIC_KEY`（P-256 公钥的 `x || y`，
  128 个小写十六进制字符，不带 `04`）。仅发布 job 的封包步骤读取匹配的 PEM `SG_UPDATE_SIGNING_KEY`；
  不匹配或缺失则在远端写入前失败。原构件先验证并由 Windows CI 做安全扫描/冒烟；发布 job 保持二进制字节不变，
  仅增加两个签名库存文本文件，用无压缩 ZIP32 最终封装，再重做 ZIP/逐文件与构建身份校验。
- **两层签名。** Release 签名行 `<!-- shiguang-update-v2 BASE64 SIGNATURE -->` 签最终 ZIP 大小、SHA-256、
  架构、通道/版本、提交、源码序号和逐文件条目；包内 `UPDATE-INVENTORY.txt` 与 `UPDATE-INVENTORY.sig`
  用不同域 `shiguang-inventory/1` 签本构建身份及原始文件库存，不签自身，也不签 ZIP。
  两层身份和文件条目必须一致。P-256 ECDSA/SHA-256 的固定宽度 `r || s` 不是 Windows Authenticode。
- 首次启用须手动解压新发布的完整已签 ZIP（含助手和签名库存）；手动构件和旧无此库存的包须手动迁移。
  更新及回滚只使用安装和事务内的已签库存，无需查询已安装开发版本原 Release，不依赖历史资产。
  便携目录应只授予当前使用者可信写权限；不提权、不修改用户文件。包最多 64 MiB。
- 发布后仍需在真实 Windows x64 桌面从公开下载入口验证检查、暂存、重启、启动确认及回滚。客户端只用一页 30 个 Release 作为发现范围。返回不足 30 项即视为完整列表，直接按其中已验签的记录判定，不再额外请求：
列表里已找到的候选不会被补充查询的异常抹掉，也不浪费 API 限额。只有返回正好达到页大小（结果可能被截断）
且仍缺所需通道的有效签名时，才沿用原本适用的补充查询：无有效正式候选记录时另查 GitHub 的最新正式 Release，
测试通道无滚动记录时按固定 dev-latest 查询。列表请求、列表解析和此时真正需要的补充查询一样，出错即本次检查失败，
不会被当成“没有更新”。范围外的旧发布不会触发自动降级。

## 包内容

| 文件 | 内容 |
| --- | --- |
| `shiguang.exe` | Windows x64 可执行文件，解压即用 |
| `shiguang-updater.exe` | 由主程序从独立目录启动的更新助手，不应手动运行 |
| `SHA256SUMS.txt` | EXE 与随包 DLL 的 SHA-256 |
| `UPDATE-INVENTORY.txt`、`UPDATE-INVENTORY.sig` | 最终分发包中的本构建身份与离线签名库存 |
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

工作流仍由 push 或手动触发；只开 PR（包括 fork）不会触发本工作流，不能计为 PR 测试覆盖。
