#!/usr/bin/env bash
# The build artifact is checked before the release job seals the final ZIP.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mode=${1:?用法: publish.sh dev|release}
: "${GH_TOKEN:?需要 GH_TOKEN}"
: "${GH_REPO:?需要 GH_REPO}"
: "${RUN_URL:?需要 RUN_URL}"
dist=${DIST:-dist}
zip="$dist/shiguang-windows-x64.zip"
[ "${ZIP_NAME:-shiguang-windows-x64.zip}" = shiguang-windows-x64.zip ] || exit 1
export SG_REQUIRE_UPDATE=1
# Keep the secret out of prechecks (including gh) and the remote adapter.
export -n SG_UPDATE_SIGNING_KEY
case "$mode" in
  dev)
    : "${SHA:?dev 模式需要 SHA}"
    verified=$(MODE=commit COMMIT="$SHA" bash "$here/verify_dist.sh" "$dist")
    version="dev-${SHA:0:12}"
    ;;
  release)
    : "${REF:?release 模式需要 REF}"
    version=${REF#refs/tags/}
    [[ "$REF" =~ ^refs/tags/v(0|[1-9][0-9]{0,9})\.(0|[1-9][0-9]{0,9})\.(0|[1-9][0-9]{0,9})$ ]] ||
      { echo "::error::正式 tag 必须为 vMAJOR.MINOR.PATCH"; exit 1; }
    verified=$(MODE=ref bash "$here/verify_dist.sh" "$dist")
    ;;
  *) exit 1 ;;
esac
printf '%s\n' "$verified"
commit=$(sed -n 's/^commit=//p' <<< "$verified")
ref_object=$(sed -n 's/^ref_object=//p' <<< "$verified")
order=$(bash "$here/update_source_order.sh" "$commit")
[ "$order" = "$(sed -n 's/^update_order=//p' <<< "$verified")" ] ||
  { echo "::error::构件序号不匹配 main 第一父链"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export PYTHONDONTWRITEBYTECODE=1
SG_UPDATE_SIGNING_KEY="${SG_UPDATE_SIGNING_KEY:-}" python3 "$here/update_release.py" sign "$zip" "$work/shiguang-windows-x64.zip" \
  "$mode" "$GH_REPO" "$version" "$commit" "$order" >"$work/signed-note"
# Never pass the private key to the remote adapter or gh.
unset SG_UPDATE_SIGNING_KEY
(cd "$work" && sha256sum shiguang-windows-x64.zip >shiguang-windows-x64.zip.sha256)
if [ "$mode" = dev ]; then
  MODE=commit COMMIT="$SHA" SG_FINAL_UPDATE_NOTE="$work/signed-note" SG_FINAL_UPDATE_TAG=dev-latest \
    bash "$here/verify_dist.sh" "$work"
else
  MODE=ref SG_FINAL_UPDATE_NOTE="$work/signed-note" SG_FINAL_UPDATE_TAG="$version" \
    bash "$here/verify_dist.sh" "$work"
fi
python3 "$here/update_publish.py" "$mode" "$zip" "$work/shiguang-windows-x64.zip" \
  "$work/signed-note" "$commit" "$version" "$ref_object"
