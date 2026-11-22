## A fixed signed recovery copy runs independently of the installed application.
when not defined(windows):
  {.error: "The executable update helper only runs on Windows.".}

import std/os
import update_install, update_package, update_policy, update_windows

proc installUpdate(dir, root: string; gate: uint): int =
  let m = verifyManifest(UpdatePublicKey,
    readBounded(root / PreparedManifest, MaxManifestBytes), readBounded(root / PreparedSignature, 128))
  if not m.hasInventory: raise newException(ValueError, "release lacks embedded inventory")
  let stage = root / "payload"
  m.requireMatchingInventory(verifyInventory(UpdatePublicKey,
    readBounded(stage / InventoryName, MaxManifestBytes), readBounded(stage / InventoryProof, 128)))
  let prior = readBounded(root / PreparedPrior, MaxManifestBytes)
  let proof = readBounded(root / PreparedPriorSignature, 128)
  let current = installedBuild()
  let previous = installedInventory(UpdatePublicKey, prior, proof, current)
  if not accepts(current, ucTest, m.build):
    raise newException(ValueError, "update helper rejected candidate order")
  # Give the user a known, separately runnable repair entry before publishing
  # the transaction. Do not rotate or delete it while a transaction is pending.
  let helper = checkedHelper(getAppFilename(), previous)
  ensureRecoveryEntry(dir, helper, prior, proof, UpdatePublicKey)
  let recoveryPin = pinUpdateDir(dir / RecoveryDir)
  defer: unpinUpdateDir(recoveryPin)
  let tx = prepareInstall(dir, stage, m, previous, prior, proof)
  var child, ready, go, ack: uint
  var canRecover = true
  try:
    tx.apply()
    if startChild(newWideCString(dir / AppName), newWideCString(dir), gate,
                  addr child, addr ready, addr go, addr ack) == 0: return 1
    canRecover = false
    var committed = false
    try:
      if childReady(child, ready) == 0 or releaseStartup(go) == 0 or
          childAcknowledged(child, ack) == 0 or childAlive(child) == 0:
        return 1
      tx.markConfirmed(proc(): bool = childAlive(child) != 0)
      committed = true
    finally:
      canRecover = childFinish(child, ready, go, ack, cint(committed)) != 0
    if canRecover: return 0
    return 1
  finally:
    if canRecover: recoverInstall(dir, UpdatePublicKey, requirePrior = true)

proc recoverManually(root: string): int =
  try:
    if root.extractFilename != RecoveryDir:
      raise newException(ValueError, "请从拾光目录下的 Shiguang-Recovery 运行恢复程序。")
    let dir = root.parentDir
    let pin = pinUpdateDir(root)
    defer: unpinUpdateDir(pin)
    checkRecoveryEntry(root, UpdatePublicKey)
    let data = updateGateDir()
    let dataPin = pinUpdateDir(data)
    defer: unpinUpdateDir(dataPin)
    let gate = enterUpdateGate(data, dir, 0)
    if gate == 0: raise newException(IOError, "拾光或更新助手正在运行，退出后再试。")
    defer: closeUpdateHandle(gate)
    if not interrupted(dir):
      showRecoveryResult("没有需要恢复的更新。")
      return 0
    let prior = journalPrior(dir, UpdatePublicKey).build
    let candidate = journalManifest(dir, UpdatePublicKey).build
    if not accepts(prior, ucTest, candidate):
      raise newException(ValueError, "恢复事务的已签名版本顺序无效。")
    recoverInstall(dir, UpdatePublicKey, requirePrior = true)
    showRecoveryResult("更新状态已恢复。请重新打开拾光。")
  except CatchableError as e:
    showRecoveryResult("无法恢复，已保留更新事务。\n" & e.msg, error = true)
    result = 1

proc main(): int =
  let root = getAppDir()
  if paramCount() == 0 and getAppFilename().extractFilename == RecoveryExe:
    return recoverManually(root)
  if paramCount() != 4 or paramStr(1) != "--install": return 1
  try:
    let dir = paramStr(4)
    if not dir.isAbsolute: return 1
    let dirPin = pinUpdateDir(dir)
    defer: unpinUpdateDir(dirPin)
    let workPin = pinUpdateDir(root)
    defer: unpinUpdateDir(workPin)
    let data = updateGateDir()
    let dataPin = pinUpdateDir(data)
    defer: unpinUpdateDir(dataPin)
    let gate = enterUpdateGate(data, dir, handleArg(paramStr(3)))
    if gate == 0: raise newException(IOError, "update helper did not inherit the installation gate")
    defer: closeUpdateHandle(gate)
    if waitParent(handleArg(paramStr(2))) == 0:
      raise newException(IOError, "application did not exit before update deadline")
    result = installUpdate(dir, root, gate)
    if result == 0:
      try:
        removeDir(root / "payload")
        for name in [PreparedManifest, PreparedSignature, PreparedPrior, PreparedPriorSignature]:
          removeFile(root / name)
      except OSError: discard
    else:
      writeDurable(root / "result.txt", "Update not confirmed. Use Shiguang-Recovery if needed.\n")
  except CatchableError as e:
    try: writeDurable(root / "result.txt", e.msg & "\n")
    except CatchableError: discard
    result = 1

when isMainModule:
  quit(main())
