## A per-install transaction journal is published before touching managed files.
import std/[algorithm, os, strutils, tables, tempfiles]
import update_package

when defined(windows):
  {.compile: "update_identity_win.c".}
  proc directoryId(path, output: WideCString; capacity: csize_t): cint
    {.cdecl, importc: "sg_update_directory_id".}
  proc moveOwnedDir(src, dst, identity: WideCString): cint
    {.cdecl, importc: "sg_update_move_owned_dir".}
  proc rawPin(path: WideCString): pointer {.cdecl, importc: "sg_update_pin".}
  proc unpinUpdateDir*(pin: pointer) {.cdecl, importc: "sg_update_unpin".}
  proc moveVerified(src, dst: WideCString; digest: pointer): cint
    {.cdecl, importc: "sg_update_move_verified".}

  proc holdVerified(file: WideCString; digest: pointer; size: uint32): uint
    {.cdecl, importc: "sg_update_hold_verified".}
  proc closeVerified(handle: uint): int32
    {.stdcall, dynlib: "kernel32", importc: "CloseHandle".}

  proc holdSignedFile*(path, digest: string; size: int): uint =
    let raw = parseHexStr(digest)
    holdVerified(newWideCString(path), cstring(raw), uint32(size))

  proc pinUpdateDir*(path: string): pointer =
    result = rawPin(newWideCString(path))
    if result == nil: raise newException(OSError, "update directory is not a fixed local path")
  proc attributes(path: WideCString): uint32
    {.stdcall, dynlib: "kernel32", importc: "GetFileAttributesW".}
  proc moveAtomic(src, dst: WideCString; flags: uint32): int32
    {.stdcall, dynlib: "kernel32", importc: "MoveFileExW".}
  proc removeEmpty(path: WideCString): int32
    {.stdcall, dynlib: "kernel32", importc: "RemoveDirectoryW".}
  proc writeNew(path: WideCString; bytes: pointer; len: uint32): cint
    {.cdecl, importc: "sg_update_write_new".}
else:
  import std/posix

type
  Change = object
    name, priorHash, nextHash: string
  InstallTransaction* = object
    dir, work, workIdentity: string
    changes: seq[Change]
  ReplaceFile* = proc(src, dst: string) {.closure.}

const
  JournalName* = ".shiguang-update-transaction"
  Old = "old"
  Next = "next"
  Displaced = "displaced"
  PriorBody = "previous.txt"
  PriorProof = "previous.sig"
  Plan = "plan"
  Committed = "confirmed"
  Authorization = "signed.txt"
  Proof = "signature.txt"
  PlanCap = 32 * 1024

proc invalid(reason: string) {.noreturn.} =
  raise newException(ValueError, reason)

