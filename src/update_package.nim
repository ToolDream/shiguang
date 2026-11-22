## Only the signed manifest can authorize the exact archive and installed files.
import std/[options, os, strutils, tables]
import update_policy

{.compile: "update_crypto.c".}
when not defined(windows):
  {.passL: "-lcrypto".}

proc sha256(data: pointer; len: uint32; digest: pointer): cint {.importc: "sg_update_sha256".}
proc p256(key, hash, signature: pointer): cint {.importc: "sg_update_p256".}

type
  UpdateFile* = object
    name*: string
    size*: int
    digest*: string
  VerifiedManifest* = object
    identity: UpdateBuild
    url: string
    archiveSize: int
    portable: bool
    archiveHash, signedBody, signedProof: string
    entries: seq[UpdateFile]

const
  MaxManifestBytes* = 16 * 1024
  MaxPackageBytes* = 64 * 1024 * 1024
  MaxEntries = 64
  UpdateRepo* = "ToolDream/shiguang"
  UpdateTarget* = "windows-x64"
  UpdateZip* = "shiguang-windows-x64.zip"
  DevTag* = "dev-latest"
  InventoryName* = "UPDATE-INVENTORY.txt"
  InventoryProof* = "UPDATE-INVENTORY.sig"
  AppName* = "shiguang.exe"
  HelperName* = "shiguang-updater.exe"
  BuildManifest* = "BUILD-MANIFEST.json"
  Checksums* = "SHA256SUMS.txt"
  Hex = {'a'..'f', '0'..'9'}

proc invalid(reason: string) {.noreturn.} =
  raise newException(ValueError, reason)

func build*(m: VerifiedManifest): UpdateBuild = m.identity
func packageUrl*(m: VerifiedManifest): string = m.url
func packageSize*(m: VerifiedManifest): int = m.archiveSize
func signedMetadata*(m: VerifiedManifest): tuple[body, signature: string] =
  (m.signedBody, m.signedProof)

func managedName*(name: string): bool =
  if name in [AppName, HelperName, BuildManifest, Checksums, InventoryName, InventoryProof]: return true
  if name.len < 5 or name.len > 100 or not name.endsWith(".dll") or
      not name.allCharsInSet({'A'..'Z', 'a'..'z', '0'..'9', '.', '_', '-'}):
    return false
  let stem = name.split('.', maxsplit = 1)[0].toLowerAscii()
  if stem.len == 0 or stem in ["con", "prn", "aux", "nul"]: return false
  if stem.len == 4 and stem[0..2] in ["com", "lpt"] and stem[3] in {'0'..'9'}:
    return false
  true

iterator managedFiles*(m: VerifiedManifest): UpdateFile =
  for entry in m.entries:
    if managedName(entry.name): yield entry

proc digestBytes(data: string): array[32, byte] =
  if data.len > MaxPackageBytes or sha256(cstring(data), uint32(data.len), addr result[0]) != 1:
    invalid("update digest failed")

proc digest*(data: string): string =
  result = newStringOfCap(64)
  for b in digestBytes(data): result.add(toHex(b, 2).toLowerAscii())

proc verifyUpdateSignature*(publicKey, body, signature: string): bool =
  if publicKey.len != 128 or signature.len != 128 or body.len == 0 or
      body.len > MaxManifestBytes or not publicKey.allCharsInSet(Hex) or
      not signature.allCharsInSet(Hex):
    return false
  let key = parseHexStr(publicKey)
  let sig = parseHexStr(signature)
  let bodyHash = digestBytes(body)
  p256(cstring(key), unsafeAddr bodyHash[0], cstring(sig)) == 1

proc natural(s: string; max: uint64): uint64 =
  if s.len == 0 or s.len > 20 or s[0] == '0' or not s.allCharsInSet({'0'..'9'}):
    invalid("invalid update number")
  result = parseBiggestUInt(s)
  if result > max: invalid("update size or order out of range")

func releaseTag*(m: VerifiedManifest): string =
  if m.portable and m.identity.channel == ucTest: DevTag else: m.identity.version

