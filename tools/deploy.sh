#!/usr/bin/env bash
# Full-auto: commit → wait CI (bound to *this* SHA) → download → adb push
# → insmod (real exit-code propagation) → dmesg tail. Requires: gh (authed),
# adb (device online). Run from repo root.
#
# Findings addressed:
#   #15 pushes the userspace helper (my-driver-test) alongside the .ko so the
#       APK's su-c bridge actually has something to execute.
#   #16 selects the CI run by the exact SHA we just pushed (and the workflow
#       name), instead of "whatever ran most recently in the repo".
#   #17 lets insmod's real exit code cross the adb shell boundary so a failed
#       load surfaces as a non-zero pipeline status instead of a friendly echo.

set -euo pipefail

REPO="${REPO:-Ghopp3r/Android-Test-Driver}"
KMI="${KMI:-android15-6.6}"
DRIVER="${DRIVER:-my-driver}"
DEVICE="${DEVICE:-adb-PDNP05J000052536-tm4RhU._adb-tls-connect._tcp}"
WORKFLOW="${WORKFLOW:-build.yml}"
BRANCH="${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}"
COMMIT_MSG="${1:-wip iteration}"

echo ">> stage 1: commit + push"
if ! git diff --quiet HEAD 2>/dev/null || [ -n "$(git status --porcelain)" ]; then
    git add -A
    git commit -m "$COMMIT_MSG"
fi
git push origin "$BRANCH"
SHA=$(git rev-parse HEAD)
echo "   sha: $SHA"

echo ">> stage 2: find CI run for $SHA"
RUN_ID=""
for attempt in $(seq 1 20); do
    RUN_ID=$(gh run list --repo "$REPO" --workflow "$WORKFLOW" \
                        --branch "$BRANCH" --limit 25 \
                        --json databaseId,headSha \
                        --jq ".[] | select(.headSha == \"$SHA\") | .databaseId" \
                        | head -1)
    if [ -n "$RUN_ID" ]; then
        break
    fi
    sleep 3
done
if [ -z "$RUN_ID" ]; then
    echo "!! no CI run for $SHA on branch $BRANCH after 20 attempts" >&2
    exit 2
fi
echo "   run: https://github.com/$REPO/actions/runs/$RUN_ID"
gh run watch "$RUN_ID" --repo "$REPO" --exit-status

echo ">> stage 3: download .ko + userspace helper"
STAGE=$(mktemp -d)
gh run download "$RUN_ID" --repo "$REPO" -n "${DRIVER}-${KMI}.ko" --dir "$STAGE"
gh run download "$RUN_ID" --repo "$REPO" -n "userspace-arm64-v8a"  --dir "$STAGE"

KO=$(find "$STAGE" -name "${DRIVER}-${KMI}.ko" -o -name "${DRIVER}.ko" | head -1)
HELPER=$(find "$STAGE" -type f -name "${DRIVER}-test" | head -1)
[ -f "$KO" ]     || { echo "!! .ko not in artifact" >&2; exit 3; }
[ -f "$HELPER" ] || { echo "!! ${DRIVER}-test not in userspace artifact" >&2; exit 3; }
echo "   .ko:     $(du -h "$KO"     | cut -f1)"
echo "   helper:  $(du -h "$HELPER" | cut -f1)"

echo ">> stage 4: push to device"
adb -s "$DEVICE" push "$KO"     "/data/local/tmp/${DRIVER}.ko"
adb -s "$DEVICE" push "$HELPER" "/data/local/tmp/${DRIVER}-test"
adb -s "$DEVICE" shell "chmod +x /data/local/tmp/${DRIVER}-test"

echo ">> stage 5: insmod (exit code parsed back through adb)"
# Old adb builds swallow the remote shell's exit status, so the previous
# `insmod || echo INSMOD_FAIL` trick left the pipeline reporting success even
# on a failed load. We now print a magic __RC=N marker after insmod and grep
# it out of the captured output — the marker is authoritative, adb's own
# return status is treated as best-effort.
INSMOD_OUT=$(adb -s "$DEVICE" shell "su -c 'insmod /data/local/tmp/${DRIVER}.ko; echo __RC=\$?'")
echo "$INSMOD_OUT"
INSMOD_RC=$(printf '%s\n' "$INSMOD_OUT" | grep -oE '__RC=[0-9]+' | tail -1 | cut -d= -f2)
if [ -z "$INSMOD_RC" ] || [ "$INSMOD_RC" != "0" ]; then
    echo "!! insmod failed (rc=${INSMOD_RC:-<unknown>})" >&2
    adb -s "$DEVICE" shell "su -c 'dmesg | tail -20'" >&2 || true
    exit 4
fi

echo ">> stage 6: dmesg tail"
adb -s "$DEVICE" shell "su -c 'dmesg | grep -E \"\\[${DRIVER}\\]|memory-driver\" | tail -30'"

echo ">> done. HIDE_SELF_MODULE=1 → lsmod won't list it; reboot device to unload."
