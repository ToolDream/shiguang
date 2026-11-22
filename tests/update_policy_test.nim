import std/[options, os, tempfiles, unittest]
import ../src/[update_policy, updater]

proc build(n: uint64; revision: string; channel: UpdateChannel): UpdateBuild =
  UpdateBuild(order: n, revision: revision, channel: channel, version: "display")

let current = build(20, "current", ucTest)
let older = build(19, "old", ucStable)
let equal = build(20, "other", ucStable)
let same = build(21, "current", ucStable)
let stable = build(22, "stable", ucStable)
let preview = build(23, "next", ucTest)

suite "update policy":
  test "fresh settings default to test and download automatically":
    let s = defaultUpdateSettings()
    check s.channel == ucTest
    check s.autoDownload
    check checkSchedule(s, 1000) == csDue

  test "daily boundary and rollback rebase survive restart":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    let path = dir / "update.settings"
    var u = openUpdater(path, current)
    let request = u.onTimer(100000).get
    check request.kind == ujCheck
    check u.complete(request, none(UpdateBuild))
    u = openUpdater(path, current)
    check u.onTimer(186399).isNone
    check u.onTimer(186400).isSome
    u = openUpdater(path, current)
    check u.onTimer(110000).isNone
    u = openUpdater(path, current)
    check u.onTimer(196399).isNone
    check u.onTimer(196400).isSome

  test "stable excludes test; test may take a newer stable":
    check selectUpdate(current, ucStable, [stable, preview]).get == stable
    check selectUpdate(current, ucTest, [stable, preview]).get == preview
    check selectUpdate(current, ucTest, [stable]).get == stable

  test "older, equal and already installed source builds never replace current":
    check selectUpdate(current, ucStable, [older, equal, same]).isNone
    let ambiguous = build(22, "unexpected", ucStable)
    check selectUpdate(current, ucTest, [stable, ambiguous]).isNone

  test "corrupt or unwritable persistence does not silently schedule checks":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    writeFile(dir / "broken", "1\nucStable\ntrue\nNaN\n")
    expect ValueError:
      discard openUpdater(dir / "broken", current)
    var u = openUpdater(dir / "missing" / "update.settings", current)
    expect OSError:
      discard u.onTimer(100000)
    check u.snapshot.phase == upIdle

  test "channel changes discard in-flight results, retaining daily throttling":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    let path = dir / "update.settings"
    var u = openUpdater(path, current)
    let request = u.onTimer(100000).get
    check u.setChannel(ucStable)
    check u.snapshot.phase == upIdle
    check not u.complete(request, some(preview))
    check u.snapshot.phase == upIdle
    check u.onTimer(100001).isNone
    u = openUpdater(path, current)
    check u.snapshot.settings.channel == ucStable
    check u.onTimer(186400).get.channel == ucStable

  test "manual download retries; repeat restart cannot launch twice":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.setAutoDownload(false)
    let request = u.onTimer(100000).get
    check u.complete(request, some(stable))
    check u.snapshot.phase == upAvailable
    let download = u.requestDownload().get
    check u.requestDownload().isNone
    check u.complete(download, error = "interrupted")
    check u.snapshot.phase == upDownloadFailed
    let retry = u.requestDownload().get
    check not u.complete(download)
    check u.complete(retry)
    check u.snapshot.phase == upReady
    check not u.requestRestart(false)
    check u.requestRestart(true)
    check not u.requestRestart(true)
    u.restartFailed("helper not started")
    check u.snapshot.phase == upReady

  test "auto download uses the same single flight as manual download":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.complete(u.onTimer(100000).get, some(stable))
    let download = u.onTimer(100001).get
    check download.kind == ujDownload
    check u.onTimer(100002).isNone
    check u.setChannel(ucStable)
    check not u.complete(download)
    check u.snapshot.phase == upIdle

  test "newer source cannot lower the installed formal semantic version":
    let installed = UpdateBuild(order: 20, revision: "old", version: "v1.9.0", channel: ucStable)
    let downgrade = UpdateBuild(order: 22, revision: "new", version: "v1.8.99", channel: ucStable)
    let upgrade = UpdateBuild(order: 22, revision: "new", version: "v1.10.0", channel: ucStable)
    check selectUpdate(installed, ucStable, [downgrade]).isNone
    check selectUpdate(installed, ucStable, [upgrade]).get == upgrade
    var unknown = installed
    unknown.version = ""
    check selectUpdate(unknown, ucStable, [upgrade]).isNone
