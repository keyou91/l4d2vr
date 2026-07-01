#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

mkdir -p Release/linux

cxx="${CXX:-g++}"
"$cxx" -m32 -std=c++17 -O2 -fPIC -fvisibility=hidden \
  -Wall -Wextra -Wno-unused-parameter \
  -shared L4D2VR/l4d2vr_server_linux.cpp \
  -ldl -pthread \
  -o Release/linux/l4d2vr_server.so

cp -f L4D2VR/l4d2vr_server.vdf Release/linux/l4d2vr_server.vdf

echo "Built Release/linux/l4d2vr_server.so"
