"""One rolling release, with a recoverable, non-atomic asset switch."""
from update_release import FIXED, require

STAGED = "shiguang-windows-x64-staged.zip"
BACKUP = "shiguang-windows-x64-previous.zip"


def match(asset, release):
    return asset == release["digest"]


def validate(r):
    current, pending = r.current, r.pending
    names = r.assets
    require(set(names) <= {FIXED, STAGED, BACKUP}, "unknown rolling release asset")
    legacy = bool(current and current.get("legacy"))
    allowed_refs = {x["commit"] for x in (current, pending) if x and not x.get("legacy")}
    require(r.ref in allowed_refs or (r.ref is None and (current is None or legacy)),
            "unknown rolling machine tag owner")
    if pending:
        require(not current or pending["order"] > current["order"] or
                (legacy and pending["commit"] == current["commit"] and pending["order"] == current["order"]),
                "out-of-order pending transaction")
    if STAGED in names:
        require(pending and match(names[STAGED], pending), "unknown staged asset")
    if BACKUP in names:
        require((current and match(names[BACKUP], current)) or
                (not pending and FIXED in names and r.retired and match(names[BACKUP], r.retired)),
                "unknown backup asset")
    if FIXED in names:
        require(any(match(names[FIXED], a) for a in (current, pending) if a),
                "unknown fixed asset")
    if current and FIXED not in names:
        require(pending and BACKUP in names, "missing current development asset")
    if not pending and current:
        require(FIXED in names and match(names[FIXED], current) and (r.ref is None if legacy else r.ref == current["commit"]),
                "published development release is inconsistent")


def commit(r):
    pending = r.pending
    require(FIXED in r.assets and match(r.assets[FIXED], pending), "new fixed asset is missing")
    # Persist the verified predecessor alongside the new stable notes until cleanup.
    r.set_notes(pending, None, r.current if BACKUP in r.assets else None)
    collect(r)


def collect(r):
    if r.retired:
        r.confirm_current()
        if BACKUP in r.assets:
            require(match(r.assets[BACKUP], r.retired), "backup differs from signed previous release")
            r.delete(BACKUP)
        r.set_notes(r.current, None)


def abort(r, replacement):
    """Restore the old fixed name before removing any candidate asset."""
    if FIXED in r.assets and match(r.assets[FIXED], r.pending):
        require(STAGED not in r.assets, "cannot safely undo candidate asset name")
        r.rename(FIXED, STAGED)
    if BACKUP in r.assets:
        r.rename(BACKUP, FIXED)
    if STAGED in r.assets:
        r.delete(STAGED)
    r.set_notes(r.current, replacement)


def publish(r, new):
    validate(r)
    pending = r.pending
    if r.current and new["commit"] == r.current["commit"] and not pending and not r.current.get("legacy"):
        if r.retired and new["commit"] == r.main_head(): collect(r)
        return  # Readback and payload equivalence were checked by the caller.
    require(new["commit"] == r.main_head(), "obsolete build is not current main")
    if pending and pending["commit"] != new["commit"]:
        if r.ref == pending["commit"]:
            # The ref is already advanced; finish this interrupted switch,
            # rather than moving it backwards. New builds still go forward.
            commit(r)
        else:
            abort(r, new)
        pending = r.pending
    collect(r)
    if r.current:
        require(new["order"] > r.current["order"] or
                (r.current.get("legacy") and new["commit"] == r.current["commit"] and
                 new["order"] == r.current["order"]), "obsolete development build sequence")
    if pending and not r.assets.get(STAGED) and r.assets.get(FIXED) != pending["digest"]:
        # Signed state existed before upload. No candidate bytes were visible,
        # so another attempt for the same identity can replace its signature.
        require(BACKUP not in r.assets, "cannot recover a missing staged asset")
        r.set_notes(r.current, new)
        pending = new
    elif not pending:
        r.set_notes(r.current, new)
        pending = new
    if STAGED not in r.assets and r.assets.get(FIXED) != pending["digest"]:
        r.upload(STAGED, new)
        require(match(r.assets[STAGED], pending), "staged upload differs from signed metadata")
    if r.assets.get(FIXED) != pending["digest"]:
        require(pending["commit"] == r.main_head(), "main changed before asset switch")
        if FIXED in r.assets:
            r.rename(FIXED, BACKUP)
        r.rename(STAGED, FIXED)
    if r.ref != pending["commit"]:
        require(pending["commit"] == r.main_head(), "main changed during asset switch")
        r.move_ref(pending["commit"])
    commit(r)
