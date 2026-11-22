when defined(windows):
  import std/[os, strutils, tempfiles]
  import update_install, update_package, update_policy

  {.compile: "update_launch_win.c".}
  proc enterGate(data, install: WideCString; inherited: uint): uint {.importc: "sg_update_enter".}
  proc holdHelper*(file, bytes: string): uint =
    result = holdSignedFile(file, digest(bytes), bytes.len)
    if result == 0: raise newException(OSError, "could not lock the signed helper")
  proc closeUpdateHandle*(handle: uint) {.importc: "sg_update_close".}
  proc runHelper*(helper, work, dir: WideCString; gate: uint): cint
    {.importc: "sg_update_run_helper".}
  proc waitParent*(parent: uint): cint {.importc: "sg_update_wait_parent".}
  proc startChild*(exe, dir: WideCString; gate: uint; child, ready, go, ack: ptr uint): cint
    {.importc: "sg_update_start_child".}
  proc childReady*(child, ready: uint): cint {.importc: "sg_update_child_ready".}
  proc childAcknowledged*(child, ack: uint): cint {.importc: "sg_update_child_ack".}
  proc childAlive*(child: uint): cint {.importc: "sg_update_child_alive".}
  proc releaseStartup*(go: uint): cint {.importc: "sg_update_release_child".}
  proc childFinish*(child, ready, go, ack: uint; commit: cint): cint {.importc: "sg_update_child_finish".}
  proc reachStartupReady*(ready, go: uint): cint {.importc: "sg_update_startup_ready".}
  proc acknowledgeStartup*(ack: uint): cint {.importc: "sg_update_startup_ack".}
  proc recoveryMessage(owner: pointer; message, caption: WideCString; flags: uint32): int32
    {.stdcall, dynlib: "user32", importc: "MessageBoxW", header: "<windows.h>".}
  proc showRecoveryResult*(message: string; error = false) =
    discard recoveryMessage(nil, newWideCString(message), newWideCString("拾光更新恢复"),
                             if error: 0x10'u32 else: 0x40'u32)

  proc enterUpdateGate*(data, install: string; inherited: uint): uint =
    enterGate(newWideCString(data), newWideCString(install), inherited)

  const
    UpdatePublicKey* {.strdefine.} = ""
    UpdateOrder {.strdefine.} = "0"
    UpdateRevision {.strdefine.} = ""
    UpdateTag {.strdefine.} = ""
    PreparedManifest* = "signed-manifest.txt"
    PreparedSignature* = "signed-manifest.sig"
    PreparedPrior* = "previous.txt"
    PreparedPriorSignature* = "previous.sig"
    RecoveryDir* = "Shiguang-Recovery"
    RecoveryExe* = "恢复拾光.exe"

  proc installedBuild*(): UpdateBuild =
    let channel = if UpdateTag.startsWith("dev-"): ucTest else: ucStable
    result = UpdateBuild(order: parseBiggestUInt(UpdateOrder), revision: UpdateRevision,
      version: (if UpdateTag != "": UpdateTag elif UpdateRevision.len >= 12:
                  UpdateRevision[0..<12] else: ""), channel: channel)

  proc handleArg*(text: string): uint =
    if text.len == 0 or text.len > 20 or not text.allCharsInSet({'0'..'9'}):
      raise newException(ValueError, "invalid update handle")
    result = uint(parseBiggestUInt(text))
    if result == 0: raise newException(ValueError, "invalid update handle")

  proc updateGateDir*(): string =
    let base = getEnv("LOCALAPPDATA")
    if not base.isAbsolute: raise newException(ValueError, "LOCALAPPDATA unavailable")
    result = base / "ToolDream" / "Shiguang"
    createDir(result)

  proc updateDataDir*(): string =
    result = updateGateDir() / "update"
    createDir(result)

  proc checkedHelper*(path: string; inventory: openArray[UpdateFile]): string =
    for file in inventory:
      if file.name == HelperName:
        result = readBounded(path, file.size)
        if result.len == file.size and digest(result) == file.digest: return
        break
    raise newException(ValueError, "installed updater was not authorized by a signed inventory")

  proc checkRecoveryEntry*(root, key: string) =
    let pin = pinUpdateDir(root)
    defer: unpinUpdateDir(pin)
    for _, file in walkDir(root, relative = true):
      if file notin [RecoveryExe, PreparedPrior, PreparedPriorSignature]:
        raise newException(ValueError, "unknown file in the recovery directory")
    let manifest = verifyInventory(key, readBounded(root / PreparedPrior, MaxManifestBytes),
      readBounded(root / PreparedPriorSignature, 128))
    var files: seq[UpdateFile]
    for entry in manifest.managedFiles: files.add(entry)
    discard checkedHelper(root / RecoveryExe, files)

  proc ensureRecoveryEntry*(dir, helperBytes, body, signature, key: string) =
    let target = dir / RecoveryDir
    # An existing directory may belong to the user. Keep it only if its
    # independent recovery launcher and receipt are both valid.
    if dirExists(target) or fileExists(target) or symlinkExists(target):
      checkRecoveryEntry(target, key)
      return
    let temp = createTempDir(".sg-recovery-prep-", "", dir)
    var published = false
    try:
      writeDurable(temp / RecoveryExe, helperBytes)
      writeDurable(temp / PreparedPrior, body)
      writeDurable(temp / PreparedPriorSignature, signature)
      checkRecoveryEntry(temp, key)
      atomicMove(temp, target, false)
      published = true
    finally:
      if not published: removeDir(temp)
