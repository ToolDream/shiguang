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

proc cachedReady(path: string): Updater =
  ## A previous round that already downloaded and verified the cached candidate.
  result = openUpdater(path, current)
  doAssert result.setAutoDownload(false)
  doAssert result.complete(result.onTimer(100).get, some(stable))
  let download = result.requestDownload().get
  doAssert result.complete(download)
  doAssert result.snapshot.phase == upReady
  doAssert result.snapshot.candidate == some(stable)

suite "explicit check and install":
  test "manual check skips only the daily window and records the attempt":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    let path = dir / "update.settings"
    var u = openUpdater(path, current)
    check u.complete(u.onTimer(100000).get, none(UpdateBuild))
    check not u.snapshot.autoInstall
    check not u.snapshot.installReady
    let job = u.requestCheck(100500).job.get
    check job.kind == ujCheck
    check u.snapshot.phase == upChecking
    check u.snapshot.autoInstall            # cancellable while the round runs
    check not u.snapshot.installReady       # nothing verified yet
    check u.snapshot.settings.lastCheck == 100500
    check loadUpdateSettings(path).lastCheck == 100500
    u.checkPersistenceFailed("")
    check not u.snapshot.autoInstall
    u = openUpdater(path, current)
    check u.snapshot.settings.lastCheck == 100500
    check u.onTimer(100501).isNone            # the daily window is not restarted

  test "manual round pins the candidate and downloads once for one install":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    let path = dir / "update.settings"
    var u = openUpdater(path, current)
    check u.setAutoDownload(false)
    check u.complete(u.requestCheck(200000).job.get, some(stable))
    check u.snapshot.phase == upAvailable
    check u.snapshot.candidate == some(stable)
    check u.snapshot.autoInstall            # authorized for this round only
    check not u.snapshot.installReady       # no verified download yet
    check not u.snapshot.settings.autoDownload
    let download = u.onTimer(200001).get      # this round downloads despite the switch
    check download.kind == ujDownload
    check download.build == some(stable)
    check u.onTimer(200002).isNone
    check u.complete(download)
    check u.snapshot.phase == upReady
    check u.snapshot.installReady
    check not u.snapshot.settings.autoDownload    # the setting itself is unchanged
    check not u.requestRestart(true, some(preview))   # stage must match the candidate
    check u.requestRestart(true, some(stable))
    check u.snapshot.phase == upRestarting
    check not u.snapshot.installReady
    u = openUpdater(path, current)
    check not u.snapshot.autoInstall          # one-shot: never persisted

  test "an unconfirmed or lower identity never authorizes the cached candidate":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    for (incoming, name) in [(build(21, "mid", ucTest), "lower"),
                              (build(22, "other", ucTest), "same-order")]:
      var u = cachedReady(dir / ("unconfirmed-" & name))
      let job = u.requestCheck(101).job.get
      check u.complete(job, some(incoming))
      check u.snapshot.candidate == some(stable)     # the verified cache is kept
      check u.snapshot.phase == upReady
      check u.snapshot.result == rrUpToDate          # not confirmed this round
      check not u.snapshot.autoInstall
      check not u.snapshot.installReady
      check u.onTimer(102).isNone                    # never downloads the old cache
      check not u.requestRestart(false, some(stable))
      check u.requestRestart(true, some(stable))     # only the manual action remains

  test "a periodic round with nothing better keeps ready and never re-downloads":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = cachedReady(dir / "periodic")
    let job = u.onTimer(100 + CheckInterval).get
    check u.complete(job, some(build(21, "mid", ucTest)))
    check u.snapshot.phase == upReady              # never downgraded to available
    check u.snapshot.candidate == some(stable)
    check u.snapshot.result == rrUpToDate
    check not u.snapshot.autoInstall
    check u.onTimer(102).isNone                    # no duplicate transfer
    check u.requestRestart(true, some(stable))

  test "an identical identity confirmed again reuses the verified stage":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = cachedReady(dir / "same")
    check u.complete(u.requestCheck(101).job.get, some(stable))
    check u.snapshot.phase == upReady
    check u.snapshot.candidate == some(stable)
    check u.snapshot.result == rrCandidate
    check u.snapshot.installReady
    check u.onTimer(102).isNone
    check u.requestRestart(true, some(stable))

  test "a failed round, an empty round and a helper failure stay distinguishable":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    for name in ["offline", "empty"]:
      var u = cachedReady(dir / name)
      let job = u.requestCheck(101).job.get
      let error = if name == "offline": "network diagnostic" else: ""
      check u.complete(job, error = error)
      check u.snapshot.phase == upReady            # the verified stage is kept
      check not u.snapshot.installReady
      check u.snapshot.result == (if name == "offline": rrCheckFailed else: rrUpToDate)
      check (u.snapshot.error.len != 0) == (name == "offline")
      check u.requestRestart(true, some(stable))
    var u = cachedReady(dir / "install-failed")
    check u.complete(u.requestCheck(101).job.get, some(stable))
    check u.snapshot.installReady
    check u.requestRestart(true, some(stable))
    u.restartFailed("helper not started")
    check u.snapshot.phase == upReady
    check u.snapshot.result == rrInstallFailed
    check not u.snapshot.installReady
    check u.snapshot.error == "helper not started"
    check u.requestRestart(true, some(stable))     # manual retry stays available

  test "the verified stage waits for a visible cancel window and one attempt":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.setAutoDownload(false)
    check u.complete(u.requestCheck(1000).job.get, some(stable))
    check u.complete(u.onTimer(1001).get)
    check u.snapshot.installReady
    check not u.installDue(50)             # opens the visible window
    check not u.installDue(52)
    check u.installDue(53)                 # 3 seconds later it may start
    check u.installDue(90)                 # a busy app keeps the authorization
    check u.cancelAutoInstall()            # a cancel inside the wait wins
    check not u.installDue(91)
    check not u.snapshot.installReady
    check u.requestRestart(true, some(stable))
    var again = cachedReady(dir / "second")
    discard again.installDue(100)          # window opened by an older round
    check again.complete(again.requestCheck(101).job.get, some(stable))
    check not again.installDue(102)        # a new round starts a new window
    check again.installDue(105)
    check again.setChannel(ucStable)
    check not again.installDue(200)        # a channel switch clears the wait
    again = cachedReady(dir / "restart-failed")
    discard again.installDue(100)
    check again.requestRestart(true, some(stable))
    again.restartFailed("helper not started")
    check not again.installDue(200)        # a helper failure never retries itself

  test "cancel, channel switch and restart failure revoke the authorization":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    let path = dir / "update.settings"
    var u = openUpdater(path, current)
    check u.setAutoDownload(false)
    check u.complete(u.requestCheck(1000).job.get, some(stable))
    let download = u.onTimer(1001).get
    check u.complete(download)
    check u.snapshot.installReady
    check u.cancelAutoInstall()
    check not u.cancelAutoInstall()
    check not u.snapshot.autoInstall
    check u.snapshot.phase == upReady         # the verified stage is still ready
    check u.requestRestart(true)
    u.restartFailed("helper not started")
    check u.snapshot.phase == upReady
    check not u.snapshot.installReady
    var switched = cachedReady(dir / "switch")
    check switched.setChannel(ucStable)
    check not switched.snapshot.autoInstall
    check switched.snapshot.settings.channel == ucStable

  test "a background round never authorizes an automatic restart":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.complete(u.onTimer(100000).get, some(stable))
    check u.snapshot.phase == upAvailable
    check not u.snapshot.autoInstall
    check not u.snapshot.installReady
    let download = u.onTimer(100001).get
    check u.complete(download)
    check u.snapshot.phase == upReady
    check not u.snapshot.autoInstall
    check not u.installDue(1000)

  test "repeat clicks merge into the flight and run one queued check":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.setAutoDownload(false)
    let first = u.requestCheck(100000).job.get
    check u.requestCheck(100001).job.isNone       # no parallel job
    check u.snapshot.phase == upChecking
    check u.complete(first, some(stable))
    check u.snapshot.phase == upAvailable
    check u.snapshot.autoInstall
    let download = u.onTimer(100001).get
    check download.kind == ujDownload
    check u.requestCheck(100002).job.isNone       # only queues behind the transfer
    check u.snapshot.autoInstall
    check u.complete(download)
    check u.snapshot.phase == upReady
    let recheck = u.onTimer(100002).get
    check recheck.kind == ujCheck
    check u.snapshot.phase == upChecking
    check u.complete(recheck, some(stable))
    check u.snapshot.phase == upReady         # same candidate: the stage is reused
    check u.snapshot.installReady
    check u.onTimer(100003).isNone

  test "a manual click joins a running background check and still installs":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.setAutoDownload(false)
    let background = u.onTimer(100000).get        # the timer's own due check
    check u.snapshot.phase == upChecking
    check u.requestCheck(100001).job.isNone           # joins, never a parallel job
    check u.snapshot.phase == upChecking
    check u.snapshot.autoInstall
    check u.complete(background, some(stable))
    check u.snapshot.phase == upAvailable
    check u.snapshot.autoInstall                  # the click authorized this round
    let download = u.onTimer(100001).get
    check download.kind == ujDownload
    check u.complete(download)
    check u.snapshot.phase == upReady
    check u.snapshot.installReady

  test "a click after a channel switch waits for the discarded job":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    var u = openUpdater(dir / "update.settings", current)
    check u.setAutoDownload(false)
    let stale = u.onTimer(100000).get
    check u.setChannel(ucStable)
    check u.requestCheck(100001).job.isNone           # one flight only
    check not u.complete(stale, some(preview))    # discarded with its channel
    check u.snapshot.phase == upIdle
    let fresh = u.onTimer(100001).get
    check fresh.kind == ujCheck
    check fresh.channel == ucStable
    check u.complete(fresh, some(stable))
    check u.snapshot.phase == upAvailable
    check u.snapshot.autoInstall

  test "three relations of a check result to the cached identity":
    let dir = createTempDir("sg-update-policy-", "")
    defer: removeDir(dir)
    let newer = build(24, "brand-new", ucTest)
    for explicit in [true, false]:
      let tag = if explicit: "explicit-" else: "background-"
      # A) a genuinely higher identity is adopted from a ready stage: the phase and
      #    the old stage must not carry over, and nothing may restart before its own
      #    verified download.
      var ready = cachedReady(dir / (tag & "ready"))
      let request = if explicit: ready.requestCheck(200).job.get
                    else: ready.onTimer(100 + CheckInterval).get
      check ready.complete(request, some(newer))
      check ready.snapshot.candidate == some(newer)
      check ready.snapshot.phase == upAvailable        # never ready without its stage
      check ready.snapshot.result == rrCandidate
      check not ready.snapshot.installReady
      check ready.snapshot.autoInstall == explicit      # only a click authorizes
      check not ready.installDue(1000)                  # no restart yet
      if explicit:
        check ready.onTimer(201).isSome                 # this round skips the switch
      else:
        check ready.onTimer(201).isNone                 # background keeps the switch
        check ready.requestDownload().isSome            # manual entry still offered
      # A) the same replacement from a background download that had failed
      var failed = openUpdater(dir / (tag & "failed"), current)
      check failed.setAutoDownload(false)
      check failed.complete(failed.requestCheck(100).job.get, some(stable))
      check failed.complete(failed.onTimer(101).get, error = "network")
      check failed.snapshot.phase == upDownloadFailed
      let next = if explicit: failed.requestCheck(200).job.get
                 else: failed.onTimer(100 + CheckInterval).get
      check failed.complete(next, some(newer))
      check failed.snapshot.candidate == some(newer)
      check failed.snapshot.phase == upAvailable        # never the stale failure state
      check not failed.installDue(1000)
      # B) the identical identity is the only case that reuses the prior ready stage.
      var same = cachedReady(dir / (tag & "same"))
      let again = if explicit: same.requestCheck(200).job.get
                  else: same.onTimer(100 + CheckInterval).get
      check same.complete(again, some(stable))
      check same.snapshot.phase == upReady
      check same.snapshot.installReady == explicit
      # C) an unconfirmed result keeps the cached identity with manual access only.
      var cache = cachedReady(dir / (tag & "unconfirmed"))
      let stale = if explicit: cache.requestCheck(200).job.get
                  else: cache.onTimer(100 + CheckInterval).get
      check cache.complete(stale, some(build(22, "other", ucTest)))
      check cache.snapshot.candidate == some(stable)
      check cache.snapshot.phase == upReady
      check not cache.snapshot.autoInstall
