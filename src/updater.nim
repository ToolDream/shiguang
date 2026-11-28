## Timer/UI-thread coordination only; workers never mutate an Updater.
import std/options
import update_policy

type
  UpdatePhase* = enum
    upIdle, upChecking, upAvailable, upDownloading, upReady, upDownloadFailed,
    upRestarting, upCheckFailed
  RoundResult* = enum
    rrNone,                          # no round has finished yet
    rrChecking,                      # an explicit round is still running
    rrUpToDate,                      # the last round confirmed nothing installable
    rrCandidate,                     # the last round confirmed its candidate
    rrCheckFailed,                   # the last round's check did not complete
    rrDownloadFailed,                # the last round could not download or verify
    rrInstallFailed                  # the verified round could not start the helper
  UpdateJobKind* = enum ujCheck, ujDownload
  InstallIntent* = object
    ## One explicit "check and install" round. Memory only: it never survives a
    ## restart, a channel change, a failed check or a finished installation.
    channel*: UpdateChannel
    build*: Option[UpdateBuild]      # only a fresh confirmation pins an identity
  UpdateJob* = object
    kind*: UpdateJobKind
    channel*: UpdateChannel
    build*: Option[UpdateBuild]
    priorPhase*: UpdatePhase         # restored when a check confirms nothing new
    ticket, generation: uint64
  CheckRequest* = object
    ## The single decision source for an explicit "check and install" click:
    ## `accepted` also covers a click merged into a running check or queued behind a
    ## transfer, where no second worker may be created.
    accepted*: bool
    job*: Option[UpdateJob]
  UpdateSnapshot* = object
    phase*: UpdatePhase
    installed*: UpdateBuild
    settings*: UpdateSettings
    candidate*: Option[UpdateBuild]
    error*: string
    result*: RoundResult             # how the last round ended, independent of the cache
    autoInstall*: bool               # this round may still install: cancellable
    installReady*: bool              # its verified download waits for an idle moment
  Updater* = object
    settingsPath: string
    view: UpdateSnapshot
    flight: Option[UpdateJob]
    ticket, generation: uint64
    intent: Option[InstallIntent]
    queued: bool                     # explicit check waiting for the job in flight
    readyAt: Option[int64]           # monotonic second the verified stage became ready

const
  CancelWindowSeconds* = 3           # visible, cancellable hold before an automatic restart

proc openUpdater*(settingsPath: string; installed: UpdateBuild): Updater =
  Updater(settingsPath: settingsPath, view: UpdateSnapshot(
    installed: installed, settings: loadUpdateSettings(settingsPath)))

func snapshot*(u: Updater): UpdateSnapshot =
  result = u.view
  result.autoInstall = u.intent.isSome or u.queued
  # Only a ready phase backed by this round's own confirmed identity is installable.
  result.installReady = u.view.phase == upReady and u.intent.isSome and
    u.view.candidate.isSome and u.intent.get.build == u.view.candidate

proc persist(u: var Updater; settings: UpdateSettings) =
  saveUpdateSettings(u.settingsPath, settings)
  u.view.settings = settings

proc clearRound(u: var Updater) =
  u.intent = none(InstallIntent)
  u.readyAt = none(int64)

proc setChannel*(u: var Updater; channel: UpdateChannel): bool =
  if u.view.settings.channel == channel or u.view.phase == upRestarting: return false
  var s = u.view.settings
  s.channel = channel
  u.persist(s)
  u.clearRound()                     # a new channel never inherits an install round
  u.queued = false
  u.view.result = rrNone
  inc u.generation
  if u.view.phase notin {upAvailable, upReady, upDownloadFailed} or
      u.view.candidate.isNone or
      not accepts(u.view.installed, channel, u.view.candidate.get):
    u.view.candidate = none(UpdateBuild)
    u.view.phase = upIdle
    u.view.error = ""
  true

proc setAutoDownload*(u: var Updater; enabled: bool): bool =
  if u.view.settings.autoDownload == enabled or u.view.phase == upRestarting: return false
  var s = u.view.settings
  s.autoDownload = enabled
  u.persist(s)
  true

proc start(u: var Updater; kind: UpdateJobKind): Option[UpdateJob] =
  inc u.ticket
  u.flight = some(UpdateJob(kind: kind, channel: u.view.settings.channel,
    build: (if kind == ujDownload: u.view.candidate else: none(UpdateBuild)),
    priorPhase: u.view.phase, ticket: u.ticket, generation: u.generation))
  u.flight

proc requestDownload*(u: var Updater): Option[UpdateJob] =
  if u.flight.isSome or u.view.phase notin {upAvailable, upDownloadFailed}: return
  u.view.phase = upDownloading
  u.view.error = ""
  u.readyAt = none(int64)
  u.start(ujDownload)

proc requestCheck*(u: var Updater; now: int64): CheckRequest =
  ## "Check and install": skips only the daily window. A click that joins a running
  ## check or queues behind a transfer is accepted without a second worker, and the
  ## caller must not report it as a failure.
  if u.view.phase == upRestarting: return
  let flight = u.flight
  if flight.isSome:
    # A click joins a check of the same round and carries the install intent into
    # its result; behind a transfer (or a discarded job) only one follow-up check
    # is queued, so the verified stage is re-confirmed before anything installs.
    result.accepted = true
    if flight.get.generation != u.generation or flight.get.kind == ujDownload:
      u.queued = true
      return
    if u.intent.isNone: u.intent = some(InstallIntent(channel: flight.get.channel))
    u.view.result = rrChecking
    u.readyAt = none(int64)
    u.view.phase = upChecking
    u.view.error = ""
    return
  var s = u.view.settings
  s.lastCheck = now                   # raises before any job or intent exists
  u.persist(s)
  u.queued = false
  u.intent = some(InstallIntent(channel: s.channel))
  result.job = u.start(ujCheck)
  result.accepted = result.job.isSome
  u.view.result = rrChecking
  u.readyAt = none(int64)
  u.view.phase = upChecking
  u.view.error = ""

