import std/[os, strutils, tempfiles, unittest]
import ../src/[update_install, update_package]
import update_package_fixture

when defined(windows) and defined(updatePinDiagnostics):
  if paramCount() == 1 and paramStr(1) == "--diagnose-pin-roots":
    # Test-only comparison, not a fallback for installation or normal tests.
    for (label, base) in [("default-temp", getTempDir()), ("workspace", getCurrentDir())]:
      try:
        let root = createTempDir("sg-update-ci-pin-", "", base)
        defer: removeDir(root)
        let dir = root / "install"
        createDir(dir)
        echo "[pin-probe] ", label, " input=[", dir, "]"
        let pin = pinUpdateDir(dir)
        unpinUpdateDir(pin)
        echo "[pin-probe] ", label, " accepted"
      except CatchableError as e:
        echo "[pin-probe] ", label, " rejected: ", e.msg
    quit(0)

const names = ["runtime.dll", "shiguang.exe"]
let signed = verifyManifest(PublicKey, GoodManifest, GoodSignature)

proc oldFiles(): seq[UpdateFile] =
  for (name, bytes) in [(names[0], "old dll"), (names[1], "old exe")]:
    result.add(UpdateFile(name: name, size: bytes.len, digest: digest(bytes)))

template installation(body: untyped) =
  let root {.inject.} = createTempDir("sg-install-test-", "")
  defer: removeDir(root)
  let dir {.inject.} = root / "install"
  createDir(dir)
  writeFile(dir / names[0], "old dll")
  writeFile(dir / names[1], "old exe")
  writeFile(dir / "settings.ini", "user")
  let stage {.inject.} = root / "stage"
  stagePackage(signed, GoodZip, stage)
  body

proc checkOld(dir: string) =
  check readFile(dir / names[0]) == "old dll"
  check readFile(dir / names[1]) == "old exe"
  check readFile(dir / "settings.ini") == "user"
  check not interrupted(dir)

