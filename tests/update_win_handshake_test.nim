import std/[os, tempfiles, unittest]
import ../src/[update_install, update_windows]
import update_package_fixture

proc removeFixtureDir(path: WideCString): int32
  {.stdcall, dynlib: "kernel32", importc: "RemoveDirectoryW".}
proc createFixtureDir(path: WideCString; security: pointer): int32
  {.stdcall, dynlib: "kernel32", importc: "CreateDirectoryW".}

if paramCount() == 5 and paramStr(1) == "--update-ready":
  if getEnv("SG_FIXTURE_CRASH") == "before-ready": quit(7)
  let ready = handleArg(paramStr(2))
  let go = handleArg(paramStr(3))
  let ack = handleArg(paramStr(4))
  if reachStartupReady(ready, go) == 0: quit(1)
  if getEnv("SG_FIXTURE_CRASH") == "before-ack": quit(8)
  if acknowledgeStartup(ack) == 0: quit(9)
  sleep(1000)
  quit(0)

suite "Windows updater startup confirmation":
  test "the inherited gate blocks other regular startup and an ACK is required":
    let root = createTempDir("sg-update-gate-", "")
    defer: removeDir(root)
    let lock = enterUpdateGate(root, getAppDir(), 0)
    require lock != 0
    defer: closeUpdateHandle(lock)
    check enterUpdateGate(root, getAppDir(), 0) == 0
    var child, ready, go, ack: uint
    require startChild(newWideCString(getAppFilename()), newWideCString(getAppDir()),
      lock, addr child, addr ready, addr go, addr ack) != 0
    let held = childReady(child, ready) != 0
    check held
    check releaseStartup(0) == 0
    check releaseStartup(go) != 0
    check childAcknowledged(child, ack) != 0
    check childFinish(child, ready, go, ack, 0) != 0

  test "ready followed by exit does not count as confirmed":
    let root = createTempDir("sg-update-gate-", "")
    defer: removeDir(root)
    let lock = enterUpdateGate(root, getAppDir(), 0)
    require lock != 0
    defer: closeUpdateHandle(lock)
    putEnv("SG_FIXTURE_CRASH", "before-ack")
    defer: delEnv("SG_FIXTURE_CRASH")
    var child, ready, go, ack: uint
    require startChild(newWideCString(getAppFilename()), newWideCString(getAppDir()),
      lock, addr child, addr ready, addr go, addr ack) != 0
    check childReady(child, ready) != 0
    check releaseStartup(go) != 0
    check childAcknowledged(child, ack) == 0
    check childFinish(child, ready, go, ack, 0) != 0

  test "pin prevents renaming an ancestor and rejects substituting the install identity":
    let root = createTempDir("sg-update-identity-", "")
    defer: removeDir(root)
    let parent = root / "parent"
    let install = parent / "install"
    let other = root / "other"
    createDir(install)
    createDir(other)
    let pin = pinUpdateDir(install)
    try:
      expect OSError:
        moveDir(parent, root / "renamed")
      let gate = enterUpdateGate(root, install, 0)
      require gate != 0
      defer: closeUpdateHandle(gate)
      check enterUpdateGate(root, other, gate) == 0
    finally:
      unpinUpdateDir(pin)


  test "pinned paths cannot be deleted or replaced and stay usable by the gate":
    let root = createTempDir("sg-update-delete-", "")
    defer: removeDir(root)
    let parent = root / "parent"
    let install = parent / "install"
    createDir(install)
    let parentPath = newWideCString(parent)
    let installPath = newWideCString(install)
    let parentId = getFileInfo(parent, followSymlink = false).id
    let installId = getFileInfo(install, followSymlink = false).id
    let pin = pinUpdateDir(install)
    try:
      for path in [install, parent]:
        let removed = removeFixtureDir(newWideCString(path))
        let removeError = osLastError()
        check removed == 0
        # Distinguish sharing protection from merely hitting a nonempty parent.
        check removeError == OSErrorCode(32)
      for path in [parent, install]:
        let created = createFixtureDir(newWideCString(path), nil)
        let createError = osLastError()
        check created == 0
        check createError == OSErrorCode(183)
      check getFileInfo(parent, followSymlink = false).id == parentId
      check getFileInfo(install, followSymlink = false).id == installId
      let gate = enterUpdateGate(root, install, 0)
      require gate != 0
      closeUpdateHandle(gate)
    finally:
      unpinUpdateDir(pin)
    require removeFixtureDir(installPath) != 0
    require removeFixtureDir(parentPath) != 0
    require createFixtureDir(parentPath, nil) != 0
    require createFixtureDir(installPath, nil) != 0

  test "an unrelated fixed recovery directory is not overwritten":
    let root = createTempDir("sg-update-recovery-", "")
    defer: removeDir(root)
    let recovery = root / RecoveryDir
    createDir(recovery)
    writeFile(recovery / "personal.txt", "keep me")
    expect ValueError:
      ensureRecoveryEntry(root, "untrusted", "", "", "")
    check readFile(recovery / "personal.txt") == "keep me"
    check not fileExists(recovery / RecoveryExe)

  test "the signed fixed recovery copy remains independent of the main executable":
    let root = createTempDir("sg-update-recovery-", "")
    defer: removeDir(root)
    ensureRecoveryEntry(root, "old helper", RollingAInventory, RollingAInventoryProof, RollingKey)
    checkRecoveryEntry(root / RecoveryDir, RollingKey)
    check not fileExists(root / "shiguang.exe")
    ensureRecoveryEntry(root, "would not replace existing", RollingAInventory, RollingAInventoryProof, RollingKey)
    check readFile(root / RecoveryDir / RecoveryExe) == "old helper"
