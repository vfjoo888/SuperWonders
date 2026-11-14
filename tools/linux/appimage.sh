#!/bin/bash
# tools/linux/appimage.sh: build/dist/SuperWonders-v<version>-Linux-x64.AppImage (and the .tar.gz), built in an Ubuntu
# 22.04 container (podman; tools/linux/Containerfile) so it runs on glibc 2.35+ distributions. After Savage Wind's
# tools/linux/appimage.sh (MIT). x86_64 only. See tools/linux/package.sh for what goes in.
#   TRANSLATOR=<published linux-x64 spirv_harness> [REPO=owner/name] tools/linux/appimage.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${TRANSLATOR:?TRANSLATOR=<a published linux-x64 spirv_harness (dotnet publish ... -r linux-x64 --self-contained)>}"
podman build -q -t localhost/superwonders-build:amd64 -f tools/linux/Containerfile tools/linux >/dev/null
TR=$(realpath "$TRANSLATOR")
podman run --rm -v "$PWD:/src" -v "$TR:/translator:ro" -w /src -e TRANSLATOR=/translator -e REPO="${REPO:-}" \
    localhost/superwonders-build:amd64 tools/linux/package.sh
