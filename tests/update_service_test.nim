import std/[base64, json, monotimes, options, os, strutils, tempfiles, times, unittest]
import ../src/[updater, update_policy, update_package]
import update_package_fixture
# Included, not imported: the service-boundary fixtures below need the private
# worker result and stage types to reproduce a collected check/download outcome
# without any network or child process. The public API stays the only product surface.
include ../src/update_service

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
  dsShortSigned, dsShortUnsigned, dsShortForged, dsFullTest, dsFullStableProbe, dsFullStable

# Scripted source: only this value type crosses the thread boundary. A test sets the
# scenario before the worker starts and changes it only after waitPhase saw the joined
# result; the response text itself is built on the fetching thread.
var scenario = dsShortSigned

proc signedEntry(): JsonNode =
  %* {"tag_name": DevTag, "draft": false, "prerelease": true,
      "body": "<!-- shiguang-update-v2 " & encode(RollingBManifest) & " " & RollingBSignature & " -->",
      "assets": [{"name": UpdateZip}]}

proc forgedEntry(): JsonNode =
  ## Routes to the test channel but its signature cannot verify.
  var signature = RollingBSignature
  signature[10] = if signature[10] == 'A': 'B' else: 'A'
  %* {"tag_name": DevTag, "draft": false, "prerelease": true,
      "body": "<!-- shiguang-update-v2 " & encode(RollingBManifest) & " " & signature & " -->",
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
    of dsShortForged: result = jsonList([forgedEntry()])
    of dsFullTest, dsFullStableProbe, dsFullStable: result = fullPage()
  of LatestUrl:
    case scenario
    of dsShortForged: raise newException(IOError, "unexpected latest release query")
    of dsFullStableProbe: raise newException(IOError, "latest probe failed")
    of dsFullTest, dsFullStable: result = ""   # WinHTTP reports API 404 as an empty body.
    else: raise newException(IOError, "unexpected latest release query")
  of "https://github.com/ToolDream/shiguang/releases/download/dev-latest/shiguang-windows-x64.zip":
    result = RollingBZip                       # The signed test package itself.
  of TagUrl:
    case scenario
    of dsShortForged: result = $forgedEntry()
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
    check service.snapshot.result == rrUpToDate     # nothing routed, nothing refused
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

proc waitUntil(s: UpdateService; now: int64; condition: proc(): bool) =
  let until = getMonoTime() + initDuration(seconds = 3)
  while not condition() and getMonoTime() < until:
    s.tick(now)
    sleep(1)
  check condition()

suite "explicit update rounds":
  test "check now downloads, verifies and arms exactly one restart":
    let dir = createTempDir("sg-update-manual-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, fetch, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    check service.checkNow(100000)
    check service.snapshot.autoInstall            # cancellable while the round runs
    check not service.snapshot.installReady       # nothing verified yet
    check service.checkNow(100001)                # a repeat click is accepted, merged
    check service.snapshot.phase == upChecking    # and never opens a second worker
    service.waitPhase(upReady)
    check service.snapshot.installReady
    check not service.snapshot.settings.autoDownload
    check service.snapshot.error.len == 0
    check service.beginRestart(false).isNone
    check service.beginRestart(true).isSome
    check not service.snapshot.installReady
    check service.beginRestart(true).isNone       # one installation per round

  test "cancelling before the result lands leaves the stage manual":
    let dir = createTempDir("sg-update-manual-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, fetch, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    check service.checkNow(100000)
    check service.cancelAutoInstall()
    check not service.cancelAutoInstall()
    service.waitPhase(upAvailable)
    check not service.snapshot.autoInstall
    service.tick(100001)
    check service.snapshot.phase == upAvailable   # no automatic download either
    check not service.snapshot.autoInstall
    check service.download()                      # the manual retry still works
    service.waitPhase(upReady)
    check not service.snapshot.installReady
    check service.beginRestart(true).isSome

  test "a round without an accepted update keeps the stage manual":
    scenario = dsShortSigned
    let dir = createTempDir("sg-update-manual-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    check service.checkNow(100000)
    service.waitPhase(upReady)
    check service.snapshot.installReady
    scenario = dsShortUnsigned
    check service.checkNow(100002)
    service.waitUntil(100002, proc(): bool =
      service.snapshot.phase == upReady and service.snapshot.settings.lastCheck == 100002)
    check service.snapshot.result == rrUpToDate   # nothing new, and not a failure
    check not service.snapshot.installReady
    check service.beginRestart(true).isSome       # verified stage stays available

  test "a qualifying release that fails verification is not \"up to date\"":
    scenario = dsShortSigned
    let dir = createTempDir("sg-update-verify-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    check service.checkNow(100000)
    service.waitPhase(upReady)
    check service.snapshot.installReady
    scenario = dsShortForged                        # same routing, impossible signature
    check service.checkNow(100002)
    service.waitUntil(100002, proc(): bool = service.snapshot.result != rrChecking)
    check service.snapshot.result == rrCheckFailed  # never reported as up to date
    check service.snapshot.error.contains("signature")
    check service.snapshot.phase == upReady         # the verified stage is kept
    check service.snapshot.candidate.isSome
    check not service.snapshot.installReady
    service.tick(100003)
    check service.snapshot.phase == upReady         # and never silently retried
    check service.beginRestart(true).isSome         # manual restart still available

  test "the service opens a cancellable window before the automatic restart":
    scenario = dsShortSigned
    let dir = createTempDir("sg-update-window-", "")
    defer: removeDir(dir)
    writeFile(dir / InventoryName, RollingAInventory)
    writeFile(dir / InventoryProof, RollingAInventoryProof)
    let service = newUpdateService(dir, current, RollingKey, scripted, dir)
    defer: service.close()
    check service.setAutoDownload(false)
    check service.checkNow(100000)
    service.waitPhase(upReady)
    check service.snapshot.installReady
    check not service.installDue(100)      # the visible window starts here
    check not service.installDue(102)
    check service.installDue(103)          # three seconds later it may restart
    check service.installDue(300)          # a busy app keeps the authorization
    check service.cancelAutoInstall()
    check not service.installDue(301)
    check not service.snapshot.installReady
    check service.beginRestart(true).isSome

suite "newer candidate replaces the ready stage":
  test "a newer identity discards the old stage and needs its own verified download":
    let verified = verifyManifest(RollingKey, RollingBManifest, RollingBSignature)
    let newer = verified.build
    for explicit in [true, false]:
      let dir = createTempDir("sg-update-replace-", "")
      defer: removeDir(dir)
      let installed = UpdateBuild(order: 1, revision: "installed", version: "v0.0.1",
        channel: ucTest)
      let cached = UpdateBuild(order: 3, revision: "cached", version: "dev-cached",
        channel: ucTest)
      let service = newUpdateService(dir, installed, RollingKey, neverFetch)
      defer: service.close()
      check service.setAutoDownload(false)
      # A previous round verified the cached identity and left a stage behind.
      check service.policy.complete(service.policy.onTimer(100).get, some(cached))
      check service.policy.complete(service.policy.requestDownload().get)
      check service.snapshot.phase == upReady
      service.staged = some(StagedUpdate(build: cached,
        directory: createTempDir("sg-fake-stage-", "", dir) / "payload"))
      # The collected result of this round is a genuinely different, higher identity.
      let job = if explicit: service.policy.requestCheck(200).job
                else: service.policy.onTimer(100 + CheckInterval)
      check job.isSome
      service.finish(WorkerResult(job: job.get, release: some(Release(manifest: verified))))
      check service.snapshot.candidate == some(newer)
      check service.snapshot.phase == upAvailable    # never ready for an unseen download
      check service.staged.isNone                    # the old identity's stage is gone
      check service.beginRestart(true).isNone        # nothing may install yet
      check not service.installDue(1000)
      check service.snapshot.autoInstall == explicit
      let now = 100 + CheckInterval + 1
      let download = service.policy.onTimer(now)
      if explicit:
        check download.isSome                        # an authorized round skips the switch
        check download.get.build == some(newer)
        # Only this identity's own verified download may become ready.
        service.staged = some(StagedUpdate(build: newer,
          directory: createTempDir("sg-fake-new-", "", dir) / "payload"))
        check service.policy.complete(download.get)
        check service.snapshot.phase == upReady
        check service.beginRestart(true).isSome
      else:
        check download.isNone                        # the background switch is honoured
        check service.download()                     # but the manual entry still exists

  test "checkNow reports acceptance instead of worker creation":
    let dir = createTempDir("sg-update-accept-", "")
    defer: removeDir(dir)
    let service = newUpdateService(dir, current, RollingKey, neverFetch)
    defer: service.close()
    let candidate = verifyManifest(RollingKey, RollingBManifest, RollingBSignature).build
    let running = service.policy.requestCheck(100).job.get
    check service.checkNow(101)                      # merged into the running check
    check service.snapshot.phase == upChecking
    check service.policy.complete(running, some(candidate))
    let download = service.policy.requestDownload().get
    check service.checkNow(102)                      # queued behind the transfer
    check service.snapshot.phase == upDownloading    # no second worker took over
    check service.policy.complete(download)
    check service.snapshot.phase == upReady
    let queued = service.policy.onTimer(103)
    check queued.isSome and queued.get.kind == ujCheck   # the queued click runs once
    check service.snapshot.phase == upChecking
    check service.policy.complete(queued.get, some(candidate))
    check service.snapshot.phase == upReady
    check service.policy.requestRestart(true)
    check not service.checkNow(104)                  # a restart in flight refuses
    service.close()
    check not service.checkNow(105)                  # a closed service refuses
