## Timer/UI-thread coordination only; workers never mutate an Updater.
import std/options
import update_policy

type
  UpdatePhase* = enum
    upIdle, upChecking, upAvailable, upDownloading, upReady, upDownloadFailed,
    upRestarting, upCheckFailed
  UpdateJobKind* = enum ujCheck, ujDownload
  UpdateJob* = object
    kind*: UpdateJobKind
    channel*: UpdateChannel
    build*: Option[UpdateBuild]
    ticket, generation: uint64
  UpdateSnapshot* = object
    phase*: UpdatePhase
    installed*: UpdateBuild
    settings*: UpdateSettings
    candidate*: Option[UpdateBuild]
    error*: string
  Updater* = object
    settingsPath: string
    view: UpdateSnapshot
    flight: Option[UpdateJob]
    ticket, generation: uint64

proc openUpdater*(settingsPath: string; installed: UpdateBuild): Updater =
  Updater(settingsPath: settingsPath, view: UpdateSnapshot(
    installed: installed, settings: loadUpdateSettings(settingsPath)))

func snapshot*(u: Updater): UpdateSnapshot = u.view

proc persist(u: var Updater; settings: UpdateSettings) =
  saveUpdateSettings(u.settingsPath, settings)
  u.view.settings = settings

proc setChannel*(u: var Updater; channel: UpdateChannel): bool =
  if u.view.settings.channel == channel or u.view.phase == upRestarting: return false
  var s = u.view.settings
  s.channel = channel
  u.persist(s)
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
    ticket: u.ticket, generation: u.generation))
  u.flight

proc requestDownload*(u: var Updater): Option[UpdateJob] =
  if u.flight.isSome or u.view.phase notin {upAvailable, upDownloadFailed}: return
  u.view.phase = upDownloading
  u.view.error = ""
  u.start(ujDownload)

proc onTimer*(u: var Updater; now: int64): Option[UpdateJob] =
  if u.flight.isSome or u.view.phase == upRestarting: return
  var s = u.view.settings
  case checkSchedule(s, now)
  of csDue:
    s.lastCheck = now
    u.persist(s)
    if u.view.candidate.isNone: u.view.phase = upChecking
    return u.start(ujCheck)
  of csRebase:
    s.lastCheck = now
    u.persist(s)
  of csWait:
    discard
  if u.view.settings.autoDownload and u.view.phase == upAvailable:
    return u.requestDownload()

proc complete*(u: var Updater; job: UpdateJob;
               found = none(UpdateBuild); error = ""): bool =
  if u.flight != some(job): return false
  u.flight = none(UpdateJob)
  if job.generation != u.generation: return false
  case job.kind
  of ujCheck:
    if error.len != 0:
      if u.view.candidate.isNone:
        u.view.phase = upCheckFailed
        u.view.error = error
    elif found.isSome and accepts(u.view.installed, job.channel, found.get) and
        (u.view.candidate.isNone or (found.get.order > u.view.candidate.get.order and
                                     found.get.revision != u.view.candidate.get.revision)):
      u.view.candidate = found
      u.view.phase = upAvailable
      u.view.error = ""
    elif u.view.candidate.isNone:
      u.view.phase = upIdle
      u.view.error = ""
  of ujDownload:
    u.view.phase = if error.len == 0: upReady else: upDownloadFailed
    u.view.error = error
  true

proc checkPersistenceFailed*(u: var Updater; reason: string) =
  u.view.phase = upCheckFailed
  u.view.candidate = none(UpdateBuild)
  u.view.error = reason

proc requestRestart*(u: var Updater; safeToExit: bool): bool =
  if not safeToExit or u.flight.isSome or u.view.phase != upReady: return false
  u.view.phase = upRestarting
  u.view.error = ""
  true

proc restartFailed*(u: var Updater; error: string) =
  if u.view.phase == upRestarting:
    u.view.phase = upReady
    u.view.error = error
