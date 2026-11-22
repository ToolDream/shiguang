when not (defined(windows) and defined(amd64)):
  {.error: "拾光截图的桌面应用需要 Windows x64".}

import leaf
import leaf/ui as standard
import leaf/ui_contract
import std/[options, os]
import settings_view, update_bridge
import leaf/[native_layout, styles, ui_thread]
import leaf/backends/win32/[api, backend, calls, host]

{.compile: "crop.c".}
{.compile: "editor.c".}
{.compile: "editor_win.c".}
{.compile: "desktop_win.c".}
{.compile: "export_win.c".}
# This Leaf revision's select provider uses offsetof without including stddef.h.
{.passC: "-D_WIN32_WINNT=0x0a00 -include stddef.h".}
{.passL: "-luser32 -lgdi32 -lmsimg32 -ldwmapi -lcomctl32 -lshell32 -ladvapi32 -lole32 -luuid -lwindowscodecs".}

{.push cdecl, importc, header: "desktop_win.h".}
proc sg_start(): cint
proc sg_stop()
proc sg_bind_settings(window: HWND): cint
proc sg_ready(background: cint)
proc sg_startup_state(): cint
proc sg_set_startup(enable: cint): cint
proc sg_after_dispatch()
proc sg_update_set_available(available: cint)
proc sg_update_notice(message: WideCString)
proc sg_update_can_restart(): cint
proc sg_update_ui_ready(): cint
proc sg_update_exit()
proc sg_hotkey(): uint32
proc sg_set_hotkey(chord: uint32): cint
proc sg_record_hotkey(recording: cint): cint {.discardable.}
proc sg_settings_caption(action: cint)
proc sg_style_settings()
proc sg_settings_dark(): cint
proc sg_exit()
proc sg_fatal(message: cstring)
{.pop.}

proc isChild(root, window: HWND): BOOL {.
  stdcall, dynlib: "user32.dll", importc: "IsChild", raises: [].}

const
  recordMessage = WM_APP + 20
  trayMessage = WM_APP + 21
  updateMessage = WM_APP + 22
  updateConfirmMessage = WM_APP + 23
var recordKey: proc(chord: uint32)
var trayState: proc(ready: bool)
var updateState: proc(snapshot: UpdateSnapshot, feedback: Option[UpdateFeedback])
var postUpdateAction: proc(action: UpdateAction): bool

type SettingsState = object
  chord: uint32
  recording: bool
  status: string
  trayReady: bool
  startup: cint
  startupError: bool
  updates: UpdateSnapshot
  updateFeedback: UpdateFeedback
  channelOpen: bool
  dark: bool
let Settings = defineComponent[tuple[], State[SettingsState]]("Settings",
  proc(ctx: MountContext, props: tuple[]): State[SettingsState] =
    ctx.state(SettingsState(chord: sg_hotkey(), startup: sg_startup_state(),
      updates: updateSnapshot(), dark: sg_settings_dark() != 0)),
  proc(ctx: ComponentContext, props: tuple[], session: State[SettingsState]): UiDescription =
    let model = ctx.value(session)
    trayState = proc(ready: bool) =
      discard session.update(proc(old: SettingsState): SettingsState =
        result = old
        result.trayReady = ready
        result.startup = sg_startup_state()
        result.dark = sg_settings_dark() != 0)
    updateState = proc(snapshot: UpdateSnapshot, feedback: Option[UpdateFeedback]) =
      discard session.update(proc(old: SettingsState): SettingsState =
        result = old
        result.updates = snapshot
        result.updateFeedback = nextUpdateFeedback(old.updateFeedback, old.updates, snapshot, feedback)
        if snapshot.phase == upRestarting: result.channelOpen = false)
    let onChannelOpen: Callback = proc(e: Event) =
      discard session.update(proc(old: SettingsState): SettingsState =
        result = old
        result.channelOpen = e.open)
    let onUpdateAction = proc(action: UpdateAction) =
      # The bridge may notify synchronously. Handle the command after Leaf's event.
      if postUpdateAction == nil or not postUpdateAction(action):
        discard session.update(proc(old: SettingsState): SettingsState =
          result = old
          result.updateFeedback = updateActionFeedback(action, false))
    let toggleStartup: Callback = proc(e: Event) =
      let ok = sg_set_startup(cint(e.checked)) != 0
      let actual = sg_startup_state()
      discard session.update(proc(old: SettingsState): SettingsState =
        result = old
        result.startup = actual
        result.startupError = not ok or actual != cint(e.checked))
    let toggleRecording: Callback = proc(e: Event) =
      if model.recording:
        sg_record_hotkey(0)
        recordKey = nil
        discard session.update(proc(old: SettingsState): SettingsState =
          result = old
          result.recording = false
          result.status = "")
      elif sg_record_hotkey(1) == 0:
        discard session.update(proc(old: SettingsState): SettingsState =
          result = old
          result.status = "无法开始录入，请重新打开设置后再试。")
      else:
        recordKey = proc(chord: uint32) =
          sg_record_hotkey(0)
          recordKey = nil
          let status =
            if chord == 0: ""
            else:
              let outcome = sg_set_hotkey(chord)
              shortcutFeedback(outcome, sg_hotkey())
          discard session.update(proc(old: SettingsState): SettingsState =
            result = old
            result.chord = sg_hotkey()
            result.recording = false
            result.status = status)
        discard session.update(proc(old: SettingsState): SettingsState =
          result = old
          result.recording = true
          result.status = "")
    let view = settingsView(model.chord, model.recording, model.status, model.trayReady,
      toggleRecording, proc(e: Event) = sg_exit(),
      proc(e: Event) = sg_settings_caption(0),
      proc(e: Event) = sg_settings_caption(1),
      startup = model.startup, startupError = model.startupError, onStartup = toggleStartup,
      update = UpdateUi(snapshot: model.updates, feedback: model.updateFeedback.message,
        channelOpen: model.channelOpen, onChannelOpen: onChannelOpen, onAction: onUpdateAction))
    host(standard.theme((if model.dark: UiColorMode.dark else: UiColorMode.light), view)))

