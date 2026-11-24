import std/[base64, json, monotimes, options, os, strutils, tempfiles, times]
import update_install, update_package, update_policy, updater

type
  UpdateFetch* = proc(url: string; limit, milliseconds: int): string {.nimcall, gcsafe.}
  Release = object
    manifest: VerifiedManifest
    body, signature: string
  InventoryProofPair = tuple[body, signature: string]
  StagedUpdate* = object
    directory*: string
    manifestBody*, signature*, priorBody*, priorSignature*: string
    build*: UpdateBuild
  WorkerInput = object
    job: UpdateJob
    installed: UpdateBuild
    release: Release
    prior: InventoryProofPair
    key, root, installedDir: string
    fetch: UpdateFetch
  WorkerResult = object
    job: UpdateJob
    release: Option[Release]
    prior: InventoryProofPair
    stage: Option[StagedUpdate]
    error: string
  WorkerMailbox = object
    input: Channel[WorkerInput]
    output: Channel[WorkerResult]
  UpdateService* = ref object
    policy: Updater
    key, root, installedDir: string
    fetch: UpdateFetch
    release: Option[Release]
    prior: InventoryProofPair
    staged: Option[StagedUpdate]
    mailbox: ptr WorkerMailbox
    worker: Thread[ptr WorkerMailbox]
    running, closed: bool
    retryAt: int64

const
  Api = "https://api.github.com/repos/" & UpdateRepo & "/releases"
  Marker = "<!-- shiguang-update-v2 "
  BudgetMs = 30000
  ListingLimit = 2 * 1024 * 1024
  # 单一分页常量：请求 URL、列表校验上限和截断判断必须始终一致。
  # 生成的 URL 必须与 update_http_win.c 白名单里的字面量逐字相同。
  ListingPage = 30

proc checkedGet(fetch: UpdateFetch; url: string; limit: int; deadline: MonoTime): string =
  let remaining = (deadline - getMonoTime()).inMilliseconds
  if remaining <= 0: raise newException(IOError, "update network deadline")
  result = fetch(url, limit, int(remaining))
  if result.len > limit or getMonoTime() > deadline:
    raise newException(IOError, "update response exceeded its time or size limit")

proc signedRelease(key: string; entry: JsonNode): Option[Release] =
  if entry.kind != JObject or entry{"draft"}.getBool(true): return
  let tag = entry{"tag_name"}.getStr()
  let body = entry{"body"}.getStr()
  let assets = entry{"assets"}
  if assets.isNil or assets.kind != JArray: return
  var present = false
  for asset in assets:
    present = present or asset{"name"}.getStr() == UpdateZip
  if not present: return
  for line in body.splitLines():
    if not line.startsWith(Marker): continue
    if result.isSome or not line.endsWith(" -->") or line.len > MaxManifestBytes * 2:
      return none(Release)
    let fields = line[Marker.len ..< line.len - 4].split(' ')
    if fields.len != 2: return none(Release)
    try:
      let body = decode(fields[0])
      if encode(body) != fields[0]: return none(Release)
      let manifest = verifyManifest(key, body, fields[1])
      if not manifest.hasInventory or manifest.releaseTag != tag or
          (manifest.build.channel == ucTest) != entry{"prerelease"}.getBool():
        return none(Release)
      result = some(Release(manifest: manifest, body: body, signature: fields[1]))
    except ValueError:
      return none(Release)

proc discover(input: WorkerInput; deadline: MonoTime): Option[Release] =
  let listing = parseJson(checkedGet(input.fetch, Api & "?per_page=" & $ListingPage,
    ListingLimit, deadline))
  if listing.kind != JArray or listing.len > ListingPage:
    raise newException(IOError, "invalid GitHub release listing")
  # GitHub returns the complete listing whenever it is shorter than the requested page,
  # so extra queries there cannot add a candidate and must not be repeated.
  let maybeTruncated = listing.len >= ListingPage
  var releases: seq[Release]
  var builds: seq[UpdateBuild]
  var hasStable, hasTest = false
  for entry in listing:
    let release = signedRelease(input.key, entry)
    if release.isSome:
      releases.add(release.get)
      builds.add(release.get.manifest.build)
      hasStable = hasStable or release.get.manifest.build.channel == ucStable
      hasTest = hasTest or release.get.manifest.build.channel == ucTest
  if maybeTruncated and not hasStable:
    let listing = checkedGet(input.fetch, Api & "/latest", ListingLimit, deadline)
    if listing != "":          # WinHTTP uses empty body for API 404 (no formal release).
      let release = signedRelease(input.key, parseJson(listing))
      if release.isSome:
        releases.add(release.get)
        builds.add(release.get.manifest.build)
  if maybeTruncated and input.job.channel == ucTest and not hasTest:
    let item = checkedGet(input.fetch, Api & "/tags/" & DevTag, ListingLimit, deadline)
    if item != "":
      let release = signedRelease(input.key, parseJson(item))
      if release.isSome and release.get.manifest.build.channel == ucTest:
        releases.add(release.get)
        builds.add(release.get.manifest.build)
  let choice = selectUpdate(input.installed, input.job.channel, builds)
  if choice.isSome:
    for release in releases:
      if release.manifest.build == choice.get:
        if result.isSome and result.get.body != release.body:
          raise newException(ValueError, "conflicting signed payloads for update build")
        result = some(release)

