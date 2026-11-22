#!/usr/bin/env bash
# 校验发布件：ZIP 摘要、包内清单、EXE 与随包 DLL 的校验值，以及源 ref/提交是否仍与构建时一致。
#
# 用法:
#   MODE=ref    REF=refs/tags/v1.2.3 verify_dist.sh <dist 目录>   # 正式版本：连远端 ref 一起复核
#   MODE=commit COMMIT=<sha>        verify_dist.sh <dist 目录>   # 开发版：只认本次构建的提交
#
# 需要: GH_REPO（ref 模式还要 GH_TOKEN）
set -euo pipefail


here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
dist=${1:?用法: verify_dist.sh <dist 目录>}
zip_name=${ZIP_NAME:-shiguang-windows-x64.zip}
mode=${MODE:-ref}
: "${GH_REPO:?需要 GH_REPO}"

cd "$dist"
for file in "$zip_name" "$zip_name.sha256"; do
  [ -f "$file" ] || { echo "::error::缺少 $file"; exit 1; }
done
sha256sum -c "$zip_name.sha256"
zip_sha=$(sha256sum "$zip_name" | awk '{print $1}')

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -n "${SG_FINAL_UPDATE_NOTE:-}" ]; then
  : "${SG_FINAL_UPDATE_TAG:?需要最终分发的 tag}"
  python3 "$here/update_release.py" verify "$zip_name" "$SG_FINAL_UPDATE_NOTE" "$SG_FINAL_UPDATE_TAG"
elif [ "${SG_REQUIRE_UPDATE:-0}" = 1 ]; then
  python3 "$here/update_release.py" inspect "$zip_name"
fi
unzip -q "$zip_name" -d "$work"
manifest="$work/BUILD-MANIFEST.json"
[ -f "$manifest" ] || { echo "::error::包内缺少 BUILD-MANIFEST.json"; exit 1; }
jq -e --arg repo "$GH_REPO" '.repo == $repo' "$manifest" > /dev/null

commit=$(jq -r '.commit // empty' "$manifest")
pkg_ref=$(jq -r '.ref // empty' "$manifest")
ref_object=$(jq -r '.ref_object // empty' "$manifest")
if [ -z "$commit" ] || [ -z "$pkg_ref" ] || [ -z "$ref_object" ]; then
  echo "::error::清单缺少 commit / ref / ref_object"
  exit 1
fi

# 包内每个文件的大小与 SHA-256 必须与清单一致
count=0
while IFS=$'\t' read -r name size sha; do
  count=$((count + 1))
  case "$name" in
    */*|.*|'') echo "::error::清单里有非法文件名：$name"; exit 1 ;;
  esac
  [ -f "$work/$name" ] || { echo "::error::包内缺少 $name"; exit 1; }
  actual_size=$(stat -c %s "$work/$name")
  [ "$actual_size" = "$size" ] || { echo "::error::$name 大小不符：$actual_size ≠ $size"; exit 1; }
  actual_sha=$(sha256sum "$work/$name" | awk '{print $1}')
  [ "$actual_sha" = "$sha" ] || { echo "::error::$name 的 SHA-256 与清单不符"; exit 1; }
done < <(jq -r '.files[] | [.name, (.size | tostring), .sha256] | @tsv' "$manifest")
[ "$count" -gt 0 ] || { echo "::error::清单里没有文件"; exit 1; }

# 包内不得有清单之外的文件
listed=$(jq -r '.files[].name' "$manifest" | LC_ALL=C sort)
present=$(cd "$work" && ls -A | grep -Fxv 'BUILD-MANIFEST.json' | LC_ALL=C sort)
if [ -n "${SG_FINAL_UPDATE_NOTE:-}" ]; then
  present=$(printf '%s\n' "$present" | grep -Fxv -e UPDATE-INVENTORY.txt -e UPDATE-INVENTORY.sig)
fi
[ "$listed" = "$present" ] || { echo "::error::包内文件与清单不一致"; exit 1; }
(cd "$work" && sha256sum -c SHA256SUMS.txt > /dev/null)

if [ "$mode" = ref ]; then
  : "${REF:?ref 模式需要 REF（完整 ref，例如 refs/tags/v1.2.3）}"
  [ "$pkg_ref" = "$REF" ] || { echo "::error::清单 ref=$pkg_ref 与触发 ref=$REF 不一致"; exit 1; }
  api_ref=${REF#refs/}
  live_object=$(gh api "repos/$GH_REPO/git/ref/$api_ref" --jq .object.sha)
  live_type=$(gh api "repos/$GH_REPO/git/ref/$api_ref" --jq .object.type)
  live_commit=$live_object
  if [ "$live_type" = tag ]; then
    live_commit=$(gh api "repos/$GH_REPO/git/tags/$live_object" --jq .object.sha)
  fi
  if [ "$live_object" != "$ref_object" ]; then
    echo "::error::ref 对象已变化（构建时 $ref_object，现在 $live_object），tag 可能被删除重建"
    exit 1
  fi
  if [ "$live_commit" != "$commit" ]; then
    echo "::error::ref 现在指向 $live_commit，本次构建的是 $commit"
    exit 1
  fi
  echo "ref 复核：$REF 仍指向构建时的提交（$live_type）"
elif [ "$mode" = commit ]; then
  : "${COMMIT:?commit 模式需要 COMMIT}"
  [ "$pkg_ref" = refs/heads/main ] || { echo "::error::开发版必须来自 refs/heads/main，清单记录 $pkg_ref"; exit 1; }
  [ "$commit" = "$COMMIT" ] || { echo "::error::清单提交 $commit 与本次构建 $COMMIT 不一致"; exit 1; }
  echo "提交复核：清单与本次构建都是 $commit"
else
  echo "::error::未知 MODE：$mode"
  exit 1
fi

echo "zip=$zip_name"
echo "zip_sha256=$zip_sha"
echo "commit=$commit"
echo "ref_object=$ref_object"
echo "files=$count"
echo "update_order=$(jq -r '.update_order // empty' "$manifest")"
