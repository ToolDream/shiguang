## Main-thread adapter. The future Settings view uses these same actions as the tray.
import std/[options, os, strutils, times]
import update_http, update_install, update_package, update_policy, update_service, update_windows
import updater

export update_policy, updater.UpdateSnapshot, updater.UpdatePhase

var
  service: UpdateService
  bridgeError: string
  observer: proc (state: UpdateSnapshot)
  previous: Option[UpdateSnapshot]
  launchGate: uint
  installPin, dataPin: pointer

proc updateSnapshot*(): UpdateSnapshot =
  result = if service == nil: UpdateSnapshot(settings: defaultUpdateSettings(),
    installed: installedBuild(), phase: upCheckFailed) else: service.snapshot
  if result.error == "" and bridgeError != "": result.error = bridgeError

proc observeUpdates*(callback: proc (state: UpdateSnapshot)) =
  observer = callback
  previous = none(UpdateSnapshot)

proc notify() =
  let snapshot = updateSnapshot()
  if previous != some(snapshot):
    previous = some(snapshot)
    if observer != nil: observer(snapshot)

proc initUpdates*() =
  try:
    service = newUpdateService(updateDataDir(), installedBuild(), UpdatePublicKey, fetchUpdateHttps, getAppDir())
  except CatchableError as e:
    bridgeError = e.msg

proc stopUpdates*() =
  if service != nil: service.closeIfIdle()

proc tickUpdates*() =
  if service != nil:
    service.tick(getTime().toUnix())
  notify()

proc changeUpdateChannel*(channel: UpdateChannel): bool =
  if service != nil:
    try:
      result = service.setChannel(channel)
      if result: bridgeError = ""
    except CatchableError as e: bridgeError = e.msg
  notify()

proc changeAutoDownload*(enabled: bool): bool =
  if service != nil:
    try:
      result = service.setAutoDownload(enabled)
      if result: bridgeError = ""
    except CatchableError as e: bridgeError = e.msg
  notify()

proc downloadUpdate*(): bool =
  if service != nil: result = service.download()
  notify()

proc persistPrepared(path, expected: string) =
  let existing = readBounded(path, expected.len, missing = true)
  if existing == "": writeDurable(path, expected)
  elif existing != expected: raise newException(ValueError, "prepared update metadata changed")

proc copyAndLaunch(root, install: string; staged: StagedUpdate) =
  if launchGate == 0: raise newException(IOError, "update start gate not held")
  let pin = pinUpdateDir(root)
  defer: unpinUpdateDir(pin)
  let inventory = installedInventory(UpdatePublicKey, staged.priorBody,
    staged.priorSignature, installedBuild())
  let bytes = checkedHelper(install / HelperName, inventory)
  let helper = root / HelperName
  if fileExists(helper):
    if checkedHelper(helper, inventory) != bytes:
      raise newException(ValueError, "staged update helper was modified")
  elif symlinkExists(helper) or dirExists(helper):
    raise newException(ValueError, "update helper path is already occupied")
  else:
    writeDurable(helper, bytes)
  persistPrepared(root / PreparedPrior, staged.priorBody)
  persistPrepared(root / PreparedPriorSignature, staged.priorSignature)
  let filePin = holdHelper(helper, bytes)
  defer: closeUpdateHandle(filePin)
  if runHelper(newWideCString(helper), newWideCString(root), newWideCString(install),
               launchGate) == 0:
    raise newException(IOError, "unable to start the update helper")

proc restartForUpdate*(safeToExit: bool): bool =
  if service == nil: return false
  let stage = service.beginRestart(safeToExit)
  if stage.isNone: return false
  try:
    if getAppFilename().extractFilename().toLowerAscii() != AppName:
      raise newException(ValueError, "the portable executable must retain its original file name")
    let work = stage.get.directory.parentDir
    persistPrepared(work / PreparedManifest, stage.get.manifestBody)
    persistPrepared(work / PreparedSignature, stage.get.signature)
    copyAndLaunch(work, getAppDir(), stage.get)
    result = true
  except CatchableError as e:
    service.restartFailed(e.msg)
  notify()

proc confirmationMode*(): bool =
  paramCount() >= 1 and paramStr(1) == "--update-ready"

proc closeUpdateStartup*() =
  closeUpdateHandle(launchGate)
  launchGate = 0
  unpinUpdateDir(dataPin)
  dataPin = nil
  unpinUpdateDir(installPin)
  installPin = nil

proc guardUpdateStartup*(): bool =
  ## Keep the gate and pins until the process exits. The installing helper and
  ## its new child inherit the same gate, so there is no unlocked handoff.
  let dir = getAppDir()
  let confirm = confirmationMode()
  let inherited = if confirm:
      (if paramCount() == 5: handleArg(paramStr(5))
       else: raise newException(ValueError, "invalid startup confirmation"))
    else: 0'u
  installPin = pinUpdateDir(dir)
  let data = updateGateDir()
  dataPin = pinUpdateDir(data)
  launchGate = enterUpdateGate(data, dir, inherited)
  if launchGate == 0: return false
  let pending = interrupted(dir)
  if confirm:
    return pending and journalManifest(dir, UpdatePublicKey).build == installedBuild()
  if pending:
    raise newException(IOError,
      "上次更新未完成。请打开拾光目录中的 Shiguang-Recovery\\恢复拾光.exe，再重新打开拾光。")
  true

proc signalUpdateReady*(): bool =
  reachStartupReady(handleArg(paramStr(2)), handleArg(paramStr(3))) != 0

proc signalUpdateAck*(): bool =
  acknowledgeStartup(handleArg(paramStr(4))) != 0
