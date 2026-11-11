#!/usr/bin/env bash
#
# debar uninstaller
#
#   curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/uninstall.sh | bash
#   curl -fsSL .../uninstall.sh | bash -s -- --system
#   curl -fsSL .../uninstall.sh | bash -s -- --purge
#
# Removes the installed binary from $PREFIX/bin. With --purge it also deletes
# the user configuration in ${XDG_CONFIG_HOME:-~/.config}/debar.
set -euo pipefail

PREFIX="${DEBAR_PREFIX:-$HOME/.local}"
PURGE=0

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

info() {
  printf '%s\n' "$*"
}

usage() {
  cat <<EOF
debar uninstaller

Usage:
  uninstall.sh [options]

Options:
  --prefix DIR       Remove DIR/bin/debar.
                     Default: \$HOME/.local, or /usr/local with --system.
  --system           Shortcut for --prefix /usr/local (uses sudo if needed).
  --purge            Also remove the config directory (${XDG_CONFIG_HOME:-$HOME/.config}/debar).
  -h, --help         Show this help.

Environment:
  DEBAR_PREFIX       Same as --prefix.

Examples:
  curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/uninstall.sh | bash
  curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/uninstall.sh | bash -s -- --system --purge
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)
      [ $# -ge 2 ] || die "--prefix needs a directory"
      PREFIX="$2"
      shift 2
      ;;
    --system)
      PREFIX="/usr/local"
      shift
      ;;
    --purge)
      PURGE=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1 (try --help)"
      ;;
  esac
done

[ -n "$PREFIX" ] || die "install prefix is empty"

BIN="${PREFIX}/bin/debar"

if [ -e "$BIN" ]; then
  probe="$PREFIX/bin"
  while [ ! -e "$probe" ]; do
    probe="$(dirname "$probe")"
  done

  if [ -w "$probe" ]; then
    rm -f "$BIN"
  elif [ "$(id -u)" -eq 0 ]; then
    rm -f "$BIN"
  elif command -v sudo >/dev/null 2>&1; then
    info "Root privileges are needed to remove ${BIN}."
    sudo rm -f "$BIN"
  else
    die "cannot remove ${BIN} (run as root or use --prefix)"
  fi

  info "Removed ${BIN}"
else
  info "debar is not installed at ${BIN}; nothing to remove."
fi

if [ "$PURGE" -eq 1 ]; then
  CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/debar"
  if [ -d "$CONFIG_DIR" ]; then
    rm -rf "$CONFIG_DIR"
    info "Removed configuration directory ${CONFIG_DIR}"
  else
    info "No configuration directory at ${CONFIG_DIR}."
  fi
fi

info ""
info "Done. If debar is still running, stop it with: pkill -x debar"
