"""Publish only verified, signed distribution ZIPs. No credentials beyond gh's existing token."""
import argparse
from pathlib import Path
import os
import json
import re
import shutil
import subprocess
import tempfile
from zipfile import ZipFile

from update_release import DEV_TAG, FIXED, NOTE, REPO, SHA, ZIP_LIMIT, EMBEDDED
from update_release import digest, inspect, record, require, verify_final
from update_rolling import BACKUP, STAGED, publish

PREFIX = f"repos/{REPO}"
OWNER = "<!-- shiguang-rolling-release/1 -->"
PENDING = "<!-- shiguang-pending-v2 "
RETIRED = "<!-- shiguang-retired-v2 "
LEGACY = "<!-- shiguang-ci-release "
RETIRED_LEGACY = "<!-- shiguang-retired-legacy "


def gh(*args, data=None, stream=None):
    p = subprocess.run(["gh", *args], input=data, stdin=None if data is not None else subprocess.DEVNULL,
                       stdout=stream if stream is not None else subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=90)
    if p.returncode:
        raise IOError(f"GitHub command failed ({args[0]}): {p.stderr.decode(errors='replace').strip()}")
    return p.stdout


def api(path, method="GET", fields=None):
    args = ["api", "-X", method, f"{PREFIX}/{path}"]
    raw = gh(*args, data=None) if fields is None else gh(
        *args, "--input", "-", data=json.dumps(fields).encode())
    return json.loads(raw) if raw else None


def tag_ref(tag):
    # Paginated matching-refs is a 200 with an empty list for an absent ref;
    # unlike ignoring a nonzero gh status it distinguishes outage from absence.
    refs = api(f"git/matching-refs/tags/{tag}")
    exact = [x["object"] for x in refs if x["ref"] == f"refs/tags/{tag}"]
    require(len(exact) <= 1, "ambiguous tag")
    return exact[0] if exact else None


def scan_releases():
    for page in range(1, 21):
        items = api(f"releases?per_page=100&page={page}")
        yield from items
        if len(items) < 100:
            return
    raise ValueError("release listing limit exceeded")


def unique(items, message):
    require(len(items) <= 1, message)
    return items[0] if items else None


def releases(tag):
    return unique([x for x in scan_releases() if x["tag_name"] == tag], "ambiguous release")


def select_development(items):
    candidates = [x for x in items if x["tag_name"].startswith("dev-")]
    selected = unique(candidates, "multiple development releases; specify an authorized migration target")
    if selected:
        tag = selected["tag_name"]
        require(selected["prerelease"] and (tag == DEV_TAG or re.fullmatch(r"dev-[0-9a-f]{12}", tag)),
                "not a recognized development release")
    return selected


def marker(body, prefix):
    items = [line for line in body.splitlines() if line.startswith(prefix)]
    require(len(items) <= 1, "duplicate signed release record")
    return items[0] if items else None


def download(asset, path):
    size = asset["size"]
    require(asset["state"] == "uploaded" and 0 < size <= ZIP_LIMIT and
            isinstance(asset["id"], int), "invalid GitHub asset size, state or id")
    with path.open("wb") as f:
        gh("api", f"{PREFIX}/releases/assets/{asset['id']}", "-H",
           "Accept: application/octet-stream", stream=f)
    require(path.stat().st_size == size, "downloaded asset size differs")
    actual = digest(path.read_bytes())
    require(asset.get("digest") in (None, "", "sha256:" + actual), "GitHub asset digest differs")
    return actual


def read_assets(rid):
    results = api(f"releases/{rid}/assets?per_page=8")
    require(len(results) < 8 and len({a["name"] for a in results}) == len(results),
            "unexpected release asset set")
    return {a["name"]: a for a in results}


def upload_asset(item, path):
    rid = item["id"]
    require(isinstance(rid, int) and rid > 0 and path.name in (FIXED, STAGED),
            "invalid asset upload target")
    # Use the known numeric Release ID, including when it is still a draft or
    # the legacy tag is about to change. Never discover it by download alias.
    url = f"https://uploads.github.com/{PREFIX}/releases/{rid}/assets"
    require(item["upload_url"] == url + "{?name,label}", "unexpected asset upload URL")
    gh("api", "-X", "POST", url + "?name=" + path.name,
       "-H", "Content-Type: application/octet-stream", "--input", str(path))


def payload_matches(path, raw):
    return {n: b for n, b in inspect(path, final=True).items() if n not in EMBEDDED} == inspect(raw)


