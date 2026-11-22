"""Build the stored ZIP variant and sign the updater's exact metadata (CI only)."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
from zipfile import ZipFile, ZipInfo, ZIP_STORED

REPO = "ToolDream/shiguang"
FILES = {"shiguang.exe", "shiguang-updater.exe", "BUILD-MANIFEST.json", "SHA256SUMS.txt"}
MANIFEST = "BUILD-MANIFEST.json"
INVENTORY = "UPDATE-INVENTORY.txt"
PROOF = "UPDATE-INVENTORY.sig"
EMBEDDED = {INVENTORY, PROOF}
FIXED = "shiguang-windows-x64.zip"
DEV_TAG = "dev-latest"
NOTE = "<!-- shiguang-update-v2 "
SHA = r"[0-9a-f]{40}"
SEMVER = r"v(?:0|[1-9][0-9]{0,9})(?:\.(?:0|[1-9][0-9]{0,9})){2}"
ZIP_LIMIT = 64 * 1024 * 1024
SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d03010703420004")


def require(valid, detail):
    if not valid:
        raise ValueError(detail)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def managed(name):
    return name in FILES or (
        re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9._-]{0,95}\.dll", name) is not None
        and not re.fullmatch(r"con|prn|aux|nul|com[0-9]|lpt[0-9]", name.split(".")[0].lower())
    )


def verify_entries(names, final=False):
    require(len(names) <= 64 and len(set(n.lower() for n in names)) == len(names),
            "duplicate or excessive update files")
    required = FILES | (EMBEDDED if final else set())
    require(required.issubset(names) and all(managed(n) or (final and n in EMBEDDED) for n in names),
            "invalid update file list")


def write_zip(payload, target, final=False):
    verify_entries(payload, final)
    require(sum(map(len, payload.values())) < ZIP_LIMIT, "update exceeds size limit")
    with ZipFile(target, "x", compression=ZIP_STORED, allowZip64=False) as out:
        for name, data in sorted(payload.items()):
            entry = ZipInfo(name)
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | 0o644) << 16
            out.writestr(entry, data)
    require(0 < target.stat().st_size <= ZIP_LIMIT, "update ZIP exceeds size limit")


def pack(source, target):
    paths = list(source.iterdir())
    verify_entries([p.name for p in paths])
    require(all(p.is_file() and not p.is_symlink() for p in paths), "not a regular package file")
    require(sum(p.stat().st_size for p in paths) < ZIP_LIMIT, "update exceeds size limit")
    write_zip({p.name: p.read_bytes() for p in paths}, target)


def inspect(path, final=False):
    require(0 < path.stat().st_size <= ZIP_LIMIT, "update ZIP exceeds size limit")
    with ZipFile(path) as archive:
        files = archive.infolist()
        verify_entries([f.filename for f in files], final)
        require(not archive.comment and all(
            not f.extra and not f.comment and not (f.flag_bits & ~0x800)
            and f.compress_type == ZIP_STORED and 0 < f.file_size <= ZIP_LIMIT
            and stat.S_IFMT(f.external_attr >> 16) in (0, stat.S_IFREG)
            and not (f.external_attr & 0x418)
            for f in files
        ), "invalid signed ZIP format")
        require(sum(f.file_size for f in files) <= ZIP_LIMIT, "update contents exceed limit")
        payload = {f.filename: archive.read(f) for f in files}
    return payload


def signature_rs(der):
    require(len(der) >= 8 and der[0] == 0x30 and der[1] == len(der) - 2, "invalid P-256 signature")
    p = 2
    values = []
    for _ in range(2):
        require(p + 2 <= len(der) and der[p] == 2 and 1 <= der[p + 1] <= 33,
                "invalid P-256 signature integer")
        n = der[p + 1]
        require(p + 2 + n <= len(der), "invalid P-256 signature length")
        values.append(int.from_bytes(der[p + 2:p + 2 + n], "big").to_bytes(32, "big"))
        p += 2 + n
    require(p == len(der), "invalid P-256 signature trailer")
    return b"".join(values).hex()


def openssl(*args, data=None):
    proc = subprocess.run(["openssl", *args], input=data,
                          stdin=None if data is not None else subprocess.DEVNULL,
                          capture_output=True, timeout=10)
    require(proc.returncode == 0, "OpenSSL rejected the update signing input")
    return proc.stdout


def der_signature(raw):
    def number(v):
        v = v.lstrip(bytes(1)) or bytes(1)
        if v[0] & 128:
            v = bytes(1) + v
        return bytes((2, len(v))) + v
    body = number(raw[:32]) + number(raw[32:])
    return bytes((48, len(body))) + body


def check_signature(public, body, signature):
    require(re.fullmatch(r"[a-f0-9]{128}", public or "") and
            re.fullmatch(r"[a-f0-9]{128}", signature) and 0 < len(body) <= 16384,
            "bad signature format or missing pinned public key")
    with tempfile.TemporaryDirectory(prefix="sg-verify-") as work:
        pub = Path(work) / "public.der"
        proof = Path(work) / "proof.der"
        pub.write_bytes(SPKI_PREFIX + bytes.fromhex(public))
        proof.write_bytes(der_signature(bytes.fromhex(signature)))
        openssl("dgst", "-sha256", "-verify", str(pub), "-keyform", "DER",
                "-signature", str(proof), data=body)


def body_bytes(lines):
    data = ("\n".join(lines) + "\n").encode("ascii")
    require(len(data) <= 16384, "update metadata exceeds limit")
    return data


def entries(payload):
    return [f"{n}|{len(b)}|{digest(b)}" for n, b in sorted(payload.items())]


def record(public, note, expected_tag=None):
    require(note.startswith(NOTE) and note.endswith(" -->"), "not a v2 update record")
    encoded, signature = note[len(NOTE):-4].split(" ")
    require(len(encoded) <= 21848, "oversized update record")
    data = base64.b64decode(encoded, validate=True)
    require(base64.b64encode(data).decode() == encoded, "noncanonical note")
    check_signature(public, data, signature)
    domain, repo, target, channel, version, order, commit, size, sha, *files = data.decode("ascii").splitlines()
    require(domain == "shiguang-update/2" and repo == REPO and target == "windows-x64" and
            re.fullmatch(SHA, commit) and re.fullmatch(r"[1-9][0-9]{0,19}", order) and int(order) < 2**64 and
            re.fullmatch(r"[1-9][0-9]{0,7}", size) and int(size) <= ZIP_LIMIT and
            re.fullmatch(r"[a-f0-9]{64}", sha), "invalid signed build identity")
    if channel == "test":
        require(version == "dev-" + commit[:12], "invalid development build version")
        tag = DEV_TAG
    else:
        require(channel == "stable" and re.fullmatch(SEMVER, version), "invalid formal release version")
        tag = version
    require(expected_tag is None or tag == expected_tag, "signed release tag mismatch")
    return dict(commit=commit, order=int(order), digest=sha, size=int(size), note=note,
                tag=tag, identity=[repo, target, channel, version, order, commit], files=files)


def verify_final(path, public, note, expected_tag=None):
    r = record(public, note, expected_tag)
    data = path.read_bytes()
    require(len(data) == r["size"] and digest(data) == r["digest"], "final ZIP disagrees with signed record")
    payload = inspect(path, final=True)
    require(entries(payload) == r["files"], "final ZIP inventory disagrees with signed record")
    inventory = payload.pop(INVENTORY)
    proof = payload.pop(PROOF).decode("ascii")
    require(inventory == body_bytes(["shiguang-inventory/1", *r["identity"], *entries(payload)]),
            "embedded signed inventory disagrees with ZIP")
    check_signature(public, inventory, proof)
    return r


def seal(a):
    key = os.environ.get("SG_UPDATE_SIGNING_KEY", "")
    public = os.environ.get("SG_UPDATE_PUBLIC_KEY", "")
    require(key and re.fullmatch(r"[a-f0-9]{128}", public), "update signing key or pinned public key is not configured")
    require(a.repo == REPO and re.fullmatch(SHA, a.commit) and
            re.fullmatch(r"[1-9][0-9]{0,19}", a.order) and int(a.order) < 2**64,
            "invalid signed update repository, commit or order")
    require((a.mode == "dev" and a.tag == "dev-" + a.commit[:12]) or
            (a.mode == "release" and re.fullmatch(SEMVER, a.tag)), "invalid update build version")
    payload = inspect(a.zip)
    build = json.loads(payload[MANIFEST])
    require(build["repo"] == a.repo and build["commit"] == a.commit
            and build["target"].startswith("x86_64-w64-mingw32")
            and build["update_order"] == a.order and build["update_tag"] == a.tag
            and build["update_public_key"] == public, "packaged identity disagrees with signing inputs")
    identity = [a.repo, "windows-x64", "test" if a.mode == "dev" else "stable",
                a.tag, a.order, a.commit]
    inventory = body_bytes(["shiguang-inventory/1", *identity, *entries(payload)])
    with tempfile.TemporaryDirectory(prefix="sg-update-sign-") as directory:
        path = Path(directory) / "key"
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as secret:
            secret.write(key + "\n")
        actual = openssl("pkey", "-in", str(path), "-pubout", "-outform", "DER")
        require(actual == SPKI_PREFIX + bytes.fromhex(public), "configured signing key does not match pinned P-256 key")
        def sign(body):
            return signature_rs(openssl("dgst", "-sha256", "-sign", str(path), data=body))
        payload[INVENTORY] = inventory
        payload[PROOF] = sign(inventory).encode("ascii")
        write_zip(payload, a.output, final=True)
        size = a.output.stat().st_size
        body = body_bytes(["shiguang-update/2", *identity, str(size),
                           digest(a.output.read_bytes()), *entries(payload)])
        note = f"{NOTE}{base64.b64encode(body).decode('ascii')} {sign(body)} -->"
    tag = DEV_TAG if a.mode == "dev" else a.tag
    verify_final(a.output, public, note, tag)
    print(note)


def main():
    cli = argparse.ArgumentParser()
    sub = cli.add_subparsers(dest="command", required=True)
    pack_cmd = sub.add_parser("pack")
    pack_cmd.add_argument("dir", type=Path)
    pack_cmd.add_argument("zip", type=Path)
    inspect_cmd = sub.add_parser("inspect")
    inspect_cmd.add_argument("zip", type=Path)
    sign_cmd = sub.add_parser("sign")
    sign_cmd.add_argument("zip", type=Path)
    sign_cmd.add_argument("output", type=Path)
    sign_cmd.add_argument("mode", choices=("dev", "release"))
    for arg in ("repo", "tag", "commit", "order"):
        sign_cmd.add_argument(arg)
    verify_cmd = sub.add_parser("verify")
    verify_cmd.add_argument("zip", type=Path)
    verify_cmd.add_argument("note", type=Path)
    verify_cmd.add_argument("tag")
    a = cli.parse_args()
    try:
        if a.command == "pack":
            pack(a.dir, a.zip)
        elif a.command == "inspect":
            inspect(a.zip)
        elif a.command == "verify":
            verify_final(a.zip, os.environ.get("SG_UPDATE_PUBLIC_KEY"), a.note.read_text().strip(), a.tag)
        else:
            seal(a)
    except (ValueError, KeyError, TypeError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        cli.exit(1, f"update release: {error}\n")


if __name__ == "__main__":
    main()
