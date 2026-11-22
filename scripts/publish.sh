#!/usr/bin/env bash
# 发布：先建 draft，上传后用 REST 接口回读核实，再复核源 ref，最后才公开。
#
# 用法: publish.sh dev|release
# 需要: GH_TOKEN GH_REPO RUN_URL，可选 ZIP_NAME、DIST
#       release 模式另需 REF（完整 ref，例如 refs/tags/v1.2.3）
#       dev 模式另需 SHA（本次构建的提交）
#
# 身份不靠 release.targetCommitish 推断：发布说明第一行是本流水线写入的机器记录
#    <!-- shiguang-ci-release {"schema":1,"repo":...,"mode":...,"tag":...,"commit":...,"ref_object":...,"zip_sha256":...} -->
# 只有记录中的每一项都与「经验证 manifest + 本地 ZIP 摘要」完全一致，才认为是自己的发布；
# 记录缺失、无法解析或不一致，一律不接管、不覆盖（零写）。
#
# 没有原子保证：全过程只有「不可见的 draft」和「已公开且回读核实过的版本」两种稳定状态。
# 已公开的同身份版本（典型是 rerun failed jobs 复用同一 artifact）只做只读核对后直接成功。
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mode=${1:?用法: publish.sh dev|release}
: "${GH_TOKEN:?需要 GH_TOKEN}"
: "${GH_REPO:?需要 GH_REPO}"
: "${RUN_URL:?需要 RUN_URL}"
dist=${DIST:-dist}
zip_name=${ZIP_NAME:-shiguang-windows-x64.zip}
local_zip="$dist/$zip_name"
record_pattern='^<!-- shiguang-ci-release \(.*\) -->$'

tag_commit() { # 完整 tag 名 → 提交（annotated 自动剥离）；不存在返回空
  local object type
  object=$(gh api "repos/$GH_REPO/git/ref/tags/$1" --jq .object.sha 2>/dev/null) || return 0
  type=$(gh api "repos/$GH_REPO/git/ref/tags/$1" --jq .object.type)
  if [ "$type" = tag ]; then
    gh api "repos/$GH_REPO/git/tags/$object" --jq .object.sha
  else
    printf '%s\n' "$object"
  fi
}

read_record() { # stdin: release body → 记录 JSON（不存在则为空）
  sed -n "s/${record_pattern}/\\1/p" | head -1
}

record_matches() { # $1 = 记录 JSON
  [ -n "$1" ] || return 1
  printf '%s' "$1" | jq -e . > /dev/null 2>&1 || return 1
  printf '%s' "$1" | jq -e \
    --arg repo "$GH_REPO" --arg mode "$mode" --arg tag "$tag" \
    --arg commit "$commit" --arg ref_object "$ref_object" --arg zip "$zip_sha" \
    '.schema == 1 and .repo == $repo and .mode == $mode and .tag == $tag
     and .commit == $commit and .ref_object == $ref_object and .zip_sha256 == $zip' > /dev/null
}