proc requestUpdateRestart*(): bool =
  ## All UI surfaces use this gate, not restartForUpdate directly.
  if restartForUpdate(sg_update_can_restart() != 0):
    sg_update_exit()
    return true

proc runSettingsHost(background, confirming: bool): int =
  let application = app(Settings, title = settingsTitle, width = settingsWidth, height = settingsHeight)
    .withNativeTheme(shadcnLight) # The root theme provider selects the current Windows mode.
  defer:
    recordKey = nil
    trayState = nil
    updateState = nil
    postUpdateAction = nil
    sg_record_hotkey(0)
  let activity = newUiScopeActivity()
  let guarded = guardUiApplication(application, proc(): uint64 = uint64(GetCurrentThreadId()), activity)
  let runtime = newRuntime(guarded, validator = proc(snapshot: Snapshot) = validateProps(snapshot.root))
  defer: runtime.closeRuntime()

  var hostCalls = systemWin32Calls()
  var settingsError: ref Exception
  proc applyUpdate(snapshot: UpdateSnapshot, feedback = none(UpdateFeedback)) =
    sg_update_set_available(cint(snapshot.phase == upReady))
    if updateState != nil:
      runtime.uiTask(proc() = updateState(snapshot, feedback))
      sg_style_settings()

  proc handleUpdateAction(action: UpdateAction; fromTray: bool) =
    let busy = action == uaRestart and sg_update_can_restart() == 0
    let accepted = case action
      of uaStable, uaTest:
        let desired = if action == uaStable: ucStable else: ucTest
        updateSnapshot().settings.channel == desired or changeUpdateChannel(desired)
      of uaAutoOff, uaAutoOn:
        let desired = action == uaAutoOn
        updateSnapshot().settings.autoDownload == desired or changeAutoDownload(desired)
      of uaDownload: downloadUpdate()
      of uaRestart: requestUpdateRestart()
    if action == uaRestart and accepted: return
    let feedback = updateActionFeedback(action, accepted, busy)
    applyUpdate(updateSnapshot(), some(feedback))
    if fromTray and feedback.message.len > 0: sg_update_notice(newWideCString(feedback.message))
  proc finishRecording(chord: uint32) {.raises: [].} =
    if recordKey == nil: return
    let finish = recordKey
    try:
      runtime.uiTask(proc() = finish(chord))
      sg_style_settings()
    except Exception as error:
      settingsError = error
      sg_exit()
  # Leaf owns the settings HWND; the tray alone decides when to reveal it.
  hostCalls.showWindow = proc(window: HWND, command: int32): BOOL = 0
  let dispatchMessage = hostCalls.dispatchMessage
  hostCalls.dispatchMessage = proc(message: ptr MSG): LRESULT =
    let before = runtime.acceptedRevision
    result = dispatchMessage(message)
    if runtime.acceptedRevision != before: sg_style_settings()
    sg_after_dispatch()
  var settingsWindow: HWND
  var confirmationTurn = 0
  let dialogMessage = hostCalls.dialogMessage
  hostCalls.dialogMessage = proc(root: HWND, message: ptr MSG): BOOL =
    # Keep the dialog manager from consuming Escape/clicks on the capture overlay.
    if confirming and message.hwnd == settingsWindow and message.message == updateConfirmMessage:
      try:
        case confirmationTurn
        of 0:
          confirmationTurn = 1
          if not signalUpdateReady() or PostMessageW(settingsWindow, updateConfirmMessage, 0, 0) == 0:
            raise newException(IOError, "update startup release failed")
        of 1:
          confirmationTurn = 2
          if not signalUpdateAck():
            raise newException(IOError, "update startup acknowledgment failed")
        else: discard
      except Exception as error:
        settingsError = error
        sg_update_exit()
      return 1
    if message.hwnd == settingsWindow and message.message == updateMessage:
      if confirming and confirmationTurn < 2: return 1
      try:
        case message.wParam
        of 0: tickUpdates()
        of 1: handleUpdateAction(uaRestart, fromTray = true)
        else:
          if message.wParam in WPARAM(2)..WPARAM(2 + ord(high(UpdateAction))):
            handleUpdateAction(UpdateAction(message.wParam - 2), fromTray = false)
      except Exception as error:
        settingsError = error
        sg_exit()
      return 1
    if message.hwnd == root and message.message == recordMessage:
      finishRecording(uint32(message.wParam))
      return 1
    if message.hwnd == root and message.message == trayMessage:
      if trayState != nil:
        try:
          let ready = message.wParam != 0
          runtime.uiTask(proc() = trayState(ready))
          sg_style_settings()
        except Exception as error:
          settingsError = error
          sg_exit()
      return 1
    if message.hwnd != root and (message.hwnd == nil or isChild(root, message.hwnd) == 0):
      return 0
    let before = runtime.acceptedRevision
    result = dialogMessage(root, message)
    if runtime.acceptedRevision != before: sg_style_settings()
  let window = createHost(application.title,
    DipSize(width: float64(application.width), height: float64(application.height)),
    calls = hostCalls, theme = application.nativeTheme)
  defer: window.destroyHost()
  window.setStartupScope(proc(): bool = activity.busy or runtime.installingUpdate or runtime.installingHook)
  let native = newWin32Backend(window, renderer = application.canvasRenderer)
  let session = native.attachWin32Backend(runtime)
  native.bindSession(session)
  window.bindSession(session, proc(): bool = activity.busy)
  window.setSafeTick(proc() = native.safeListTurn())
  if sg_bind_settings(window.windowHandle()) == 0:
    raise newException(OSError, "无法绑定设置窗口。")
  settingsWindow = window.windowHandle()
  postUpdateAction = proc(action: UpdateAction): bool =
    PostMessageW(settingsWindow, updateMessage, WPARAM(2 + ord(action)), 0) != 0
  applyUpdate(updateSnapshot())
  observeUpdates(proc(snapshot: UpdateSnapshot) = applyUpdate(snapshot))
  defer: observeUpdates(nil)
  sg_ready(cint(background or confirming))
  if confirming and (sg_update_ui_ready() == 0 or
      PostMessageW(settingsWindow, updateConfirmMessage, 0, 0) == 0):
    return 1
  result = window.pump()
  if settingsError != nil: raise settingsError

proc main(): int =
  let background = paramCount() == 1 and paramStr(1) == "--startup"
  let confirming = confirmationMode()
  defer: closeUpdateStartup()
  try:
    if not guardUpdateStartup(): return 0
  except CatchableError as error:
    if not background and not confirming: sg_fatal(error.msg.cstring)
    return 1
  let start = sg_start()
  defer: sg_stop()
  case start
  of 1: return 0
  of 0:
    try:
      initUpdates()
      defer: stopUpdates()
      return runSettingsHost(background, confirming)
    except CatchableError as error:
      if not background and not confirming: sg_fatal(error.msg.cstring)
  else:
    if not background: sg_fatal("无法初始化截图服务。需要 Windows 10 1703 或更新版本。")
  1

when isMainModule:
  quit(main())
