#!/usr/bin/env bash
# Full-auto: commit → wait CI → download → adb push → insmod → dmesg tail.
# Requires: gh (authed), adb (device online). Run from repo root.

set -euo pipefail

REPO="${REPO:-Ghopp3r/Android-Test-Driver}"
KMI="${KMI:-android15-6.6}"
DRIVER="${DRIVER:-my-driver}"
DEVICE="${DEVICE:-adb-PDNP05J000052536-tm4RhU._adb-tls-connect._tcp}"
COMMIT_MSG="${1:-wip iteration}"

echo ">> stage 1: commit + push"
if ! git diff --quiet HEAD 2>/dev/null || [ -n "$(git status --porcelain)" ]; then
    git add -A
    git commit -m "$COMMIT_MSG"
fi
git push origin main

echo ">> stage 2: wait for CI"
sleep 3
RUN_ID=$(gh run list --repo "$REPO" --limit 1 --json databaseId --jq '.[0].databaseId')
echo "   run: https://github.com/$REPO/actions/runs/$RUN_ID"
gh run watch "$RUN_ID" --repo "$REPO" --exit-status

echo ">> stage 3: download artifact ${DRIVER}-${KMI}.ko"
STAGE=$(mktemp -d)
gh run download "$RUN_ID" --repo "$REPO" -n "${DRIVER}-${KMI}.ko" --dir "$STAGE"
KO="$STAGE/${DRIVER}.ko"
[ -f "$KO" ] || KO=$(find "$STAGE" -name '*.ko' | head -1)
echo "   .ko: $(du -h "$KO" | cut -f1)"

echo ">> stage 4: push to device + insmod"
adb -s "$DEVICE" push "$KO" "/data/local/tmp/${DRIVER}.ko"
adb -s "$DEVICE" shell "su -c 'dmesg -c >/dev/null'"
adb -s "$DEVICE" shell "su -c 'insmod /data/local/tmp/${DRIVER}.ko && echo INSMOD_OK || echo INSMOD_FAIL:\$?'"

echo ">> stage 5: dmesg tail"
adb -s "$DEVICE" shell "su -c 'dmesg | grep -E \"\\[${DRIVER}\\]|memory-driver\" | tail -30'"

echo ">> done. NOTE: HIDE_SELF_MODULE=1 -> lsmod won't list it; reboot device to unload."