suite "install transaction":
  test "only owned matching files may be replaced":
    installation:
      expect ValueError:
        discard prepareInstall(dir, stage, signed, @[oldFiles()[1]])
      checkOld(dir)

  test "modified stage or existing DLL aborts before changing the installation":
    installation:
      writeFile(stage / names[0], "tampered")
      expect ValueError:
        discard prepareInstall(dir, stage, signed, oldFiles())
      checkOld(dir)
      writeFile(stage / names[0], "dll")
      writeFile(dir / names[0], "custom")
      expect ValueError:
        discard prepareInstall(dir, stage, signed, oldFiles())
      check readFile(dir / names[0]) == "custom"
      check not interrupted(dir)

  test "startup failure rolls back App and DLL, successful confirmation keeps them":
    installation:
      check not installAndCheck(dir, stage, signed, oldFiles(), PublicKey, proc(): bool = false)
      checkOld(dir)
      check installAndCheck(dir, stage, signed, oldFiles(), PublicKey, proc(): bool =
        readFile(dir / names[0]) == "dll" and readFile(dir / names[1]) == "app")
      check readFile(dir / names[0]) == "dll"
      check readFile(dir / names[1]) == "app"
      check readFile(dir / "settings.ini") == "user"
      check not interrupted(dir)

  test "failed replacement restores prior files when recovery can safely proceed":
    installation:
      var calls = 0
      expect IOError:
        discard installAndCheck(dir, stage, signed, oldFiles(), PublicKey,
          proc(): bool = true,
          proc(src, dst: string) =
            inc calls
            if calls == 2: raise newException(IOError, "sharing violation")
            moveFile(src, dst))
      checkOld(dir)

  test "interrupted replacement can be recovered by a fresh helper":
    installation:
      let tx = prepareInstall(dir, stage, signed, oldFiles())
      tx.apply()
      check interrupted(dir)
      check readFile(dir / names[1]) == "app"
      recoverInstall(dir, PublicKey)
      checkOld(dir)

  when not defined(windows):
    test "symlinks are rejected without following them":
      installation:
        removeFile(dir / names[0])
        createSymlink(dir / "settings.ini", dir / names[0])
        expect ValueError:
          discard prepareInstall(dir, stage, signed, oldFiles())
        check readFile(dir / "settings.ini") == "user"
        check not interrupted(dir)

  test "recovery validates the transaction against its signed candidate":
    installation:
      let tx = prepareInstall(dir, stage, signed, oldFiles())
      tx.apply()
      expect ValueError:
        recoverInstall(dir, "")
      check readFile(dir / names[1]) == "app"
      recoverInstall(dir, PublicKey)
      checkOld(dir)

  test "every replace cut can recover the matching old executable and DLL":
    var mixed = false
    for cut in 0..4:
      installation:
        let tx = prepareInstall(dir, stage, signed, oldFiles())
        var replaced = 0
        try:
          tx.apply(proc(src, dst: string) =
            if replaced == cut: raise newException(IOError, "interruption")
            moveFile(src, dst)
            inc replaced)
        except IOError:
          discard
        mixed = mixed or ((readFile(dir / names[0]) == "dll") != (readFile(dir / names[1]) == "app"))
        recoverInstall(dir, PublicKey)
        checkOld(dir)
    check mixed

  test "a modified unsigned inventory does not establish the installed build identity":
    installation:
      let installed = verifyInventory(RollingKey, RollingBInventory, RollingBInventoryProof).build
      check installedInventory(RollingKey, RollingBInventory, RollingBInventoryProof, installed).len == 7
      expect ValueError:
        discard installedInventory(RollingKey,
          RollingBInventory.replace(digest("dll"), digest("modified")), RollingBInventoryProof, installed)

  test "recovery also covers the gap after removing an old name before promotion":
    for cut in 0 .. 3:
      installation:
        let tx = prepareInstall(dir, stage, signed, oldFiles())
        var seen = 0
        expect IOError:
          tx.applyDetached(proc() =
            if seen == cut: raise newException(IOError, "interruption before promotion")
            inc seen)
        recoverInstall(dir, PublicKey)
        checkOld(dir)

  test "an interrupted journal cannot be transplanted into a different directory":
    installation:
      let tx = prepareInstall(dir, stage, signed, oldFiles())
      tx.apply()
      let other = root / "other"
      createDir(other)
      moveDir(dir / JournalName, other / JournalName)
      expect ValueError:
        recoverInstall(other, PublicKey)
      check interrupted(other)
      check not fileExists(other / names[1])
      moveDir(other / JournalName, dir / JournalName)
      recoverInstall(dir, PublicKey)
      checkOld(dir)

  test "concurrent insertion between detach and promotion never gets overwritten":
    installation:
      let tx = prepareInstall(dir, stage, signed, oldFiles())
      expect ValueError:
        tx.applyDetached(proc() = writeFile(dir / names[0], "user-owned replacement"))
      expect ValueError:
        recoverInstall(dir, PublicKey)
      check readFile(dir / names[0]) == "user-owned replacement"
      check interrupted(dir)

  when defined(windows):
    test "prepare apply rollback and cleanup work with the install ancestor pinned":
      installation:
        let pin = pinUpdateDir(dir.parentDir)
        defer: unpinUpdateDir(pin)
        let installPin = pinUpdateDir(dir)
        defer: unpinUpdateDir(installPin)
        let tx = prepareInstall(dir, stage, signed, oldFiles())
        check interrupted(dir)
        tx.apply()
        check readFile(dir / AppName) == "app"
        recoverInstall(dir, PublicKey)
        checkOld(dir)

  test "first-install A can update to rolling B and roll back with only local proofs":
    let root = createTempDir("sg-rolling-offline-", "")
    defer: removeDir(root)
    let dir = root / "installed"
    let stage = root / "staged"
    let a = verifyManifest(RollingKey, RollingAManifest, RollingASignature)
    let b = verifyManifest(RollingKey, RollingBManifest, RollingBSignature)
    stagePackage(a, RollingAZip, dir, RollingKey)
    stagePackage(b, RollingBZip, stage, RollingKey)
    let inventory = installedInventory(RollingKey, readFile(dir / InventoryName),
      readFile(dir / InventoryProof), a.build)
    let tx = prepareInstall(dir, stage, b, inventory, RollingAInventory, RollingAInventoryProof)
    tx.apply()
    let stored = dir / JournalName / "previous.txt"
    writeFile(stored, RollingAInventory.replace(digest("old dll"), digest("other dll")))
    expect ValueError:
      recoverInstall(dir, RollingKey, requirePrior = true)
    check readFile(dir / names[0]) == "dll"
    writeFile(stored, RollingAInventory)
    recoverInstall(dir, RollingKey, requirePrior = true)
    check readFile(dir / names[1]) == "old exe"
    check readFile(dir / names[0]) == "old dll"
    check readFile(dir / HelperName) == "old helper"
    check readFile(dir / InventoryName) == RollingAInventory
    check readFile(dir / InventoryProof) == RollingAInventoryProof
    check not interrupted(dir)

  test "lost child at final commit check leaves rollback data":
    installation:
      let tx = prepareInstall(dir, stage, signed, oldFiles())
      tx.apply()
      expect IOError:
        tx.markConfirmed(proc(): bool = false)
      check interrupted(dir)
      recoverInstall(dir, PublicKey)
      checkOld(dir)

