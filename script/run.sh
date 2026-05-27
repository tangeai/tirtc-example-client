#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

platform=${PLATFORM:-}
if [ -z "$platform" ]; then
  case "$(uname -s)" in
    Darwin) platform=macos-arm64 ;;
    Linux) platform=linux-x64 ;;
    *)
      echo "[client-example] unsupported host: $(uname -s)" >&2
      exit 3
      ;;
  esac
fi

binary="$repo_root/build/$platform/tirtc_client_example"
if [ ! -x "$binary" ]; then
  (cd "$repo_root" && PLATFORM="$platform" make)
fi

lib_dir="$repo_root/3rd/$platform/lib"
case "$platform" in
  macos-arm64)
    DYLD_LIBRARY_PATH="$lib_dir:${DYLD_LIBRARY_PATH:-}" "$binary" "$@"
    ;;
  linux-x64)
    LD_LIBRARY_PATH="$lib_dir:${LD_LIBRARY_PATH:-}" "$binary" "$@"
    ;;
  *)
    echo "[client-example] unsupported PLATFORM=$platform" >&2
    exit 3
    ;;
esac
