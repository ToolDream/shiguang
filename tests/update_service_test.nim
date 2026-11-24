import std/[base64, json, monotimes, options, os, tempfiles, times, unittest]
import ../src/[update_service, updater, update_policy, update_package]
import update_package_fixture

var corrupt: bool

proc fetch(url: string; limit, ms: int): string {.gcsafe.} =
  doAssert ms > 0
  case url
  of "https://api.github.com/repos/ToolDream/shiguang/releases?per_page=30":
    # Rolling history contains only B. Any attempt to fetch A is a test failure.
    let marker = "<!-- shiguang-update-v2 " & encode(RollingBManifest) & " " & RollingBSignature & " -->"
    result = $(%* [{"tag_name": "dev-latest", "draft": false, "prerelease": true,
                   "body": marker, "assets": [{"name": "shiguang-windows-x64.zip"}]}])
  of "https://api.github.com/repos/ToolDream/shiguang/releases/latest":
    result = ""
  of "https://github.com/ToolDream/shiguang/releases/download/dev-latest/shiguang-windows-x64.zip":
    result = if corrupt: RollingAZip else: RollingBZip
  else: raise newException(IOError, "unexpected endpoint")

proc neverFetch(url: string; limit, ms: int): string {.gcsafe.} =
  raise newException(IOError, "unexpected network call")

const
  ListingUrl = "https://api.github.com/repos/ToolDream/shiguang/releases?per_page=30"
  LatestUrl = "https://api.github.com/repos/ToolDream/shiguang/releases/latest"
  TagUrl = "https://api.github.com/repos/ToolDream/shiguang/releases/tags/dev-latest"

type DiscoveryScenario = enum
  dsShortSigned, dsShortUnsigned, dsFullTest, dsFullStableProbe, dsFullStable

# Scripted source: only this value type crosses the thread boundary. A test sets the
# scenario before the worker starts and changes it only after waitPhase saw the joined
# result; the response text itself is built on the fetching thread.
var scenario = dsShortSigned

proc signedEntry(): JsonNode =
  %* {"tag_name": DevTag, "draft": false, "prerelease": true,
      "body": "<!-- shiguang-update-v2 " & encode(RollingBManifest) & " " & RollingBSignature & " -->",
      "assets": [{"name": UpdateZip}]}

proc unsignedEntry(index: int): JsonNode =
  %* {"tag_name": "dev-unused" & $index, "draft": false, "prerelease": true,
      "body": "unsigned record " & $index,
      "assets": [{"name": UpdateZip}]}

proc jsonList(entries: openArray[JsonNode]): string =
  result = "["
  for index in 0 ..< entries.len:
    if index > 0: result.add ','
    result.add $entries[index]
  result.add ']'

proc fullPage(): string =
  var entries = newSeq[JsonNode](30)      # A full page: the result may be truncated.
  for index in 0 ..< entries.len: entries[index] = unsignedEntry(index)
  jsonList(entries)

proc scripted(url: string; limit, ms: int): string {.gcsafe.} =
  doAssert ms > 0
  case url
  of ListingUrl:
    case scenario
    of dsShortSigned: result = jsonList([signedEntry()])
    of dsShortUnsigned: result = jsonList([unsignedEntry(0)])
    of dsFullTest, dsFullStableProbe, dsFullStable: result = fullPage()
  of LatestUrl:
    case scenario
    of dsFullStableProbe: raise newException(IOError, "latest probe failed")
    of dsFullTest, dsFullStable: result = ""   # WinHTTP reports API 404 as an empty body.
    else: raise newException(IOError, "unexpected latest release query")
  of TagUrl:
    case scenario
    of dsFullTest: result = $signedEntry()     # Tag lookup returns one release, not a list.
    of dsFullStableProbe, dsFullStable:
      raise newException(IOError, "stable checks must not query the dev-latest tag")
    else: raise newException(IOError, "unexpected dev-latest query")
  else: raise newException(IOError, "unexpected endpoint")

proc waitPhase(s: UpdateService; phase: UpdatePhase) =
  let until = getMonoTime() + initDuration(seconds = 3)
  while s.snapshot.phase != phase and getMonoTime() < until:
    s.tick(100000)
    sleep(1)
  check s.snapshot.phase == phase

let current = UpdateBuild(order: 41, revision: "fedcba9876543210fedcba9876543210fedcba98", version: "dev-fedcba987654", channel: ucTest)