template committedInstall(body: untyped) =
  # Both runtime.dll and the helper stay unchanged, so neither is listed in
  # the transaction's changes. The signed candidate still manages them.
  let root = createTempDir("sg-committed-test-", "")
  defer: removeDir(root)
  let dir {.inject.} = root / "install"
  let stage = root / "stage"
  createDir(dir)
  createDir(stage)
  let candidate = verifyManifest(RecoveryKey, RecoveryPrior, RecoveryPriorSig)
  for (name, bytes) in [(AppName, "old exe"), ("runtime.dll", "old dll"),
                        (HelperName, "recovery-fixture"), (BuildManifest, "old manifest"),
                        (Checksums, "old checksums")]:
    writeFile(stage / name, bytes)
  var previous: seq[UpdateFile]
  for entry in candidate.managedFiles:
    let bytes = if entry.name == AppName: "prior exe" else: readFile(stage / entry.name)
    writeFile(dir / entry.name, bytes)
    previous.add(UpdateFile(name: entry.name, size: bytes.len, digest: digest(bytes)))
  writeFile(dir / "personal.txt", "user data")
  createDir(dir / "Shiguang-Recovery")
  writeFile(dir / "Shiguang-Recovery" / "recovery-fixture", "keep recovery")
  let tx = prepareInstall(dir, stage, candidate, previous)
  tx.apply()
  tx.markConfirmed()
  body

proc checkCommittedEvidence(dir: string) =
  let journal = dir / JournalName
  check interrupted(dir)
  check fileExists(journal / "old" / AppName)
  if fileExists(journal / "old" / AppName):
    check readFile(journal / "old" / AppName) == "prior exe"
  check fileExists(journal / "confirmed")
  check readFile(dir / "personal.txt") == "user data"
  check readFile(dir / "Shiguang-Recovery" / "recovery-fixture") == "keep recovery"

suite "committed update cleanup":
  test "missing main executable retains the journal, old executable and recovery entry":
    committedInstall:
      removeFile(dir / AppName)
      for attempt in 1..2:
        expect ValueError:
          recoverInstall(dir, RecoveryKey)
        checkCommittedEvidence(dir)
        check not fileExists(dir / AppName)

  test "same-sized unknown replacement for unchanged DLL is never overwritten":
    committedInstall:
      writeFile(dir / "runtime.dll", "new dll")
      expect ValueError:
        recoverInstall(dir, RecoveryKey)
      checkCommittedEvidence(dir)
      check readFile(dir / "runtime.dll") == "new dll"

  test "missing or oversized unchanged helper retains the journal":
    committedInstall:
      removeFile(dir / HelperName)
      expect ValueError:
        recoverInstall(dir, RecoveryKey)
      checkCommittedEvidence(dir)
      check not fileExists(dir / HelperName)
      writeFile(dir / HelperName, "unknown helper larger than the signed helper")
      expect ValueError:
        recoverInstall(dir, RecoveryKey)
      checkCommittedEvidence(dir)
      check readFile(dir / HelperName) == "unknown helper larger than the signed helper"

  when not defined(windows):
    test "byte-identical linked target is not a regular unshared installed file":
      committedInstall:
        removeFile(dir / HelperName)
        writeFile(dir / "other-helper.exe", "recovery-fixture")
        createHardlink(dir / "other-helper.exe", dir / HelperName)
        expect ValueError:
          recoverInstall(dir, RecoveryKey)
        checkCommittedEvidence(dir)
        check readFile(dir / "other-helper.exe") == "recovery-fixture"

  test "full matching signed installation cleans up and repeat recovery is a no-op":
    committedInstall:
      check not fileExists(dir / JournalName / "old" / "runtime.dll")
      check not fileExists(dir / JournalName / "old" / HelperName)
      recoverInstall(dir, RecoveryKey)
      check not interrupted(dir)
      recoverInstall(dir, RecoveryKey)
      check not interrupted(dir)
      check readFile(dir / AppName) == "old exe"
      check readFile(dir / "runtime.dll") == "old dll"
      check readFile(dir / HelperName) == "recovery-fixture"
      check readFile(dir / "personal.txt") == "user data"
      check readFile(dir / "Shiguang-Recovery" / "recovery-fixture") == "keep recovery"

  when defined(windows):
    test "a target open for writing blocks committed cleanup until retry":
      committedInstall:
        let openForWrite = open(dir / HelperName, fmReadWriteExisting)
        try:
          expect ValueError:
            recoverInstall(dir, RecoveryKey)
          checkCommittedEvidence(dir)
        finally:
          openForWrite.close()
        recoverInstall(dir, RecoveryKey)
        check not interrupted(dir)