func hasInventory*(m: VerifiedManifest): bool = m.portable

proc verifyRecord(publicKey, body, signature: string; inventory: bool): VerifiedManifest =
  if not verifyUpdateSignature(publicKey, body, signature):
    invalid("update manifest signature missing or invalid")
  let lines = body.split('\n')
  let start = if inventory: 7 else: 9
  if lines.len < start + 4 or lines.len > start + MaxEntries + 1 or lines[^1] != "" or
      (if inventory: lines[0] != "shiguang-inventory/1"
       else: lines[0] notin ["shiguang-update/1", "shiguang-update/2"]) or
      lines[1] != UpdateRepo or lines[2] != UpdateTarget:
    invalid("wrong update schema, repository or architecture")
  if lines[6].len != 40 or not lines[6].allCharsInSet(Hex):
    invalid("invalid update source revision")
  let channel = case lines[3]
    of "stable": ucStable
    of "test": ucTest
    else: invalid("invalid update channel")
  let tag = lines[4]
  if (channel == ucStable and stableVersion(tag).isNone) or
     (channel == ucTest and tag != "dev-" & lines[6][0..<12]):
    invalid("invalid update release tag")
  result.identity = UpdateBuild(order: natural(lines[5], high(uint64)),
    revision: lines[6], version: tag, channel: channel)
  result.signedBody = body
  result.signedProof = signature
  result.portable = lines[0] == "shiguang-update/2"
  if not inventory:
    result.archiveSize = int(natural(lines[7], MaxPackageBytes.uint64))
    if lines[8].len != 64 or not lines[8].allCharsInSet(Hex): invalid("invalid ZIP hash")
    result.archiveHash = lines[8]
    result.url = "https://github.com/" & UpdateRepo & "/releases/download/" &
      result.releaseTag & "/" & UpdateZip
  var names = initTable[string, bool]()
  var total = 0
  for i in start ..< lines.high:
    let f = lines[i].split('|')
    if f.len != 3 or not managedName(f[0]) or
        f[2].len != 64 or not f[2].allCharsInSet(Hex) or
        ((inventory or not result.portable) and f[0] in [InventoryName, InventoryProof]):
      invalid("invalid signed file entry")
    let canonical = f[0].toLowerAscii()
    if canonical in names: invalid("duplicate signed file name")
    names[canonical] = true
    let size = int(natural(f[1], MaxPackageBytes.uint64))
    total += size
    if total > MaxPackageBytes: invalid("update contents too large")
    result.entries.add(UpdateFile(name: f[0], size: size, digest: f[2]))
  for required in [AppName, BuildManifest.toLowerAscii(), Checksums.toLowerAscii()]:
    if required notin names: invalid("missing required signed file")

  if inventory or result.portable:
    if HelperName notin names: invalid("missing update helper")
  if result.portable:
    for name in [InventoryName, InventoryProof]:
      if name.toLowerAscii notin names: invalid("missing embedded update inventory")
  if inventory:
    result.entries.add(UpdateFile(name: InventoryName, size: body.len, digest: digest(body)))
    result.entries.add(UpdateFile(name: InventoryProof, size: signature.len, digest: digest(signature)))

proc verifyManifest*(publicKey, body, signature: string): VerifiedManifest =
  verifyRecord(publicKey, body, signature, false)

proc verifyInventory*(publicKey, body, signature: string): VerifiedManifest =
  verifyRecord(publicKey, body, signature, true)

proc requireMatchingInventory*(m, inventory: VerifiedManifest) =
  if m.build != inventory.build or m.entries.len != inventory.entries.len:
    invalid("ZIP and inventory identify different builds")
  for entry in m.entries:
    if entry notin inventory.entries: invalid("ZIP and inventory authorize different files")

proc installedInventory*(key, body, signature: string; installed: UpdateBuild): seq[UpdateFile] =
  let manifest = verifyInventory(key, body, signature)
  if manifest.build != installed: invalid("signed inventory is not for the running build")
  for entry in manifest.managedFiles: result.add(entry)