verify_asset() { # $1 = release 数值 id：唯一资产 + 大小 + digest + 回读字节，公开前后共用
  local release_id=$1 assets count id name size state digest
  assets=$(gh api "repos/$GH_REPO/releases/$release_id/assets")
  count=$(printf '%s' "$assets" | jq 'length')
  if [ "$count" != "1" ]; then
    echo "::error::release $release_id 应恰好有 1 个资产，实际 $count"
    return 1
  fi
  id=$(printf '%s' "$assets" | jq -r '.[0].id')
  name=$(printf '%s' "$assets" | jq -r '.[0].name')
  size=$(printf '%s' "$assets" | jq -r '.[0].size | tostring')
  state=$(printf '%s' "$assets" | jq -r '.[0].state // empty')
  digest=$(printf '%s' "$assets" | jq -r '.[0].digest // empty')
  if [[ ! "$id" =~ ^[0-9]+$ ]]; then
    echo "::error::资产 id 不是 REST 数值 id：$id"
    return 1
  fi
  if [ "$name" != "$zip_name" ]; then
    echo "::error::资产名不符：$name"
    return 1
  fi
  if [ -n "$state" ] && [ "$state" != "uploaded" ]; then
    echo "::error::资产状态不是 uploaded：$state"
    return 1
  fi
  local local_size
  local_size=$(stat -c %s "$local_zip")
  if [ "$size" != "$local_size" ]; then
    echo "::error::资产大小不符：远端 $size，本地 $local_size"
    return 1
  fi
  if [ -n "$digest" ] && [ "$digest" != "sha256:$zip_sha" ]; then
    echo "::error::远端资产摘要不符：$digest，本地 sha256:$zip_sha"
    return 1
  fi
  gh api "repos/$GH_REPO/releases/assets/$id" -H 'Accept: application/octet-stream' > "$back/$zip_name"
  if [ "$(sha256sum "$back/$zip_name" | awk '{print $1}')" != "$zip_sha" ]; then
    echo "::error::回读的资产摘要与本地不一致"
    return 1
  fi
  echo "回读核实：$zip_name $size 字节 sha256=$zip_sha（REST 资产 id $id）"
}

case "$mode" in
  release)
    : "${REF:?release 模式需要 REF}"
    tag=${REF#refs/tags/}
    expected_prerelease=false
    case "$tag" in
      dev-*) echo "::error::dev- 前缀是本流水线的机器 tag 保留名：$tag"; exit 1 ;;
    esac
    if [[ ! "$tag" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,99}$ ]]; then
      echo "::error::tag 名称不在允许范围内（字母数字开头，最多 100 个字母数字与 . _ + -）：$tag"
      exit 1
    fi
    title="拾光截图 $tag"
    verified=$(MODE=ref bash "$here/verify_dist.sh" "$dist")
    ;;
  dev)
    : "${SHA:?dev 模式需要 SHA}"
    tag="dev-${SHA:0:12}"
    expected_prerelease=true
    title="开发版 ${SHA:0:12}"
    verified=$(MODE=commit COMMIT="$SHA" bash "$here/verify_dist.sh" "$dist")
    ;;
  *)
    echo "::error::未知模式：$mode"
    exit 1
    ;;
esac
printf '%s\n' "$verified"
commit=$(printf '%s\n' "$verified" | sed -n 's/^commit=//p')
ref_object=$(printf '%s\n' "$verified" | sed -n 's/^ref_object=//p')
zip_sha=$(printf '%s\n' "$verified" | sed -n 's/^zip_sha256=//p')
if [ -z "$commit" ] || [ -z "$ref_object" ] || [ -z "$zip_sha" ]; then
  echo "::error::校验脚本没有给出 commit / ref_object / zip 摘要"
  exit 1
fi

# 开发版：同名机器 tag 若已存在，必须指向本次提交
if [ "$mode" = dev ]; then
  existing_commit=$(tag_commit "$tag" || true)
  if [ -n "$existing_commit" ] && [ "$existing_commit" != "$commit" ]; then
    echo "::error::已存在 tag $tag 指向 $existing_commit，与本次构建 $commit 不符，不覆盖"
    exit 1
  fi
fi

record=$(jq -cn --arg repo "$GH_REPO" --arg mode "$mode" --arg tag "$tag" \
  --arg commit "$commit" --arg ref_object "$ref_object" --arg zip "$zip_sha" \
  '{schema:1, repo:$repo, mode:$mode, tag:$tag, commit:$commit, ref_object:$ref_object, zip_sha256:$zip}')

notes=$(mktemp)
back=$(mktemp -d)
cleanup() { rm -f "$notes"; rm -rf "$back"; }
trap cleanup EXIT