def legacy_record(note, tag=None):
    require(note and note.startswith(LEGACY) and note.endswith(" -->"), "missing legacy machine record")
    obj = json.loads(note[len(LEGACY):-4])
    expected_keys = {"schema", "repo", "mode", "tag", "commit", "ref_object", "zip_sha256"}
    commit = obj.get("commit", "")
    require(set(obj) == expected_keys and obj["schema"] == 1 and obj["repo"] == REPO and
            obj["mode"] == "dev" and re.fullmatch(SHA, commit) and
            obj["tag"] == "dev-" + commit[:12] and (tag is None or obj["tag"] == tag) and
            obj["ref_object"] == commit and re.fullmatch(r"[0-9a-f]{64}", obj["zip_sha256"]),
            "legacy development machine record mismatch")
    ref = tag_ref(obj["tag"])
    require(ref and ref["type"] == "commit" and ref["sha"] == commit, "legacy tag identity changed")
    order = subprocess.check_output(["bash", str(Path(__file__).with_name("update_source_order.sh")),
                                     commit], timeout=30).decode().strip()
    require(re.fullmatch(r"[1-9][0-9]*", order), "legacy source is not on the update line")
    return dict(legacy=True, note=note, order=int(order), commit=commit, tag=obj["tag"],
                digest=obj["zip_sha256"])


def verify_legacy_asset(path, r):
    # The pre-updater package need not have an update assistant. Its complete
    # ZIP hash, machine record, immutable lightweight tag and embedded build
    # identity all have to match; its metadata never authorizes auto-install.
    with ZipFile(path) as z:
        entry = z.getinfo("BUILD-MANIFEST.json")
        require(0 < entry.file_size <= 65536, "legacy manifest size out of range")
        manifest = json.loads(z.read(entry))
    require(manifest["repo"] == REPO and manifest["commit"] == r["commit"] and
            manifest["ref"] == "refs/heads/main" and manifest["ref_object"] == r["commit"],
            "legacy asset build identity mismatch")