proc cancelAutoInstall*(u: var Updater): bool =
  ## Cancels the one-shot authorization. A transfer already running is not
  ## interrupted and its verified stage stays available for a manual restart.
  result = u.intent.isSome or u.queued
  u.clearRound()
  u.queued = false

proc onTimer*(u: var Updater; now: int64): Option[UpdateJob] =
  if u.flight.isSome or u.view.phase == upRestarting: return
  if u.queued: return u.requestCheck(now).job
  var s = u.view.settings
  case checkSchedule(s, now)
  of csDue:
    s.lastCheck = now
    u.persist(s)
    result = u.start(ujCheck)
    if u.view.candidate.isNone: u.view.phase = upChecking
    return
  of csRebase:
    s.lastCheck = now
    u.persist(s)
  of csWait:
    discard
  if u.view.phase == upAvailable and
      (u.view.settings.autoDownload or u.intent.isSome):
    return u.requestDownload()

proc pin(u: var Updater; channel: UpdateChannel; fresh: UpdateBuild) =
  ## Only an explicit round pins a confirmed identity; a background round never
  ## authorizes an install.
  if u.intent.isSome:
    u.intent = some(InstallIntent(channel: channel, build: some(fresh)))

proc complete*(u: var Updater; job: UpdateJob;
               found = none(UpdateBuild); error = ""): bool =
  if u.flight != some(job): return false
  u.flight = none(UpdateJob)
  u.readyAt = none(int64)
  if job.generation != u.generation: return false
  case job.kind
  of ujCheck:
    let resume = if job.priorPhase in {upAvailable, upReady, upDownloadFailed}:
        job.priorPhase else: upIdle
    if error.len != 0:
      u.intent = none(InstallIntent)          # an incomplete round never authorizes
      u.view.result = rrCheckFailed
      u.view.error = error
      if u.view.candidate.isNone: u.view.phase = upCheckFailed
      else: u.view.phase = resume             # the old verified stage stays manual
    elif found.isSome and accepts(u.view.installed, job.channel, found.get):
      # Decide the relation between this round's result and the cached identity
      # first, then set candidate, phase and intent once:
      #   A) a genuinely higher identity is adopted  -> upAvailable (its own stage
      #      does not exist yet, so a prior ready/failed stage must not be reused);
      #   B) the identical identity is confirmed     -> only here a prior ready stage
      #      (or the prior download failure) may be kept;
      #   C) anything else                           -> keep the cache, no authorization.
      let fresh = found.get
      let cached = u.view.candidate
      let adopted = cached.isNone or
        (fresh.order > cached.get.order and fresh.revision != cached.get.revision)
      u.view.error = ""
      if adopted:
        u.view.candidate = some(fresh)
        u.view.result = rrCandidate
        u.pin(job.channel, fresh)
        u.view.phase = upAvailable                       # never ready without its stage
      elif cached.get == fresh:
        u.view.candidate = some(fresh)
        u.view.result = rrCandidate
        u.pin(job.channel, fresh)
        u.view.phase =
          if resume == upReady: upReady
          elif resume == upDownloadFailed and u.intent.isNone: upDownloadFailed
          else: upAvailable
      else:
        u.intent = none(InstallIntent)
        u.view.result = rrUpToDate
        u.view.phase = resume
    else:
      u.intent = none(InstallIntent)          # nothing newer this round
      u.view.result = rrUpToDate
      u.view.error = ""
      u.view.phase = if u.view.candidate.isSome: resume else: upIdle
  of ujDownload:
    u.view.phase = if error.len == 0: upReady else: upDownloadFailed
    u.view.error = error
    if error.len == 0:
      u.view.result = rrCandidate
    else:
      u.intent = none(InstallIntent)
      u.view.result = rrDownloadFailed
  true

proc checkPersistenceFailed*(u: var Updater; reason: string) =
  u.clearRound()
  u.queued = false
  u.view.result = rrCheckFailed
  u.view.phase = upCheckFailed
  u.view.candidate = none(UpdateBuild)
  u.view.error = reason

proc installDue*(u: var Updater; monotonicSeconds: int64): bool =
  ## Called once per timer turn. The first observation of an authorized, verified
  ## stage opens a visible cancel window; the restart may only start after it.
  if u.view.phase != upReady or u.flight.isSome or u.view.candidate.isNone or
      u.intent.isNone or u.intent.get.build != u.view.candidate:
    u.readyAt = none(int64)
    return false
  if u.readyAt.isNone: u.readyAt = some(monotonicSeconds)
  monotonicSeconds >= u.readyAt.get + CancelWindowSeconds

proc requestRestart*(u: var Updater; safeToExit: bool;
                     staged = none(UpdateBuild)): bool =
  if not safeToExit or u.flight.isSome or u.view.phase != upReady: return false
  if u.view.candidate.isNone: return false
  let candidate = u.view.candidate.get
  if staged.isSome and staged.get != candidate: return false
  if u.intent.isSome:
    let intent = u.intent.get
    if intent.build != some(candidate) or intent.channel != u.view.settings.channel:
      # Authorization no longer matches this stage; keep the manual retry only.
      u.clearRound()
      return false
  u.clearRound()
  u.queued = false
  u.view.phase = upRestarting
  u.view.error = ""
  true

proc restartFailed*(u: var Updater; error: string) =
  if u.view.phase == upRestarting:
    u.clearRound()                     # never retry the helper once per tick
    u.view.result = rrInstallFailed
    u.view.phase = upReady
    u.view.error = error