{
  echo "<!-- shiguang-ci-release $record -->"
  if [ "$mode" = dev ]; then
    echo "开发版：提交 $commit 的自动构建。每个 main 提交一个独立、不可变的预发布，不覆盖历史提交。"
    echo
    echo "- 源提交：$commit"
    echo "- 触发 ref：refs/heads/main"
  else
    echo "拾光截图 $tag（Windows x64），本次自动测试通过后的同一份构件。"
    echo
    echo "- 源提交：$commit"
    echo "- 触发 ref：$REF"
  fi
  echo "- 构建运行：$RUN_URL"
  echo "- ZIP SHA-256：$zip_sha"
  echo
  echo "唯一资产 $zip_name 内含 shiguang.exe、SHA256SUMS.txt（EXE 与随包 DLL 的校验值）和 BUILD-MANIFEST.json（源提交、ref 对象、逐个文件的校验值）。"
} > "$notes"

release_id=""
state=$(gh release view "$tag" --json databaseId,isDraft,isPrerelease,body 2>/dev/null || true)
if [ -n "$state" ]; then
  release_id=$(printf '%s' "$state" | jq -r '.databaseId')
  is_draft=$(printf '%s' "$state" | jq -r '.isDraft')
  is_prerelease=$(printf '%s' "$state" | jq -r '.isPrerelease')
  body=$(printf '%s' "$state" | jq -r '.body // empty')
  stored=$(printf '%s\n' "$body" | read_record)
  if ! record_matches "$stored"; then
    echo "::error::$tag 已有 release，但没有与本构建完全一致的本流水线机器记录，不接管也不覆盖"
    exit 1
  fi
  if [ "$is_draft" = "true" ]; then
    echo "续做同身份 draft：$tag（id $release_id，记录来自发布说明）"
  else
    # 已公开的同身份版本：只读核对后幂等成功，不做任何写入
    if [ "$is_prerelease" != "$expected_prerelease" ]; then
      echo "::error::$tag 已公开但预发布标记为 $is_prerelease，与 $mode 模式不符"
      exit 1
    fi
    if [ "$mode" = release ]; then
      MODE=ref bash "$here/verify_dist.sh" "$dist" > /dev/null
    else
      live_commit=$(tag_commit "$tag" || true)
      if [ "$live_commit" != "$commit" ]; then
        echo "::error::$tag 已公开但实际 tag 指向 ${live_commit:-不存在}，与 $commit 不符"
        exit 1
      fi
    fi
    verify_asset "$release_id"
    echo "已公开且记录、tag 与资产完全一致（同一 ZIP），幂等成功，无写入"
    exit 0
  fi
else
  if [ "$mode" = dev ]; then
    gh release create "$tag" --draft --prerelease --target "$commit" --title "$title" --notes-file "$notes"
  else
    # 正式 tag 必须已存在：--verify-tag 让 gh 按现有 tag 建 release，而不是新建 tag。
    gh release create "$tag" --draft --verify-tag --title "$title" --notes-file "$notes"
  fi
  release_id=$(gh release view "$tag" --json databaseId --jq '.databaseId')
  echo "已创建 draft：$tag（id $release_id）"
fi

gh release upload "$tag" "$local_zip" --clobber
verify_asset "$release_id"

if [ "$mode" = release ]; then
  MODE=ref bash "$here/verify_dist.sh" "$dist" > /dev/null
  echo "公开前复核：$REF 仍指向 $commit"
  gh release edit "$tag" --draft=false --title "$title" --notes-file "$notes"
else
  gh release edit "$tag" --draft=false --prerelease --title "$title" --notes-file "$notes"
  live_commit=$(tag_commit "$tag" || true)
  if [ "$live_commit" != "$commit" ]; then
    echo "::error::公开后 $tag 的实际 tag 指向 ${live_commit:-不存在}，与 $commit 不符"
    exit 1
  fi
fi

final=$(gh release view "$tag" --json isDraft,isPrerelease)
if [ "$(printf '%s' "$final" | jq -r '.isDraft')" != "false" ]; then
  echo "::error::发布后仍为 draft"
  exit 1
fi
if [ "$(printf '%s' "$final" | jq -r '.isPrerelease')" != "$expected_prerelease" ]; then
  echo "::error::发布后预发布标记不符"
  exit 1
fi
echo "已公开：$tag（$zip_sha）"