suite "background update service":
  test "missing trust bridge reports failure without contacting the source":
    let dir = createTempDir("sg-update-service-", "")
    defer: removeDir(dir)
    let service = newUpdateService(dir, current, "", neverFetch)
    defer: service.close()
    service.tick(100000)
    service.waitPhase(upCheckFailed)
    check service.snapshot.error == "update trust is not configured"

  test "stale source data cannot reach ready; verified retry exposes a single stage":
    let dir = createTempDir("sg-update-service-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, fetch, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    service.tick(100000)
    service.waitPhase(upAvailable)
    corrupt = true
    check service.download()
    check not service.download()
    service.waitPhase(upDownloadFailed)
    check service.beginRestart(true).isNone
    corrupt = false
    check service.download()
    service.waitPhase(upReady)
    check service.beginRestart(false).isNone
    let stage = service.beginRestart(true).get
    check stage.priorBody == RollingAInventory
    check readFile(stage.directory / "shiguang.exe") == "app"
    check service.beginRestart(true).isNone

  test "failed throttle persistence is visible and cools down without losing the old value":
    let dir = createTempDir("sg-update-store-", "")
    defer: removeDir(dir)
    let data = dir / "settings"
    createDir(data)
    let state = data / "update.settings"
    saveUpdateSettings(state, UpdateSettings(channel: ucTest, autoDownload: true, lastCheck: 100))
    let service = newUpdateService(data, current, "", neverFetch)
    defer: service.close()
    moveDir(data, dir / "unavailable")
    service.tick(86500, 0)
    check service.snapshot.phase == upCheckFailed
    check service.snapshot.settings.lastCheck == 100
    moveDir(dir / "unavailable", data)
    service.tick(86501, 1)
    check loadUpdateSettings(state).lastCheck == 100
    service.tick(86561, 61)
    check loadUpdateSettings(state).lastCheck == 86561

suite "release discovery":
  test "a complete short page finds the signed test release without extra queries":
    scenario = dsShortSigned
    let dir = createTempDir("sg-update-discovery-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    service.tick(100000)
    service.waitPhase(upAvailable)
    check service.snapshot.error.len == 0
    check service.snapshot.candidate.get.order == 42

  test "a complete short page without a candidate reports idle and asks nothing else":
    scenario = dsShortUnsigned
    let dir = createTempDir("sg-update-discovery-", "")
    defer: removeDir(dir)
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    service.tick(100000)
    service.waitPhase(upIdle)
    check service.snapshot.error.len == 0
    check service.snapshot.candidate.isNone

  test "a full page still uses the dev-latest tag for the test channel":
    scenario = dsFullTest
    let dir = createTempDir("sg-update-discovery-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    service.tick(100000)
    service.waitPhase(upAvailable)
    check service.snapshot.error.len == 0
    check service.snapshot.candidate.get.order == 42

  test "a full page queries /latest for the stable channel and surfaces its failure":
    scenario = dsFullStableProbe             # /latest fails on purpose: it must be reached.
    let dir = createTempDir("sg-update-discovery-", "")
    defer: removeDir(dir)
    saveUpdateSettings(dir / "update.settings",
      UpdateSettings(channel: ucStable, autoDownload: false, lastCheck: 0))
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    service.tick(100000)
    service.waitPhase(upCheckFailed)
    check service.snapshot.error == "latest probe failed"
    check service.snapshot.candidate.isNone

  test "a full page never asks the dev-latest tag for the stable channel":
    scenario = dsFullStable                   # The tag lookup would fail the check.
    let dir = createTempDir("sg-update-discovery-", "")
    defer: removeDir(dir)
    saveUpdateSettings(dir / "update.settings",
      UpdateSettings(channel: ucStable, autoDownload: false, lastCheck: 0))
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    service.tick(100000)
    service.waitPhase(upIdle)
    check service.snapshot.error.len == 0
    check service.snapshot.candidate.isNone

suite "running release inventory":
  test "missing or forged local installed inventory blocks updates without using an old release":
    for content in ["", RollingBInventory]:
      let dir = createTempDir("sg-update-prior-", "")
      defer: removeDir(dir)
      if content.len != 0:
        writeFile(dir / InventoryName, content)
        writeFile(dir / InventoryProof, RollingAInventoryProof)
      let service = newUpdateService(dir, current, RollingKey, fetch, dir)
      defer: service.close()
      service.tick(100000)
      service.waitPhase(upCheckFailed)
      check service.snapshot.candidate.isNone
      check not service.download()
