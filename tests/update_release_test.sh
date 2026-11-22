#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
d=$(mktemp -d)
trap 'rm -rf -- "$d"' EXIT
git init --quiet -b main "$d/full"
cd "$d/full"
git -c user.name=test -c user.email=test@example.invalid commit --quiet --allow-empty -m root
root=$(git rev-parse HEAD)
git branch side
git -c user.name=test -c user.email=test@example.invalid commit --quiet --allow-empty -m second
second=$(git rev-parse HEAD)
git checkout -q side
git -c user.name=test -c user.email=test@example.invalid commit --quiet --allow-empty -m side
side=$(git rev-parse HEAD)
git checkout -q main
git -c user.name=test -c user.email=test@example.invalid merge --quiet --no-ff side -m merge
git update-ref refs/remotes/origin/main HEAD

order() { bash "$repo/scripts/update_source_order.sh" "$1"; }
test "$(order "$root")" = 1
test "$(order "$second")" = 2
test "$(order "$(git rev-parse HEAD)")" = 3
if order "$side"; then echo "side-chain candidate was accepted"; exit 1; fi
git clone --quiet --depth=1 "file://$d/full" "$d/shallow"
cd "$d/shallow"
if order "$(git rev-parse HEAD)"; then echo "shallow candidate was accepted"; exit 1; fi
echo "first-parent order fixture passed"

if [ "$#" -ne 1 ]; then
  exit 0
fi
client=$(realpath "$1")
cd "$d"
mkdir pkg
printf 'fixture app' >pkg/shiguang.exe
printf 'fixture helper' >pkg/shiguang-updater.exe
printf 'fixture library' >pkg/runtime.dll
(cd pkg && sha256sum shiguang.exe shiguang-updater.exe runtime.dll >SHA256SUMS.txt)

# Ephemeral test-only signing key, never provisioned in this repository or CI.
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:prime256v1 -out "$d/fixture.pem"
public=$(openssl pkey -in "$d/fixture.pem" -pubout -outform DER | tail -c 64 | xxd -p -c 128)
commit=0123456789abcdef0123456789abcdef01234567
python3 - "$public" "$commit" <<'PY' >pkg/BUILD-MANIFEST.json
import hashlib
import json
from pathlib import Path
import sys
files = [dict(name=p.name, size=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
         for p in sorted(Path("pkg").iterdir()) if p.name != "BUILD-MANIFEST.json"]
print(json.dumps(dict(repo="ToolDream/shiguang", commit=sys.argv[2], ref="refs/heads/main",
    ref_object=sys.argv[2], target="x86_64-w64-mingw32", update_order="42",
    update_tag="dev-" + sys.argv[2][:12], update_public_key=sys.argv[1], files=files)))
PY
producer="$repo/scripts/update_release.py"
python3 "$producer" pack pkg initial.zip
python3 "$producer" inspect initial.zip
mkdir final
args=(sign initial.zip final/shiguang-windows-x64.zip dev ToolDream/shiguang "dev-${commit:0:12}" "$commit" 42)
if SG_UPDATE_PUBLIC_KEY= SG_UPDATE_SIGNING_KEY= python3 "$producer" "${args[@]}"; then
  echo "missing signing configuration was accepted"; exit 1
fi
if SG_UPDATE_PUBLIC_KEY="$public" SG_UPDATE_SIGNING_KEY="$(cat "$d/fixture.pem")" \
    python3 "$producer" "${args[@]:0:7}" 43; then
  echo "mismatched order was signed"; exit 1
fi
SG_UPDATE_PUBLIC_KEY="$public" SG_UPDATE_SIGNING_KEY="$(cat "$d/fixture.pem")" \
  python3 "$producer" "${args[@]}" > note
(cd final && sha256sum shiguang-windows-x64.zip >shiguang-windows-x64.zip.sha256)
GH_REPO=ToolDream/shiguang MODE=commit COMMIT="$commit" SG_UPDATE_PUBLIC_KEY="$public" \
  SG_FINAL_UPDATE_NOTE="$d/note" SG_FINAL_UPDATE_TAG=dev-latest bash "$repo/scripts/verify_dist.sh" final
if SG_UPDATE_PUBLIC_KEY="$public" python3 "$producer" verify initial.zip note dev-latest; then
  echo "mixed unsigned ZIP and signed metadata accepted"; exit 1
fi
if SG_UPDATE_PUBLIC_KEY="$public" python3 "$producer" verify final/shiguang-windows-x64.zip note v1.2.3; then
  echo "test record incorrectly authorized formal tag"; exit 1
fi
"$client" "$public" note final/shiguang-windows-x64.zip "$d/stage"
