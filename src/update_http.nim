when defined(windows):
  {.compile: "update_http_win.c".}
  {.passL: "-lwinhttp".}
  proc getBytes(url: cstring; buffer: pointer; capacity, ms: cint;
                resultSize: ptr cint): cint {.importc: "sg_update_get".}

  proc fetchUpdateHttps*(url: string; limit, milliseconds: int): string =
    result = newString(limit + 1)
    var size: cint
    let status = getBytes(url.cstring, addr result[0], cint(limit + 1),
                         cint(milliseconds), addr size)
    if status != 1 or size < 0 or size > limit:
      raise newException(IOError, "update HTTPS transfer failed or exceeded its limit")
    result.setLen(size)