class Rolling:
    def __init__(self, public, work):
        self.public, self.work = public, work
        self.current = self.pending = self.retired = None
        self.assets, self.meta = {}, {}
        self.item = select_development(list(scan_releases()))
        ref = tag_ref(DEV_TAG)
        require(ref is None or ref["type"] == "commit", "rolling tag is not an owned lightweight tag")
        self.ref = ref["sha"] if ref else None
        if not self.item:
            require(ref is None, "will not adopt an existing unowned development tag")
            return
        require(self.item["prerelease"], "rolling release is not marked prerelease")
        body = self.item["body"] or ""
        self.legacy_body = None
        if self.item["tag_name"] != DEV_TAG:
            require(not self.item["draft"], "will not adopt a legacy draft")
            require(body.splitlines() and body.splitlines()[0].startswith(LEGACY),
                    "will not adopt an unknown legacy release")
            self.current = legacy_record(marker(body, LEGACY), self.item["tag_name"])
            self.legacy_body = "\n".join(line for line in body.splitlines() if not line.startswith(PENDING))
            pending = marker(body, PENDING)
            if pending:
                self.pending = record(public, NOTE + pending[len(PENDING):], DEV_TAG)
            require(self.ref is None or (self.pending and self.ref == self.pending["commit"]),
                    "will not adopt an unowned dev-latest tag")
        else:
            require(body.splitlines() and body.splitlines()[0] == OWNER, "will not adopt an unknown release")
            for attr, prefix in [("current", NOTE), ("pending", PENDING), ("retired", RETIRED)]:
                raw = marker(body, prefix)
                if raw:
                    setattr(self, attr, record(public, NOTE + raw[len(prefix):], DEV_TAG))
            legacy = marker(body, RETIRED_LEGACY)
            if legacy:
                require(self.retired is None and self.current and not self.pending,
                        "unexpected legacy retirement state")
                self.retired = legacy_record(LEGACY + legacy[len(RETIRED_LEGACY):])
        require(self.current or self.pending, "rolling release has no ownership record")
        require(not self.current or not self.item["draft"], "committed rolling release is draft")
        for name, asset in read_assets(self.item["id"]).items():
            require(name in (STAGED, BACKUP, FIXED), "unknown development asset")
            if (name == STAGED and self.pending and asset["state"] == "starter" and asset["size"] == 0):
                # GitHub may leave an empty upload placeholder on an interrupted upload.
                # Retry will remove it only after validating signed transaction ownership.
                self.meta[name] = asset
                continue
            self.meta[name] = asset
            self.assets[name] = download(asset, self.work / name)
            # Never authorize a state transition using merely advertised asset metadata.
            candidates = [p for p in (self.current, self.pending, self.retired) if p and
                          p["digest"] == self.assets[name]]
            require(candidates, "asset not bound to signed rolling state")
            item = candidates[0]
            if item.get("legacy"):
                verify_legacy_asset(self.work / name, item)
            else:
                verify_final(self.work / name, self.public, item["note"], DEV_TAG)

    def main_head(self):
        data = api("git/ref/heads/main")["object"]
        require(data["type"] == "commit" and re.fullmatch(SHA, data["sha"]), "invalid main ref readback")
        return data["sha"]

    def set_notes(self, current, pending, retired=None):
        require(current or pending, "refusing to clear all rolling release ownership")
        if current and current.get("legacy"):
            require(self.item and pending, "missing migration transaction")
            body = self.legacy_body + "\n" + PENDING + pending["note"][len(NOTE):]
            fields = {"body": body}
            expected_tag = current["tag"]
        else:
            lines = [OWNER]
            for r, prefix in [(current, NOTE), (pending, PENDING), (retired, RETIRED)]:
                if r:
                    if r.get("legacy"):
                        lines.append(RETIRED_LEGACY + r["note"][len(LEGACY):])
                    else:
                        lines.append(prefix + r["note"][len(NOTE):])
            shown = current or pending
            lines += ["", "拾光滚动开发版（Windows x64）", f"- 源提交：{shown['commit']}",
                      f"- 构建运行：{os.environ['RUN_URL']}", f"- 固定下载资产：{FIXED}"]
            fields = dict(body="\n".join(lines), name="拾光开发版", prerelease=True)
            expected_tag = DEV_TAG
            if not self.item:
                fields.update(tag_name=DEV_TAG, target_commitish=pending["commit"], draft=True)
            elif current:
                fields["draft"] = False
                if self.item["tag_name"] != DEV_TAG:
                    require(self.ref == current["commit"] and self.current.get("legacy"),
                            "cannot move release without verifying machine tag")
                    fields["tag_name"] = DEV_TAG
        previous_id = self.item["id"] if self.item else None
        if self.item:
            item = api(f"releases/{previous_id}", "PATCH", fields)
        else:
            item = api("releases", "POST", fields)
        require((previous_id is None or item["id"] == previous_id) and item["prerelease"] and
                item["tag_name"] == expected_tag and item["body"] == fields["body"] and
                (not current or not item["draft"]), "rolling release notes or ID did not read back")
        self.item = item
        self.current, self.pending, self.retired = current, pending, retired

    def upload(self, name, release):
        # Clear only a known blank GitHub upload placeholder for this signed pending name.
        if name in self.meta:
            require(self.meta[name]["state"] == "starter", "will not overwrite an existing asset")
            self.delete(name)
        path = self.work / name
        shutil.copyfile(release["path"], path)
        upload_asset(self.item, path)
        asset = read_assets(self.item["id"])[name]
        require(download(asset, self.work / "uploaded.zip") == release["digest"],
                "staged upload failed digest readback")
        self.meta[name], self.assets[name] = asset, release["digest"]

    def rename(self, old, new):
        require(new not in self.meta and old in self.assets, "unsafe rolling asset rename")
        a = self.meta[old]
        renamed = api(f"releases/assets/{a['id']}", "PATCH", {"name": new})
        require(renamed["id"] == a["id"] and renamed["name"] == new and
                renamed["size"] == a["size"], "renamed asset identity changed")
        self.meta.pop(old)
        self.meta[new], self.assets[new] = renamed, self.assets.pop(old)

    def delete(self, name):
        api(f"releases/assets/{self.meta[name]['id']}", "DELETE")
        self.meta.pop(name)
        self.assets.pop(name, None)

    def confirm_current(self):
        final = api(f"releases/{self.item['id']}")
        require(final["id"] == self.item["id"] and final["tag_name"] == DEV_TAG and
                final["prerelease"] and not final["draft"] and
                marker(final["body"], NOTE) == self.current["note"], "live rolling release readback failed")
        ref = tag_ref(DEV_TAG)
        require(ref and ref["type"] == "commit" and ref["sha"] == self.current["commit"],
                "live rolling tag readback failed")
        if self.retired and self.retired.get("legacy"):
            legacy = tag_ref(self.retired["tag"])
            require(legacy and legacy["type"] == "commit" and legacy["sha"] == self.retired["commit"],
                    "legacy Git tag was not preserved; keeping the backup")
        fixed = read_assets(final["id"])[FIXED]
        require(download(fixed, self.work / "committed.zip") == self.current["digest"],
                "live fixed asset readback failed; keeping backup")

    def move_ref(self, commit):
        live = tag_ref(DEV_TAG)
        require((live["sha"] if live else None) == self.ref and
                (live is None or live["type"] == "commit"), "rolling tag changed during publication")
        if live:
            api(f"git/refs/tags/{DEV_TAG}", "PATCH", {"sha": commit, "force": False})
        else:
            api("git/refs", "POST", {"ref": f"refs/tags/{DEV_TAG}", "sha": commit})
        readback = tag_ref(DEV_TAG)
        require(readback and readback["sha"] == commit and readback["type"] == "commit",
                "rolling tag did not read back")
        self.ref = commit


