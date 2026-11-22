import std/[options, os, strutils, tempfiles]

type
  UpdateChannel* = enum ucTest, ucStable
  UpdateBuild* = object
    order*: uint64             # Signed source order, shared by both channels.
    revision*, version*: string
    channel*: UpdateChannel
  UpdateSettings* = object
    channel*: UpdateChannel
    autoDownload*: bool
    lastCheck*: int64          # UTC Unix seconds, zero means never.
  CheckSchedule* = enum csWait, csDue, csRebase

const CheckInterval* = 24'i64 * 60 * 60

func defaultUpdateSettings*(): UpdateSettings =
  UpdateSettings(channel: ucTest, autoDownload: true)

func checkSchedule*(s: UpdateSettings; now: int64): CheckSchedule =
  if now <= 0: csWait
  elif s.lastCheck == 0: csDue
  elif now < s.lastCheck: csRebase
  elif now - s.lastCheck >= CheckInterval: csDue
  else: csWait

func stableVersion*(tag: string): Option[array[3, uint64]] =
  if tag.len < 6 or tag.len > 34 or tag[0] != 'v': return
  let parts = tag[1..^1].split('.')
  if parts.len != 3: return
  var version: array[3, uint64]
  for i, p in parts:
    if p.len == 0 or p.len > 10 or (p.len > 1 and p[0] == '0'): return
    for c in p:
      if c notin {'0'..'9'}: return
      version[i] = version[i] * 10 + uint64(ord(c) - ord('0'))
  some(version)

func stableUpdateAllowed(current, candidate: string): bool =
  let a = stableVersion(current)
  let b = stableVersion(candidate)
  if a.isNone or b.isNone: return false
  for i in 0..2:
    if a.get[i] != b.get[i]: return b.get[i] > a.get[i]
  false

func accepts*(installed: UpdateBuild; channel: UpdateChannel;
              candidate: UpdateBuild): bool =
  installed.order != 0 and installed.revision.len != 0 and
    candidate.order > installed.order and candidate.revision.len != 0 and
    candidate.revision != installed.revision and
    (channel == ucTest or candidate.channel == ucStable) and
    (installed.channel != ucStable or candidate.channel != ucStable or
      stableUpdateAllowed(installed.version, candidate.version))

func selectUpdate*(installed: UpdateBuild; channel: UpdateChannel;
                   builds: openArray[UpdateBuild]): Option[UpdateBuild] =
  var ambiguous = false
  for build in builds:
    if not accepts(installed, channel, build): continue
    if result.isNone or build.order > result.get.order:
      result = some(build)
      ambiguous = false
    elif build.order == result.get.order:
      if build.revision != result.get.revision or
          (build.channel == result.get.channel and build.version != result.get.version):
        ambiguous = true
      elif build.channel == ucStable:
        result = some(build)
  if ambiguous: result = none(UpdateBuild)

proc loadUpdateSettings*(path: string): UpdateSettings =
  if not fileExists(path): return defaultUpdateSettings()
  let f = open(path, fmRead)
  defer: f.close()
  var data = newString(97)
  data.setLen(f.readChars(data))
  if data.len > 96:
    raise newException(ValueError, "oversized update settings")
  let lines = data.split('\n')
  if lines.len != 5 or lines[0] != "1" or lines[4] != "" or
      lines[1] notin ["ucStable", "ucTest"] or lines[2] notin ["false", "true"] or
      lines[3].len == 0 or not lines[3].allCharsInSet({'0'..'9'}):
    raise newException(ValueError, "invalid update settings")
  let lastCheck = parseBiggestInt(lines[3])
  if lastCheck < 0: raise newException(ValueError, "invalid update check time")
  result = UpdateSettings(channel: parseEnum[UpdateChannel](lines[1]),
    autoDownload: lines[2] == "true", lastCheck: lastCheck)

proc saveUpdateSettings*(path: string; s: UpdateSettings) =
  let dir = path.parentDir
  let (f, tmp) = createTempFile(".sg-update-", ".tmp", if dir == "": "." else: dir)
  try:
    try:
      f.write("1\n" & $s.channel & "\n" & $s.autoDownload & "\n" & $s.lastCheck & "\n")
      f.flushFile()
    finally:
      f.close()
    moveFile(tmp, path)         # Same-directory replace; never unlink the old state first.
  finally:
    if fileExists(tmp): removeFile(tmp)