proc regularInfo(path: string; missing = false): FileInfo =
  when defined(windows):
    let flags = attributes(newWideCString(path))
    if flags != high(uint32) and (flags and 0x410'u32) != 0:
      invalid("not a plain update file: " & path)
  try:
    result = getFileInfo(path, followSymlink = false)
  except OSError as e:
    when defined(windows):
      if missing and e.errorCode == 2: return
    else:
      if missing and e.errorCode == ENOENT: return
    raise
  if result.kind != pcFile or result.linkCount != 1:
    invalid("not an unshared regular update file: " & path)

proc plainDirectory*(path: string) =
  when defined(windows):
    let flags = attributes(newWideCString(path))
    if flags == high(uint32) or (flags and 0x400'u32) != 0:
      invalid("not a plain update directory: " & path)
  if getFileInfo(path, followSymlink = false).kind != pcDir:
    invalid("not a plain update directory: " & path)

proc readBounded*(path: string; cap: int; missing = false): string =
  let info = regularInfo(path, missing)
  if missing and info.linkCount == 0: return
  if info.size <= 0 or info.size > cap: invalid("invalid update file size: " & path)
  let f = open(path, fmRead)
  defer: f.close()
  result = newString(int(info.size) + 1)
  let n = f.readChars(result)
  if n != info.size: invalid("update file size changed: " & path)
  result.setLen(n)

proc writeDurable*(path, bytes: string) =
  when defined(windows):
    if writeNew(newWideCString(path), cstring(bytes), uint32(bytes.len)) == 0:
      raise newException(IOError, "could not persist a new update file: " & path)
  else:
    let f = open(path, fmWrite)
    defer: f.close()
    f.write(bytes)
    f.flushFile()
    if fsync(f.getFileHandle) != 0: raiseOSError(osLastError())

proc atomicMove*(src, dest: string; replace: bool) =
  when defined(windows):
    if moveAtomic(newWideCString(src), newWideCString(dest),
        (if replace: 0x9'u32 else: 0x8'u32)) == 0:
      raiseOSError(osLastError(), dest)
  else:
    if not replace and (fileExists(dest) or dirExists(dest) or symlinkExists(dest)):
      invalid("update destination already exists")
    moveFile(src, dest)

proc emptyDir(path: string) =
  when defined(windows):
    if removeEmpty(newWideCString(path)) == 0: raiseOSError(osLastError(), path)
  else:
    if rmdir(path.cstring) != 0: raiseOSError(osLastError(), path)

proc interrupted*(dir: string): bool =
  let path = dir / JournalName
  dirExists(path) or fileExists(path) or symlinkExists(path)

proc physicalIdentity(dir: string): string =
  when defined(windows):
    var storage: array[26, Utf16Char]
    let buf = cast[WideCString](addr storage[0])
    if directoryId(newWideCString(dir), buf, storage.len.csize_t) == 0:
      invalid("cannot identify update installation directory")
    result = $buf
  else:
    let id = getFileInfo(dir, followSymlink = false).id
    result = $id.device & "-" & $id.file

template whilePinned(path: string; body: untyped) =
  block:
    when defined(windows):
      let pin = pinUpdateDir(path)
      defer: unpinUpdateDir(pin)
    body

proc cleanup(tx: InstallTransaction) =
  # Renaming the finished journal makes cleanup interruption harmless. Never
  # recurse through a fixed name in the user's directory.
  let parent = createTempDir(".sg-update-done-", "", tx.dir)
  let save = parent / "data"
  when defined(windows):
    if moveOwnedDir(newWideCString(tx.work), newWideCString(save),
                    newWideCString(tx.workIdentity)) == 0:
      invalid("could not remove owned update journal")
  else:
    if physicalIdentity(tx.work) != tx.workIdentity:
      invalid("could not remove owned update journal")
    atomicMove(tx.work, save, false)
  whilePinned(save / Old):
    whilePinned(save / Next):
      whilePinned(save / Displaced):
        for c in tx.changes:
          removeFile(save / Old / c.name)
          removeFile(save / Next / c.name)
          removeFile(save / Displaced / c.name)
        for name in [Plan, Authorization, Proof, PriorBody, PriorProof, Committed, Committed & ".tmp"]:
          removeFile(save / name)
  emptyDir(save / Old)
  emptyDir(save / Next)
  emptyDir(save / Displaced)
  emptyDir(save)
  emptyDir(parent)

proc prepareInstall*(dir, stage: string; manifest: VerifiedManifest;
                     previous: openArray[UpdateFile];
                     previousBody = ""; previousProof = ""): InstallTransaction =
  plainDirectory(dir)
  plainDirectory(stage)
  when defined(windows):
    let rootPin = pinUpdateDir(dir)
    defer: unpinUpdateDir(rootPin)
    let stagePin = pinUpdateDir(stage)
    defer: unpinUpdateDir(stagePin)
  if interrupted(dir): invalid("unfinished update transaction")
  var ownership = initTable[string, UpdateFile]()
  for entry in previous:
    if managedName(entry.name): ownership[entry.name.toLowerAscii()] = entry
  let temp = createTempDir(".sg-update-prepare-", "", dir)
  var plan = "shiguang-install/2\n" & physicalIdentity(dir) & "\n"
  result = InstallTransaction(dir: dir, work: dir / JournalName)
  let (body, signature) = manifest.signedMetadata()
  if body.len == 0 or signature.len != 128:
    emptyDir(temp)
    invalid("missing install authorization")
  var published = false
  try:
    createDir(temp / Old)
    createDir(temp / Next)
    createDir(temp / Displaced)
    for entry in manifest.managedFiles:
      let newBytes = readBounded(stage / entry.name, entry.size)
      if newBytes.len != entry.size or digest(newBytes) != entry.digest:
        invalid("staged file changed: " & entry.name)
      let priorBytes = readBounded(dir / entry.name, MaxPackageBytes, missing = true)
      let priorHash = if priorBytes.len == 0: "-" else: digest(priorBytes)
      let key = entry.name.toLowerAscii()
      if priorBytes.len == 0 and key in ownership:
        invalid("owned file is missing: " & entry.name)
      if priorBytes.len != 0:
        if key notin ownership or ownership[key].digest != priorHash or
            ownership[key].size != priorBytes.len:
          invalid("not an owned update file: " & entry.name)
      if priorHash == entry.digest: continue
      if priorBytes.len != 0:
        writeDurable(temp / Old / entry.name, priorBytes)
      writeDurable(temp / Next / entry.name, newBytes)
      result.changes.add(Change(name: entry.name, priorHash: priorHash, nextHash: entry.digest))
    proc rank(name: string): int =
      # New helper first, old helper restored last. An interruption can always
      # authenticate it from either the signed candidate or the old inventory.
      if name == HelperName: 0
      elif name == AppName: 2
      else: 1
    result.changes.sort(proc(a, b: Change): int = cmp(rank(a.name), rank(b.name)))
    for c in result.changes:
      plan.add(c.name & "|" & c.priorHash & "|" & c.nextHash & "\n")
    writeDurable(temp / Authorization, body)
    writeDurable(temp / Proof, signature)
    if previousBody.len != 0:
      writeDurable(temp / PriorBody, previousBody)
      writeDurable(temp / PriorProof, previousProof)
    writeDurable(temp / Plan, plan)
    atomicMove(temp, result.work, false)
    published = true
  finally:
    if not published: removeDir(temp) # This path was created exclusively by this invocation.

proc observedHash(path: string): string =
  let bytes = readBounded(path, MaxPackageBytes, missing = true)
  if bytes.len == 0: "-" else: digest(bytes)

proc requireHash(path, hash: string) =
  if observedHash(path) != hash: invalid("unexpected update file contents: " & path)

proc renameChecked(src, dst, expected: string) =
  when defined(windows):
    let hash = parseHexStr(expected)
    if moveVerified(newWideCString(src), newWideCString(dst), cstring(hash)) == 0:
      invalid("cannot move verified update file without overwriting: " & src)
  else:
    requireHash(src, expected)
    atomicMove(src, dst, false)

proc applyInternal(tx: InstallTransaction; afterDetach: proc() = nil) =
  whilePinned(tx.work / Next):
    whilePinned(tx.work / Displaced):
      for c in tx.changes:
        let target = tx.dir / c.name
        if c.priorHash != "-":
          renameChecked(target, tx.work / Displaced / c.name, c.priorHash)
        else:
          requireHash(target, "-")
        if afterDetach != nil: afterDetach()
        renameChecked(tx.work / Next / c.name, target, c.nextHash)

proc apply*(tx: InstallTransaction) =
  tx.applyInternal()

proc apply*(tx: InstallTransaction; replace: ReplaceFile) =
  # Fault injection for the portable transaction tests, not the helper.
  for c in tx.changes:
    let src = tx.work / Next / c.name
    let target = tx.dir / c.name
    requireHash(src, c.nextHash)
    requireHash(target, c.priorHash)
    replace(src, target)

proc applyDetached*(tx: InstallTransaction; pause: proc()) =
  tx.applyInternal(pause)

proc readTransaction(dir: string): InstallTransaction =
  plainDirectory(dir)
  result = InstallTransaction(dir: dir, work: dir / JournalName)
  for path in [result.work, result.work / Old, result.work / Next, result.work / Displaced]: plainDirectory(path)
  result.workIdentity = physicalIdentity(result.work)
  let lines = readBounded(result.work / Plan, PlanCap).split('\n')
  if lines.len < 3 or lines.len > 67 or lines[0] != "shiguang-install/2" or
      lines[1] != physicalIdentity(dir) or lines[^1] != "":
    invalid("invalid update recovery journal")
  proc isHash(s: string): bool = s.len == 64 and s.allCharsInSet({'0'..'9', 'a'..'f'})
  var seen = initTable[string, bool]()
  for i in 2 ..< lines.high:
    let v = lines[i].split('|')
    if v.len != 3 or not managedName(v[0]) or
        (v[1] != "-" and not isHash(v[1])) or not isHash(v[2]) or
        v[0].toLowerAscii() in seen or v[1] == v[2]:
      invalid("invalid recovery journal entry")
    seen[v[0].toLowerAscii()] = true
    result.changes.add(Change(name: v[0], priorHash: v[1], nextHash: v[2]))

proc journalManifest*(dir, key: string): VerifiedManifest =
  let work = dir / JournalName
  plainDirectory(dir)
  plainDirectory(work)
  verifyManifest(key, readBounded(work / Authorization, MaxManifestBytes),
    readBounded(work / Proof, 128))

proc journalPrior*(dir, key: string): VerifiedManifest =
  let work = dir / JournalName
  plainDirectory(dir)
  plainDirectory(work)
  verifyInventory(key, readBounded(work / PriorBody, MaxManifestBytes),
    readBounded(work / PriorProof, 128))

proc recoverInstall*(dir, key: string; requirePrior = false) =
  if not interrupted(dir): return
  when defined(windows):
    let installPin = pinUpdateDir(dir)
    defer: unpinUpdateDir(installPin)
    var verifiedFiles: seq[uint]
    defer:
      for handle in verifiedFiles: discard closeVerified(handle)
  var tx: InstallTransaction
  whilePinned(dir / JournalName):
    tx = readTransaction(dir)
    let signed = journalManifest(dir, key)
    var previous: VerifiedManifest
    if requirePrior: previous = journalPrior(dir, key)
    for change in tx.changes:
      var authorized = false
      for entry in signed.managedFiles:
        if entry.name == change.name and entry.digest == change.nextHash:
          authorized = true
      if not authorized: invalid("recovery change was not authorized by the signed release")
      if requirePrior:
        var expected = "-"
        for entry in previous.managedFiles:
          if entry.name.toLowerAscii == change.name.toLowerAscii: expected = entry.digest
        if change.priorHash != expected:
          invalid("recovery inventory does not authorize old file: " & change.name)
    let commit = readBounded(tx.work / Committed, 16, missing = true)
    if commit == "":
      whilePinned(tx.work / Old):
        whilePinned(tx.work / Next):
          whilePinned(tx.work / Displaced):
            for c in tx.changes:
              let h = observedHash(dir / c.name)
              if h == c.priorHash: continue
              # A gap after detachment can leave the original name absent.
              # Restoring the signed backup does not overwrite anything.
              if h != c.nextHash and h != "-":
                invalid("update recovery found an unrelated file: " & c.name)
              if c.priorHash != "-": requireHash(tx.work / Old / c.name, c.priorHash)
            for i in countdown(tx.changes.high, 0):
              let c = tx.changes[i]
              let target = dir / c.name
              let actual = observedHash(target)
              if actual == c.priorHash: continue
              if actual != "-" and actual != c.nextHash:
                invalid("update recovery will not overwrite a changed file: " & c.name)
              if actual == c.nextHash:
                renameChecked(target, tx.work / Next / c.name, c.nextHash)
              if c.priorHash != "-":
                renameChecked(tx.work / Old / c.name, target, c.priorHash)
    elif commit == "ok\n":
      # Confirmation describes a past observation, not the files after a
      # restart. Verify the signed inventory, including unchanged files.
      for entry in signed.managedFiles:
        let path = dir / entry.name
        when defined(windows):
          let handle = holdSignedFile(path, entry.digest, entry.size)
          if handle == 0: invalid("committed update file could not be verified: " & path)
          verifiedFiles.add(handle) # Keep write/delete exclusion through cleanup.
        else:
          let bytes = readBounded(path, entry.size, missing = true)
          if bytes.len != entry.size or digest(bytes) != entry.digest:
            invalid("committed update file could not be verified: " & path)
    else:
      invalid("invalid update commit marker")
  tx.cleanup()

proc markConfirmed*(tx: InstallTransaction; stillAlive: proc(): bool = nil) =
  for c in tx.changes: requireHash(tx.dir / c.name, c.nextHash)
  whilePinned(tx.work):
    writeDurable(tx.work / (Committed & ".tmp"), "ok\n")
    if stillAlive != nil and not stillAlive():
      raise newException(IOError, "new application exited before commit")
    atomicMove(tx.work / (Committed & ".tmp"), tx.work / Committed, false)

proc installAndCheck*(dir, stage: string; manifest: VerifiedManifest;
                      previous: openArray[UpdateFile]; publicKey: string; started: proc(): bool;
                      replace: ReplaceFile = nil): bool =
  let tx = prepareInstall(dir, stage, manifest, previous)
  try:
    if replace == nil: tx.apply()
    else: tx.apply(replace)
    result = started()
    if result: tx.markConfirmed()
  finally:
    recoverInstall(dir, publicKey)
