#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CPUS="${CPUS:-4}"
RAM="${RAM:-4608}"
WEB_PORT="${WEB_PORT:-8080}"
VNC_PORT="${VNC_PORT:-5900}"
LISTEN="${LISTEN:-127.0.0.1}"

DISK1=""
EXTRA_ARGS=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- extra winvm args]

Launch Windows 11 ARM64 on DGX Spark using build/winvm.

Options:
  --virtio-net    Attach work/virtio_net.img as --disk1 (for NetKVM driver install)
  --install       Attach work/installer.img as --disk1 (for WinPE provisioning)
  -h, --help      Show this help message

Environment overrides:
  CPUS=$CPUS  RAM=$RAM  WEB_PORT=$WEB_PORT  VNC_PORT=$VNC_PORT  LISTEN=$LISTEN
  (the web viewer and VNC have no authentication: keep LISTEN=127.0.0.1 and use
   ssh -L, or set LISTEN=0.0.0.0 only on a trusted network)

Any other flags are passed directly to build/winvm (e.g. --headless, --share ~/shared).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --virtio-net)
      DISK1="$ROOT_DIR/work/virtio_net.img"
      shift
      ;;
    --install)
      DISK1="$ROOT_DIR/work/installer.img"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      EXTRA_ARGS+=("$@")
      break
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

BIN="$ROOT_DIR/build/winvm"
CODE="$ROOT_DIR/work/code.fd"
VARS="$ROOT_DIR/work/vars.fd"
DISK="$ROOT_DIR/work/windows11.img"

for req in "$BIN" "$CODE" "$VARS" "$DISK"; do
  if [[ ! -e "$req" ]]; then
    echo "Error: missing required file: $req" >&2
    exit 1
  fi
done

CMD=(
  "$BIN"
  --code "$CODE"
  --vars "$VARS"
  --disk "$DISK"
  --cpus "$CPUS"
  --ram "$RAM"
  --web "$WEB_PORT"
  --vnc "$VNC_PORT"
  --listen "$LISTEN"
)

if [[ -n "$DISK1" ]]; then
  if [[ ! -e "$DISK1" ]]; then
    echo "Error: missing secondary disk image: $DISK1" >&2
    exit 1
  fi
  CMD+=(--disk1 "$DISK1")
fi

if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
  CMD+=("${EXTRA_ARGS[@]}")
fi

exec "${CMD[@]}"
