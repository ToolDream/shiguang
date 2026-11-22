when not (defined(windows) and defined(amd64)):
  {.error: "拾光截图的桌面应用需要 Windows x64".}

import leaf
import std/os
import settings_view
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
proc sg_hotkey(): uint32
proc sg_set_hotkey(chord: uint32): cint
proc sg_record_hotkey(recording: cint): cint {.discardable.}
proc sg_settings_caption(action: cint)
proc sg_style_settings()
proc sg_exit()
proc sg_fatal(message: cstring)
{.pop.}

proc isChild(root, window: HWND): BOOL {.
  stdcall, dynlib: "user32.dll", importc: "IsChild", raises: [].}

const
  recordMessage = WM_APP + 20
  trayMessage = WM_APP + 21
var recordKey: proc(chord: uint32)
var trayState: proc(ready: bool)

type SettingsState = object
  chord: uint32
  recording: bool
  status: string
  trayReady: bool
  startup: cint
  startupError: bool
let Settings = defineComponent[tuple[], State[SettingsState]]("Settings",
  proc(ctx: MountContext, props: tuple[]): State[SettingsState] =
    ctx.state(SettingsState(chord: sg_hotkey(), startup: sg_startup_state())),
  proc(ctx: ComponentContext, props: tuple[], session: State[SettingsState]): UiDescription =
    let model = ctx.value(session)
    trayState = proc(ready: bool) =
      discard session.update(proc(old: SettingsState): SettingsState =
        result = old
        result.trayReady = ready
        result.startup = sg_startup_state())
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
    host(settingsView(model.chord, model.recording, model.status, model.trayReady,
      toggleRecording, proc(e: Event) = sg_exit(),
      proc(e: Event) = sg_settings_caption(0),
      proc(e: Event) = sg_settings_caption(1),
      startup = model.startup, startupError = model.startupError, onStartup = toggleStartup)))

proc runSettingsHost(background: bool): int =
  let application = app(Settings, title = settingsTitle, width = settingsWidth, height = settingsHeight)
    .withNativeTheme(shadcnLight) # Native control semantics and metrics; application chrome follows Windows.
  defer:
    recordKey = nil
    trayState = nil
    sg_record_hotkey(0)
  let activity = newUiScopeActivity()
  let guarded = guardUiApplication(application, proc(): uint64 = uint64(GetCurrentThreadId()), activity)
  let runtime = newRuntime(guarded, validator = proc(snapshot: Snapshot) = validateProps(snapshot.root))
  defer: runtime.closeRuntime()

  var hostCalls = systemWin32Calls()
  var settingsError: ref Exception
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
  let dialogMessage = hostCalls.dialogMessage
  hostCalls.dialogMessage = proc(root: HWND, message: ptr MSG): BOOL =
    # Keep the dialog manager from consuming Escape/clicks on the capture overlay.
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
  sg_style_settings()
  sg_ready(cint(background))
  result = window.pump()
  if settingsError != nil: raise settingsError

proc main(): int =
  let background = paramCount() == 1 and paramStr(1) == "--startup"
  let start = sg_start()
  defer: sg_stop()
  case start
  of 1: return 0
  of 0:
    try:
      return runSettingsHost(background)
    except CatchableError as error:
      if not background: sg_fatal(error.msg.cstring)
  else:
    if not background: sg_fatal("无法初始化截图服务。需要 Windows 10 1703 或更新版本。")
  1

when isMainModule:
  quit(main())
