#!/usr/bin/env bash
# Deploy Faraday to faraday.openconverters.com (51.15.253.66).
#
#   ./scripts/deploy.sh            # build from clean HEAD, deploy, byte-verify
#   ./scripts/deploy.sh --stage    # everything except the live-URL checks
#                                  # (use before the DNS A record exists)
#
# House rule, learned the hard way on Kirchhoff: a production build bundles the
# WORKING TREE, so it can silently ship uncommitted edits or a stale dist/.
# This script therefore (a) refuses to deploy a dirty tree unless --allow-dirty,
# (b) rebuilds the WASM engine, and (c) after the rsync, hashes the LIVE files
# against the local build — per artifact, engine AND bundle separately.
#
# THE HASH CHECK PROVES TRANSPORT, NOT PROVENANCE — and for the WASM engine it
# cannot prove provenance even in principle, because this emcc toolchain is not
# byte-reproducible. Measured 2026-08-19: two builds of identical source in the
# identical directory, back to back, gave two different binaries — same
# 1,372,181 bytes, 18 bytes different inside the CODE section, no custom
# sections involved, flipping between exactly two outputs. BINARYEN_CORES=1 and
# EMCC_CORES=1 do not settle it, which points at pointer-ordered containers in
# LLVM/Binaryen rather than at anything a flag reaches. So rebuilding elsewhere
# and comparing hashes would raise false alarms, and a match would be luck.
#
# What replaces it is behavioural and stronger: tests/e2e/deployed.spec.js runs
# the SAME board through the live engine and through a locally built clean-HEAD
# engine and requires the reports to be identical finding for finding — which
# subsumes the 18 bytes, since if any of them mattered the reports would differ
# — and then drives the live app end to end with an empty console.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${FARADAY_HOST:-root@51.15.253.66}"
# the local dev server the live-vs-local gate compares against; 5199 may be
# held by another project's dev server on a shared machine
# 5199 is a shared-machine collision waiting to happen (another project's dev
# server has held it since August). The gate needs A port, not that one.
DEV_PORT="${FARADAY_DEV_PORT:-5198}"
KEY="${FARADAY_KEY:-$HOME/.ssh/om_scaleway}"
URL="${FARADAY_URL:-https://faraday.openconverters.com}"
REMOTE_DIR=/opt/faraday/dist
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no)

STAGE_ONLY=0
ALLOW_DIRTY=0
for a in "$@"; do
    case "$a" in
        --stage) STAGE_ONLY=1 ;;
        --allow-dirty) ALLOW_DIRTY=1 ;;
        *) echo "unknown flag: $a" >&2; exit 2 ;;
    esac
done

if [ "$ALLOW_DIRTY" = 0 ] && [ -n "$(git status --porcelain -- cpp web scripts)" ]; then
    echo "working tree is dirty under cpp/ web/ scripts/ — commit first, or pass" >&2
    echo "--allow-dirty if you really mean to ship uncommitted changes:" >&2
    git status --short -- cpp web scripts >&2
    exit 1
fi

echo "==> building WASM engine"
./scripts/build_wasm.sh
echo "==> building SPA"
(cd web && npm run build)

# The vhost is part of the deployment. It was installed by hand twice, and the
# second time the hand-edit was the only thing standing between the site and a
# missing route — so it ships here. certbot OWNS the TLS block in the live
# file, so the repo's copy is MERGED INTO the live one rather than replacing
# it: overwriting would drop the certificate lines and take the site off HTTPS.
echo "==> nginx: merging any missing locations into the live vhost"
"${SSH[@]}" "$HOST" "test -f /etc/nginx/sites-available/faraday" || {
    echo "no faraday vhost on $HOST — install ops/faraday.nginx by hand first" >&2
    exit 1; }
"${SSH[@]}" "$HOST" "cp /etc/nginx/sites-available/faraday /etc/nginx/sites-available/faraday.bak-$(date +%Y%m%d%H%M%S)"
scp -q -i "$KEY" -o StrictHostKeyChecking=no ops/faraday.nginx "$HOST:/tmp/faraday.repo.nginx"
scp -q -i "$KEY" -o StrictHostKeyChecking=no ops/nginx/oc-librarian-limit.conf "$HOST:/etc/nginx/conf.d/oc-librarian-limit.conf"
"${SSH[@]}" "$HOST" 'python3 - <<'"'"'PYNGINX'"'"'
import re, sys
live = open("/etc/nginx/sites-available/faraday").read()
repo = open("/tmp/faraday.repo.nginx").read()
# every `location <x> {` block the repo defines, by its header line
blocks = {}
for m in re.finditer(r"^(    location .*\{)$", repo, re.M):
    start = m.start()
    depth, i = 0, m.end() - 1
    while i < len(repo):
        if repo[i] == "{": depth += 1
        elif repo[i] == "}":
            depth -= 1
            if depth == 0: break
        i += 1
    blocks[m.group(1).strip()] = repo[start:i + 1]
