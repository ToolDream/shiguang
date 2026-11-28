# 自动更新后端接线

## 设置页使用的接口

从 `src/update_bridge.nim` 导入：

- `updateSnapshot(): UpdateSnapshot`：当前状态。字段为
  `phase: UpdatePhase`、`installed: UpdateBuild`、`settings: UpdateSettings`、
  `candidate: Option[UpdateBuild]`、`error: string`。
- `observeUpdates(callback: proc (state: UpdateSnapshot))`：在 Win32 UI 线程订阅变更。
  用 `nil` 解除订阅。挂载首屏先取 `updateSnapshot()`；回调如果更新 Leaf 状态，由设置宿主进入自己的 `runtime.uiTask`。
- `changeUpdateChannel(channel: UpdateChannel): bool`：`ucStable` 或 `ucTest`。
  返回是否已保存改动。切换不会立即发网络请求。
- `changeAutoDownload(enabled: bool): bool`：返回是否已保存改动。
- `downloadUpdate(): bool`：在 `upAvailable` 或 `upDownloadFailed` 可调用，返回是否受理。
- `checkForUpdates(): bool`：「检查并安装更新」入口。只跳过 24 小时窗口，仍记录 `lastCheck`；
  已有检查在进行时合并为该次检查，已有传输在进行时排队一次复查，不打开平行任务。
- `autoInstallDue(): bool`：只在定时器回合调用，负责推进可见的取消窗口
  （`CancelWindowSeconds`）并回答本次授权是否已可自动重启。
- `cancelUpdateAutoInstall(): bool`：撤销本次「检查并安装」的一次性授权。正在进行的传输不打断，
  已验证的暂存仍可通过手动「重启更新」使用。
- `autoInstallArmed(): bool`：本次检查是否已下载验证完成并在等待空闲自动重启。
- **重启按钮**使用 `src/shiguang.nim` 的 `requestUpdateRestart(): bool`。它检查截图、编辑、
  输出及快捷键录入是否空闲，仅在成功启动助手后结束应用。不要调用桥接层内部的 `restartForUpdate` 绕过这一入口。

`UpdatePhase` 包含 `upIdle`、`upChecking`、`upCheckFailed`、`upAvailable`、
`upDownloading`、`upDownloadFailed`、`upReady`、`upRestarting`。
版本显示使用 `installed.version`，必要时用真实 `installed.revision` 补充，不能写设计稿版本。
`UpdateSnapshot.autoInstall` 表示本轮「检查并安装更新」仍有效（可取消，含排队）；
`installReady` 表示它已经下载并验证完成、正在等待空闲自动重启；`result` 是本轮结果
（`rrNone`/`rrChecking`/`rrUpToDate`/`rrCandidate`/`rrCheckFailed`/`rrDownloadFailed`/`rrInstallFailed`），
与旧候选是否仍可手动安装无关。设置页按 `result` 显示本轮结果，不解析 `error` 字符串。
托盘只在 `upReady` 显示「重启更新」。
`error` 为诊断信息，视图可用本地化且更简短的说明，不应直接把内部路径作为常规提示。

## 本地更新流程

- 检查尝试（含失败）以 24 小时为间隔，记录保存在当前用户
  `%LOCALAPPDATA%\ToolDream\Shiguang\update\update.settings`。时钟回退重置节流基点，不立即重试。
  Win32 启动、定时器和休眠恢复触发同一策略；网络与 ZIP 校验在工作线程。
- 「检查并安装更新」的一次性授权只存在于内存：检查失败、没有可用更新、下载或验签失败、持久化失败、
  切换通道和普通退出都会撤销它，进程重启后不会自动安装。授权只在本轮检查返回的**完整 `UpdateBuild`
  身份**等于被采纳的候选时才固定（低于旧缓存或同序号不同身份都保留原缓存并撤销授权），绝不用旧缓存代替。
  自动安装只在 `upReady`、`installReady` 为真、暂存与候选一致、`sg_update_can_restart()` 确认空闲，
  并且首个 ready 观察后已过 `CancelWindowSeconds` 时申请；失败一次后只保留手动「重启更新」，不会按节拍反复重启。
  取消命令经 `PostMessage` 进入同一条消息；只保证**在自动启动尝试之前已被处理的取消**生效，不承诺点击与定时 tick 的物理先后
  （Leaf 先排 session 事件、用户回调再投递消息，队列次序是源码可推导的窗口，未经实机验证）。
- 发现流程把"没有更新的发布"和"符合通道/版本条件、但签名或清单校验被拒绝的发布"分开：后者
  令本轮检查失败（`rrCheckFailed`），不会被显示成「已是最新版本」，也不会采用任何未通过验签的包；
  若同一列表另有合法候选，仍按原路由选中它。
- 下载结果仅在当次运行可用于重启。普通退出不启动助手，不等待正在进行的网络传输。
  失败日志或尚未完成清理的工作目录可留在更新目录；不要把它们当作已安装的版本。
- 节流持久化失败进入 `upCheckFailed`，保留上次成功的 `settings.lastCheck`，使用单调时钟冷却 60 秒；UI 按状态获取失败，不必解析英文诊断。
- 发现候选后使用首装 ZIP 中的 `UPDATE-INVENTORY.txt` 和 `UPDATE-INVENTORY.sig` 核验当前编译身份
  （版本、通道、提交、序号、repo 和平台）。旧助手与待替换文件逐项归属此库存，
  不信任可编辑的 `BUILD-MANIFEST.json`，也不查询已安装版本的历史 Release。缺签名库存的历史版本须手动迁移。
- 助手等旧进程退出后，在第一次替换前持久化新旧签名库存、备份、目录绑定及独立恢复入口。
  普通启动、助手和新 EXE 使用同一独占文件句柄（白名单继承）；恢复时重新独占打开，
  不需要输入参数。Windows 锁住本地目录链，并用同一个文件句柄验摘要/改名；
  晋升和回滚不覆盖未知插入文件。请从自己的本地便携目录运行，不要在不同 Windows 用户共用的可写目录自动安装。
- 新 EXE 创建控制器、设置宿主和托盘后进入实际消息泵。第一次专用消息发 ready 并等待放行，
  处理后续专用消息才回 ACK。助手收到 ACK 且新进程仍运行后才提交；这只验证该启动节点，
  不保证应用以后不会崩溃。失败时助手停止自己启动的子进程再回滚；不会自动启动旧版。
- **断电或主程序无法打开：** 退出所有拾光进程后，在拾光便携目录打开
  **`Shiguang-Recovery/恢复拾光.exe`**。它只使用系统 DLL，直接用保存的新旧签名库存离线恢复，
  不需要启动主 EXE、连接网络、提权或查找临时目录。恢复成功后手动重新打开拾光。
  已提交或无事务的恢复不会触发安装。若有提交标记，但任一签名管理文件缺失、不可读或不匹配，
  恢复会报告失败并保留事务和旧版备份，不会自动覆盖现有文件。请勿删掉或移动这个固定子目录；安全助手不在替换清单内，
  支持该事务格式的已有经验证副本继续保留。收到不兼容格式、未知文件或占用时保留现场。
- 配置、用户文件和新包不再列出的旧 DLL 不动。请将便携目录只开放给可信修改者；
  本机制不保护已控制当前 Windows 账户或可执行目录的恶意程序。

Windows CI 额外包含 `update-busy` 回归和两项独立进程握手用例；它们不替代真实桌面从公共下载入口进行的更新验收。
