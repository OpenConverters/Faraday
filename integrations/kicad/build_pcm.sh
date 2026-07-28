#!/usr/bin/env bash
# Package the plugin in the KiCad PCM zip layout.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf _pcm faraday-pcm.zip
mkdir -p _pcm/plugins
cp plugins/*.py _pcm/plugins/
cp metadata.json _pcm/
(cd _pcm && zip -qr ../faraday-pcm.zip .)
rm -rf _pcm
echo "faraday-pcm.zip ready"
