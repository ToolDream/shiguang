when not (defined(windows) and defined(amd64)):
  {.error: "拾光截图的桌面应用需要 Windows x64".}

import leaf
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
proc sg_ready()
proc sg_after_dispatch()
proc sg_hotkey(): uint32
proc sg_set_hotkey(chord: uint32): cint
proc sg_record_hotkey(recording: cint): cint {.discardable.}
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

type SettingsState = tuple[chord: uint32, recording: bool, status: string, trayReady: bool]
let Settings = defineComponent[tuple[], State[SettingsState]]("Settings",
  proc(ctx: MountContext, props: tuple[]): State[SettingsState] =
    ctx.state((sg_hotkey(), false, "", false)),
  proc(ctx: ComponentContext, props: tuple[], session: State[SettingsState]): UiDescription =
    let model = ctx.value(session)
    trayState = proc(ready: bool) =
      discard session.update(proc(old: SettingsState): SettingsState =
        (old.chord, old.recording, old.status, ready))
    let toggleRecording: Callback = proc(e: Event) =
      if model.recording:
        sg_record_hotkey(0)
        recordKey = nil
        discard session.update(proc(old: SettingsState): SettingsState = (old.chord, false, "", old.trayReady))
      elif sg_record_hotkey(1) == 0:
        discard session.update(proc(old: SettingsState): SettingsState =
          (old.chord, false, "无法开始录入，请重新打开设置后再试。", old.trayReady))
      else:
        recordKey = proc(chord: uint32) =
          sg_record_hotkey(0)
          recordKey = nil
          let status =
            if chord == 0: ""
            else:
              case sg_set_hotkey(chord)
              of 0: ""
              of 1: "此组合键无效或已被占用，未更改。"
              else: "无法保存组合键，未更改。"
          discard session.update(proc(old: SettingsState): SettingsState =
            (sg_hotkey(), false, status, old.trayReady))
        discard session.update(proc(old: SettingsState): SettingsState =
          (old.chord, true, "", old.trayReady))
    host(settingsView(model.chord, model.recording, model.status, model.trayReady,
      toggleRecording, proc(e: Event) = sg_exit())))

proc runSettingsHost(): int =
  let application = app(Settings, title = "拾光截图 / 设置", width = 544, height = 276)
    .withNativeTheme(shadcnLight)
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
    except Exception as error:
      settingsError = error
      sg_exit()
  # Leaf owns the settings HWND; the tray alone decides when to reveal it.
  hostCalls.showWindow = proc(window: HWND, command: int32): BOOL = 0
  let dispatchMessage = hostCalls.dispatchMessage
  hostCalls.dispatchMessage = proc(message: ptr MSG): LRESULT =
    result = dispatchMessage(message)
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
        except Exception as error:
          settingsError = error
          sg_exit()
      return 1
    if message.hwnd != root and (message.hwnd == nil or isChild(root, message.hwnd) == 0):
      return 0
    dialogMessage(root, message)
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
  sg_ready()
  result = window.pump()
  if settingsError != nil: raise settingsError

proc main(): int =
  let start = sg_start()
  defer: sg_stop()
  case start
  of 1: return 0
  of 0:
    try:
      return runSettingsHost()
    except CatchableError as error:
      sg_fatal(error.msg.cstring)
  else:
    sg_fatal("无法初始化截图服务。需要 Windows 10 1703 或更新版本。")
  1

when isMainModule:
  quit(main())