missing = [b for h, b in blocks.items() if h not in live]
if not missing:
    print("    vhost already carries every location the repo defines")
    sys.exit(0)
anchor = "    location / {"
if anchor not in live:
    print("    CANNOT MERGE: no `location / {` to insert before", file=sys.stderr)
    sys.exit(1)
sep = chr(10) * 2
live = live.replace(anchor, sep.join(missing) + sep + anchor, 1)
open("/etc/nginx/sites-available/faraday", "w").write(live)
print(f"    added {len(missing)} location block(s)")
PYNGINX'
"${SSH[@]}" "$HOST" "nginx -t && systemctl reload nginx" >/dev/null 2>&1 || {
    echo "nginx refused the merged config — restoring the backup" >&2
    "${SSH[@]}" "$HOST" 'cp $(ls -t /etc/nginx/sites-available/faraday.bak-* | head -1) /etc/nginx/sites-available/faraday && nginx -t && systemctl reload nginx'
    exit 1; }

echo "==> rsync to $HOST:$REMOTE_DIR"
"${SSH[@]}" "$HOST" "mkdir -p $REMOTE_DIR"
rsync -az --delete -e "ssh -i $KEY -o StrictHostKeyChecking=no" \
    web/dist/ "$HOST:$REMOTE_DIR/"

# gzip_static: precompress so nginx can serve .gz without on-the-fly cost
"${SSH[@]}" "$HOST" \
    "find $REMOTE_DIR -type f \\( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.wasm' \\) -exec gzip -9 -k -f {} \\; ; nginx -t && systemctl reload nginx"

if [ "$STAGE_ONLY" = 1 ]; then
    echo "==> staged only (no live-URL verification requested)"
    "${SSH[@]}" "$HOST" "ls -la $REMOTE_DIR | head"
    exit 0
fi

echo "==> byte-verifying the LIVE artifacts against the local build"
fail=0
for f in faraday.js faraday.wasm index.html; do
    local_sum=$(sha256sum "web/dist/$f" | cut -d' ' -f1)
    live_sum=$(curl -fsSL "$URL/$f" | sha256sum | cut -d' ' -f1)
    if [ "$local_sum" = "$live_sum" ]; then
        echo "    OK   $f  ${local_sum:0:16}"
    else
        echo "    DIFF $f  local ${local_sum:0:16} != live ${live_sum:0:16}" >&2
        fail=1
    fi
done
# the hashed bundle is named in index.html — verify it too
bundle=$(grep -oE '/assets/index-[A-Za-z0-9_-]+\.js' web/dist/index.html | head -1)
if [ -n "$bundle" ]; then
    local_sum=$(sha256sum "web/dist$bundle" | cut -d' ' -f1)
    live_sum=$(curl -fsSL "$URL$bundle" | sha256sum | cut -d' ' -f1)
    [ "$local_sum" = "$live_sum" ] \
        && echo "    OK   $bundle  ${local_sum:0:16}" \
        || { echo "    DIFF $bundle" >&2; fail=1; }
fi
[ "$fail" = 0 ] || { echo "byte verification FAILED — do not trust this deploy" >&2; exit 1; }

echo "==> live engine vs a clean-HEAD build, and the app end to end"
# The comparison needs a local build to compare against, so serve one.
# nohup, not a backgrounded subshell: the subshell's child was dying with the
# shell that spawned it, so the comparison had nothing to compare against and
# the gate failed for a reason that had nothing to do with the deployment.
# And never pipe this script through grep or tail — the pipeline's exit status
# is the LAST command's, so a failing gate would report success.
(cd web && nohup npx vite --port "$DEV_PORT" --strictPort >/tmp/faraday-deploy-dev.log 2>&1 &)
trap 'pkill -f "vite --port $DEV_PORT" 2>/dev/null || true' EXIT
sleep 8
(cd web && FARADAY_E2E_BASE="$URL" FARADAY_LOCAL_BASE="http://localhost:$DEV_PORT" \
    npx playwright test deployed.spec.js --reporter=line)
(cd web && FARADAY_E2E_BASE="$URL" npx playwright test --grep "board loads" --reporter=line)

echo "==> deployed and verified: $URL"