def verify_formal_ref(a):
    ref = tag_ref(a.version)
    require(ref and ref["sha"] == a.ref_object, "formal tag ref object changed")
    if ref["type"] == "tag":
        ref = api(f"git/tags/{ref['sha']}")["object"]
    require(ref["type"] == "commit" and ref["sha"] == a.commit, "formal tag does not point at the build")


def formal(a, new, work):
    verify_formal_ref(a)
    item = releases(a.version)
    uploaded = None
    if item:
        require(not item["prerelease"], "formal release has prerelease flag")
        notes = item["body"] or ""
        signed = marker(notes, NOTE)
        assets = read_assets(item["id"])
        if not signed:
            legacy = marker(notes, LEGACY)
            require(not item["draft"] and legacy and set(assets) == {FIXED},
                    "not an owned immutable historical release")
            expected = dict(schema=1, repo=REPO, mode="release", tag=a.version, commit=a.commit,
                            ref_object=a.ref_object, zip_sha256=digest(a.raw.read_bytes()))
            require(json.loads(legacy[len(LEGACY):-4]) == expected and
                    download(assets[FIXED], work / "historical.zip") == expected["zip_sha256"],
                    "historical release bytes or identity differ; no writes")
            print("historical immutable release verified (not automatically installable)")
            return
        prior = record(a.public, signed, a.version)
        ref_line = f"<!-- shiguang-formal-ref {a.ref_object} -->"
        require(prior["commit"] == a.commit and prior["order"] == new["order"] and
                ref_line in notes.splitlines() and set(assets) <= {FIXED},
                "existing formal release identity differs; no writes")
        if FIXED in assets and item["draft"] and assets[FIXED]["state"] == "starter" and assets[FIXED]["size"] == 0:
            api(f"releases/assets/{assets[FIXED]['id']}", "DELETE")
            del assets[FIXED]
        if FIXED in assets:
            uploaded = work / "existing.zip"
            require(download(assets[FIXED], uploaded) == prior["digest"], "immutable asset differs")
            verify_final(uploaded, a.public, signed, a.version)
            require(payload_matches(uploaded, a.raw), "immutable payload differs; no writes")
            if not item["draft"]:
                print("formal release verified (no writes)")
                return
        else:
            require(item["draft"], "published formal release is missing its asset")
    notes = ("<!-- shiguang-formal-ref " + a.ref_object + " -->\n" +
             new["note"] + f"\n\n拾光截图 {a.version}（Windows x64）\n- 源提交：{a.commit}\n" +
             f"- 构建运行：{os.environ['RUN_URL']}")
    if not item:
        item = api("releases", "POST", dict(tag_name=a.version, name=f"拾光截图 {a.version}",
                   body=notes, prerelease=False, draft=True, target_commitish=a.commit))
    if not uploaded:
        # A draft without an uploaded asset may be resealed (ECDSA is not byte deterministic).
        # Persist the exact replacement note before upload; no public publication is changed.
        current = api(f"releases/{item['id']}", "PATCH", {"body": notes})
        require(current["draft"] and current["body"] == notes, "draft notes did not read back")
        upload_asset(item, new["path"])
        assets = read_assets(item["id"])
        require(set(assets) == {FIXED} and download(assets[FIXED], work / "verify.zip") == new["digest"],
                "formal asset readback differs")
        verify_final(work / "verify.zip", a.public, new["note"], a.version)
    verify_formal_ref(a)
    final = api(f"releases/{item['id']}", "PATCH", {"draft": False})
    require(not final["draft"] and not final["prerelease"] and
            final["body"] == (item["body"] if uploaded else notes), "formal publication did not read back")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=("dev", "release"))
    p.add_argument("raw", type=Path)
    p.add_argument("final", type=Path)
    p.add_argument("note", type=Path)
    for name in ("commit", "version", "ref_object"):
        p.add_argument(name)
    a = p.parse_args()
    a.public = os.environ.get("SG_UPDATE_PUBLIC_KEY", "")
    require(os.environ["GH_REPO"] == REPO, "refusing to publish to a fork")
    note = a.note.read_text().strip()
    new = verify_final(a.final, a.public, note, DEV_TAG if a.mode == "dev" else a.version)
    new["path"] = a.final
    require(new["commit"] == a.commit and payload_matches(a.final, a.raw),
            "final and prechecked build input differ")
    with tempfile.TemporaryDirectory(prefix="sg-release-back-") as temp:
        work = Path(temp)
        if a.mode == "dev":
            r = Rolling(a.public, work)
            if r.current and r.current["commit"] == a.commit:
                require(new["order"] == r.current["order"], "same source has a different signed order")
                # Sealing and rebuilding can change incidental bytes. Keep the
                # already verified publication for this commit, without swapping it.
            publish(r, new)
        else:
            formal(a, new, work)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as e:
        raise SystemExit(f"update publication: {e}")