proc workerTurn(m: ptr WorkerMailbox) {.thread.} =
  let input = m.input.recv()
  var outcome = WorkerResult(job: input.job)
  try:
    let deadline = getMonoTime() + initDuration(milliseconds = BudgetMs)
    case input.job.kind
    of ujCheck:
      if input.key.len != 128 or input.installed.order == 0:
        raise newException(ValueError, "update trust is not configured")
      outcome.release = discover(input, deadline)
      if outcome.release.isSome:
        let body = readBounded(input.installedDir / InventoryName, MaxManifestBytes)
        let proof = readBounded(input.installedDir / InventoryProof, 128)
        discard installedInventory(input.key, body, proof, input.installed)
        outcome.prior = (body, proof)
    of ujDownload:
      let release = input.release
      let manifest = release.manifest
      let bytes = checkedGet(input.fetch, manifest.packageUrl, manifest.packageSize, deadline)
      let dir = createTempDir("sg-update-", "", input.root)
      try:
        let stage = dir / "payload"
        stagePackage(manifest, bytes, stage, input.key)
        outcome.stage = some(StagedUpdate(directory: stage, build: manifest.build,
          manifestBody: release.body, signature: release.signature,
          priorBody: input.prior.body, priorSignature: input.prior.signature))
      except CatchableError:
        removeDir(dir)
        raise
  except CatchableError as e:
    outcome.error = e.msg
  m.output.send(move outcome)

proc newUpdateService*(root: string; installed: UpdateBuild; key: string;
                       fetch: UpdateFetch; installedDir = ""): UpdateService =
  result = UpdateService(root: root, key: key, fetch: fetch, installedDir: installedDir,
    policy: openUpdater(root / "update.settings", installed))

func snapshot*(s: UpdateService): UpdateSnapshot = s.policy.snapshot

proc dropStage(s: UpdateService) =
  if s.staged.isSome:
    let path = s.staged.get.directory.parentDir
    s.staged = none(StagedUpdate)
    try: removeDir(path)
    except OSError: discard   # Unused private stage; never delete any install files here.

proc finish(s: UpdateService; r: WorkerResult) =
  let before = s.snapshot.candidate
  let build = if r.release.isSome: some(r.release.get.manifest.build) else: none(UpdateBuild)
  if s.policy.complete(r.job, build, r.error):
    case r.job.kind
    of ujCheck:
      if before != s.snapshot.candidate:
        s.dropStage()
        s.release = r.release
        s.prior = r.prior
    of ujDownload:
      if r.stage.isSome:
        s.dropStage()
        s.staged = r.stage
      elif r.error.len == 0:
        raise newException(ValueError, "updater returned no staged files")
  elif r.stage.isSome:
    try: removeDir(r.stage.get.directory.parentDir)
    except OSError: discard

proc freeMailbox(s: UpdateService) =
  s.mailbox.input.close()
  s.mailbox.output.close()
  deallocShared(s.mailbox)
  s.mailbox = nil
  s.running = false

proc collect(s: UpdateService) =
  if not s.running: return
  let r = s.mailbox.output.tryRecv()
  if not r.dataAvailable: return
  joinThread(s.worker)         # Only thread teardown after it sent the final result.
  s.freeMailbox()
  s.finish(r.msg)

proc launch(s: UpdateService; job: UpdateJob) =
  # Nim 2.2 close() frees a queue without zeroing its internal data pointer.
  # Never reopen the same channel, and allocate its storage on the shared heap.
  s.mailbox = cast[ptr WorkerMailbox](allocShared0(sizeof(WorkerMailbox)))
  s.mailbox.input.open(1)
  s.mailbox.output.open(1)
  let input = WorkerInput(job: job, installed: s.snapshot.installed,
    key: s.key, root: s.root, fetch: s.fetch, installedDir: s.installedDir,
    release: (if s.release.isSome: s.release.get else: default(Release)),
    prior: s.prior)
  try:
    createThread(s.worker, workerTurn, s.mailbox)
  except CatchableError as e:
    s.freeMailbox()
    discard s.policy.complete(job, error = e.msg)
    return
  s.running = true
  s.mailbox.input.send(input)

proc tick*(s: UpdateService; now, monotonicSeconds: int64) =
  if s.closed: return
  s.collect()
  if monotonicSeconds < s.retryAt: return
  try:
    let job = s.policy.onTimer(now)
    if job.isSome: s.launch(job.get)
  except CatchableError as error:
    s.retryAt = monotonicSeconds + 60
    s.policy.checkPersistenceFailed(error.msg)
    s.dropStage()
    s.release = none(Release)
    s.prior = default(InventoryProofPair)

proc tick*(s: UpdateService; now: int64) =
  s.tick(now, getMonoTime().ticks div 1_000_000_000)

proc setChannel*(s: UpdateService; channel: UpdateChannel): bool =
  if s.closed: return false
  result = s.policy.setChannel(channel)
  if result and s.snapshot.candidate.isNone:
    s.dropStage()
    s.release = none(Release)

proc setAutoDownload*(s: UpdateService; enabled: bool): bool =
  not s.closed and s.policy.setAutoDownload(enabled)

proc download*(s: UpdateService): bool =
  if s.closed: return false
  let job = s.policy.requestDownload()
  if job.isNone: return false
  s.launch(job.get)
  true

proc beginRestart*(s: UpdateService; safeToExit: bool): Option[StagedUpdate] =
  if not s.closed and s.staged.isSome and s.policy.requestRestart(safeToExit):
    return s.staged

proc restartFailed*(s: UpdateService; error: string) =
  s.policy.restartFailed(error)

proc close*(s: UpdateService) =
  if s.closed: return
  s.closed = true
  if s.running:
    joinThread(s.worker)       # Shutdown only, bounded by transfer deadline.
    let outcome = s.mailbox.output.recv()
    s.freeMailbox()
    s.finish(outcome)
  if s.snapshot.phase != upRestarting: s.dropStage()

proc closeIfIdle*(s: UpdateService) =
  ## Window shutdown must not wait for a transfer in progress.
  if not s.running: s.close()
