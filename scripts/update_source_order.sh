#!/usr/bin/env bash
# Source order is the candidate's own position on origin/main's first-parent line.
set -euo pipefail
commit=${1:?完整候选提交 SHA}
[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid update source SHA" >&2; exit 1; }
[ "$(git rev-parse --is-shallow-repository)" = false ] || { echo "update source history is shallow" >&2; exit 1; }
[ ! -e "$(git rev-parse --git-path info/grafts)" ] || { echo "update source history has grafts" >&2; exit 1; }
export GIT_NO_REPLACE_OBJECTS=1
git rev-list --first-parent --reverse refs/remotes/origin/main |
  awk -v sha="$commit" '$0 == sha { print NR; found=1 } END { if (!found) { print "update source is not on origin/main first-parent line" > "/dev/stderr"; exit 1 } }'
