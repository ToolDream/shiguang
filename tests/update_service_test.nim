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
