#!/usr/bin/env bash
# Deletes this ref's Actions caches whose key matches a regex, except the one just saved. Run after
# a cache save on nx2 so each cache family keeps one entry there: superseded entries filled the
# repository's 10 GB cache budget and evicted the release builds' caches.
#
# usage: prune-caches.sh <key regex> <saved key>
set -euo pipefail

export PRUNE_PATTERN=$1 PRUNE_KEEP=$2
gh cache list -R "$GITHUB_REPOSITORY" --ref "$GITHUB_REF" --limit 1000 --json id,key \
    --jq '.[] | select((.key | test(env.PRUNE_PATTERN)) and .key != env.PRUNE_KEEP) | .id' |
    while read -r id; do
        gh cache delete -R "$GITHUB_REPOSITORY" "$id"
    done
