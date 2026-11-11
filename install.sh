#!/usr/bin/env bash
#
# debar installer
#
#   curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/install.sh | bash
#   curl -fsSL .../install.sh | bash -s -- --system
#   curl -fsSL .../install.sh | bash -s -- --prefix "$HOME/.local" --version v0.1.0
#
# Installs the prebuilt binary from the latest GitHub release into
# $PREFIX/bin (default: ~/.local/bin). No sudo is needed for the default.
set -euo pipefail

REPO="${DEBAR_REPO:-Gylmynnn/Debar}"
PREFIX="${DEBAR_PREFIX:-$HOME/.local}"
VERSION="${DEBAR_VERSION:-}"

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

info() {
  printf '%s\n' "$*"
}

usage() {
  cat <<EOF
debar installer

Usage:
  install.sh [options]

Options:
  --prefix DIR       Install under DIR (binary goes to DIR/bin/debar).
                     Default: \$HOME/.local, or /usr/local with --system.
  --system           Shortcut for --prefix /usr/local (uses sudo if needed).
  --version TAG      Release tag to install (default: the latest release).
  -h, --help         Show this help.

Environment:
  DEBAR_PREFIX       Same as --prefix.
  DEBAR_VERSION      Same as --version.
  DEBAR_REPO         GitHub repository (default: ${REPO}).
  DEBAR_BASE_URL     Override the release download base URL (mirrors/tests).
  DEBAR_ASSET_SUFFIX Force an asset suffix ("" for generic, "-arch" for Arch).

Examples:
  curl -fsSL https://raw.githubusercontent.com/${REPO}/main/install.sh | bash
  curl -fsSL https://raw.githubusercontent.com/${REPO}/main/install.sh | bash -s -- --system
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
    --version)
      [ $# -ge 2 ] || die "--version needs a tag"
      VERSION="$2"
      shift 2
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

command -v curl >/dev/null 2>&1 || die "curl is required"
command -v tar >/dev/null 2>&1 || die "tar is required"
command -v install >/dev/null 2>&1 || die "install (coreutils) is required"

case "$(uname -s)" in
  Linux) ;;
  *) die "debar only supports Linux" ;;
esac

case "$(uname -m)" in
  x86_64 | amd64) ARCH="x86_64" ;;
  *)
    die "no prebuilt binary for architecture '$(uname -m)' (only x86_64)"
    ;;
esac

# Arch Linux ships Lua with a different SONAME (liblua5.4.so.5.4 instead of
# liblua5.4.so.0), so the release ships a separate Arch build.
if [ "${DEBAR_ASSET_SUFFIX+x}" = x ]; then
  ASSET_SUFFIX="${DEBAR_ASSET_SUFFIX}"
else
  ASSET_SUFFIX=""
  if [ -r /etc/os-release ]; then
    if grep -qiE '^ID=arch$' /etc/os-release ||
      grep -qiE '^ID_LIKE=.*arch' /etc/os-release; then
      ASSET_SUFFIX="-arch"
    fi
  fi
fi

if [ -z "$VERSION" ]; then
  info "Looking up the latest debar release..."
  release_info="$(
    curl -sSL --connect-timeout 15 -o /dev/null \
      -w '%{http_code} %{url_effective}' \
      "https://github.com/${REPO}/releases/latest"
  )" || die "could not reach GitHub to find the latest release"

  release_status="${release_info%% *}"
  release_url="${release_info#* }"
  if [ "$release_status" != "200" ]; then
    die "no published release found for ${REPO} (HTTP ${release_status})"
  fi

  case "$release_url" in
    */releases/tag/*) VERSION="${release_url##*/}" ;;
    *) die "no published release found for ${REPO}" ;;
  esac
fi

case "$VERSION" in
  v*) ;;
  *) VERSION="v${VERSION}" ;;
esac

ASSET="debar-linux-${ARCH}${ASSET_SUFFIX}.tar.gz"
BASE_URL="${DEBAR_BASE_URL:-https://github.com/${REPO}/releases/download/${VERSION}}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

info "Downloading debar ${VERSION} (${ARCH})..."
curl -fL --retry 3 --connect-timeout 15 --progress-bar \
  -o "${TMP_DIR}/${ASSET}" "${BASE_URL}/${ASSET}" ||
  die "download failed: ${BASE_URL}/${ASSET}"

if curl -fsL --retry 3 --connect-timeout 15 \
  -o "${TMP_DIR}/${ASSET}.sha256" "${BASE_URL}/${ASSET}.sha256" 2>/dev/null; then
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$TMP_DIR" && sha256sum -c "${ASSET}.sha256" >/dev/null) ||
      die "checksum verification failed"
    info "Checksum verified."
  fi
fi

tar -xzf "${TMP_DIR}/${ASSET}" -C "$TMP_DIR"
BIN_SRC="${TMP_DIR}/debar/debar"
[ -x "$BIN_SRC" ] || die "the release archive does not contain debar/debar"

BIN_DEST="${PREFIX}/bin/debar"

# Find the closest existing parent to decide whether sudo is needed.
probe="$PREFIX/bin"
while [ ! -e "$probe" ]; do
  probe="$(dirname "$probe")"
done

if [ -w "$probe" ]; then
  install -Dm755 "$BIN_SRC" "$BIN_DEST"
elif [ "$(id -u)" -eq 0 ]; then
  install -Dm755 "$BIN_SRC" "$BIN_DEST"
elif command -v sudo >/dev/null 2>&1; then
  info "Root privileges are needed to write to ${PREFIX}/bin."
  sudo install -Dm755 "$BIN_SRC" "$BIN_DEST"
else
  die "cannot write to ${PREFIX}/bin (run as root or use --prefix)"
fi

info ""
info "debar ${VERSION} installed to ${BIN_DEST}"

if command -v ldd >/dev/null 2>&1; then
  missing="$(
    ldd "$BIN_DEST" 2>/dev/null |
      grep 'not found' |
      sed 's/^[[:space:]]*//;s/[[:space:]].*//' |
      sort -u |
      tr '\n' ' '
  )"
  if [ -n "$missing" ]; then
    info ""
    info "Warning: missing shared libraries: ${missing}"
    info "Install them with your package manager before running debar."
  fi
fi

case ":${PATH}:" in
  *":${PREFIX}/bin:"*) ;;
  *)
    info ""
    info "Note: ${PREFIX}/bin is not in your PATH. Add it with:"
    info "  export PATH=\"${PREFIX}/bin:\$PATH\""
    ;;
esac

info "Run 'debar' from a Wayland session (Hyprland, Sway, ...)."