func word(data: string; p: int): int =
  ord(data[p]) or (ord(data[p + 1]) shl 8)

func dword(data: string; p: int): int =
  word(data, p) or (word(data, p + 2) shl 16)

proc verifiedContents(m: VerifiedManifest; data: string): seq[(UpdateFile, int)] =
  if m.archiveSize == 0 or data.len != m.archiveSize or digest(data) != m.archiveHash:
    invalid("update ZIP size or digest mismatch")
  # ZIP32 STORE only. Check the complete central directory AND contiguous local
  # records before creating a directory. No UTF-8 ambiguity: names are ASCII.
  let endPos = data.len - 22
  if endPos < 0 or dword(data, endPos) != 0x06054b50 or
      dword(data, endPos + 4) != 0 or word(data, endPos + 8) != m.entries.len or
      word(data, endPos + 10) != m.entries.len or word(data, endPos + 20) != 0:
    invalid("unexpected update ZIP end record")
  let boundary = dword(data, endPos + 16)
  if boundary > endPos or dword(data, endPos + 12) != endPos - boundary:
    invalid("invalid update ZIP central directory")
  var c = boundary
  var local = 0
  var entries = initTable[string, UpdateFile]()
  for entry in m.entries: entries[entry.name] = entry
  for _ in 0 ..< m.entries.len:
    if c > endPos - 46 or dword(data, c) != 0x02014b50:
      invalid("invalid ZIP central entry")
    let flags = word(data, c + 8)
    let nameLen = word(data, c + 28)
    let attrs = dword(data, c + 38)
    let kind = attrs shr 28
    if nameLen > endPos - c - 46 or (flags and not 0x800) != 0 or
        word(data, c + 10) != 0 or word(data, c + 30) != 0 or
        word(data, c + 32) != 0 or word(data, c + 34) != 0 or
        (attrs and 0x418) != 0 or kind notin [0, 8] or
        dword(data, c + 42) != local:
      invalid("unsupported ZIP record or unsafe attributes")
    let name = data[c + 46 ..< c + 46 + nameLen]
    if name notin entries: invalid("unknown or duplicate ZIP file")
    let entry = entries[name]
    entries.del(name)
    let size = entry.size
    if dword(data, c + 20) != size or dword(data, c + 24) != size or
        local > boundary - 30 - nameLen - size or
        dword(data, local) != 0x04034b50 or word(data, local + 6) != flags or
        word(data, local + 8) != 0 or dword(data, local + 14) != dword(data, c + 16) or
        dword(data, local + 18) != size or dword(data, local + 22) != size or
        word(data, local + 26) != nameLen or word(data, local + 28) != 0 or
        data[local + 30 ..< local + 30 + nameLen] != name:
      invalid("inconsistent ZIP local header")
    let content = local + 30 + nameLen
    if digest(data[content ..< content + size]) != entry.digest:
      invalid("update file digest mismatch")
    result.add((entry, content))
    local = content + size
    c += 46 + nameLen
  if c != endPos or local != boundary: invalid("unaccounted bytes in ZIP")

proc stagePackage*(m: VerifiedManifest; data, stage: string; key = "") =
  ## Caller supplies a new child of its private staging directory.
  let contents = verifiedContents(m, data)
  if m.hasInventory:
    var body, signature: string
    for (entry, offset) in contents:
      if entry.name == InventoryName and entry.size <= MaxManifestBytes:
        body = data[offset ..< offset + entry.size]
      elif entry.name == InventoryProof and entry.size == 128:
        signature = data[offset ..< offset + entry.size]
    m.requireMatchingInventory(verifyInventory(key, body, signature))
  if symlinkExists(stage) or existsOrCreateDir(stage):
    invalid("update stage already exists")
  var written: seq[string]
  try:
    for (entry, content) in contents:
      if not managedName(entry.name): continue
      let path = stage / entry.name
      written.add(path)
      writeFile(path, data[content ..< content + entry.size])
  except CatchableError:
    for path in written: removeFile(path)
    removeDir(stage)
    raise
