#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
zip_path=
release_repo=${TIRTC_CLIENT_RELEASE_REPO:-tangeai/tirtc-example-client}

usage() {
  cat <<'USAGE'
Usage:
  script/prepare.sh [--zip ZIP]

Downloads or unpacks a TiRTC client runtime release zip and replaces only ./3rd.

Environment:
  TIRTC_CLIENT_RELEASE_REPO   GitHub owner/repo, default tangeai/tirtc-example-client.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --zip)
      [ "$#" -ge 2 ] || { echo "[client-example] missing value for --zip" >&2; exit 2; }
      zip_path=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[client-example] unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "[client-example] missing command: $1" >&2
    exit 3
  }
}

need_cmd unzip

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/tirtc-client-prepare.XXXXXX")
cleanup() {
  rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

if [ -z "$zip_path" ]; then
  need_cmd curl
  latest_url="https://github.com/$release_repo/releases/latest"
  effective_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "$latest_url")
  timestamp=${effective_url##*/}
  case "$timestamp" in
    *[!0-9]*|"")
      echo "[client-example] latest release tag is not a timestamp: $timestamp" >&2
      exit 3
      ;;
  esac
  [ "${#timestamp}" -eq 14 ] || {
    echo "[client-example] latest release tag must be YYYYMMDDHHMMSS: $timestamp" >&2
    exit 3
  }
  zip_path="$tmp_dir/$timestamp.zip"
  asset_url="https://github.com/$release_repo/releases/download/$timestamp/$timestamp.zip"
  echo "[client-example] downloading $asset_url" >&2
  curl -fL "$asset_url" -o "$zip_path"
fi

[ -f "$zip_path" ] || {
  echo "[client-example] runtime zip not found: $zip_path" >&2
  exit 3
}

unzip -q "$zip_path" -d "$tmp_dir/unpacked"
roots=$(find "$tmp_dir/unpacked" -mindepth 1 -maxdepth 1 -type d -name 'client-runtime-*' | wc -l | tr -d ' ')
[ "$roots" = "1" ] || {
  echo "[client-example] zip must contain exactly one client-runtime-<timestamp> root" >&2
  exit 3
}
root_dir=$(find "$tmp_dir/unpacked" -mindepth 1 -maxdepth 1 -type d -name 'client-runtime-*')
for platform in macos-arm64 linux-x64; do
  for subdir in include lib; do
    dir="$root_dir/3rd/$platform/$subdir"
    [ -d "$dir" ] || {
      echo "[client-example] zip missing $platform/$subdir" >&2
      exit 3
    }
    [ "$(find "$dir" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')" -gt 0 ] || {
      echo "[client-example] zip has empty $platform/$subdir" >&2
      exit 3
    }
  done
done

rm -rf "$repo_root/3rd.tmp"
cp -R "$root_dir/3rd" "$repo_root/3rd.tmp"
rm -rf "$repo_root/3rd"
mv "$repo_root/3rd.tmp" "$repo_root/3rd"
echo "[client-example] prepared runtime 3rd/ from $zip_path"

