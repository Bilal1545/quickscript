#!/bin/sh
# qsc installer — downloads the latest prebuilt binary from GitHub Releases.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Bilal1545/Quickscript/main/install.sh | sh
#
# Environment:
#   QSC_REPO     GitHub "owner/repo" (default: Bilal1545/Quickscript)
#   QSC_VERSION  Release tag, or "latest" (default: latest)
#   QSC_PREFIX   Install prefix (default: /usr/local)
set -eu

REPO="${QSC_REPO:-Bilal1545/Quickscript}"
TAG="${QSC_VERSION:-latest}"
PREFIX="${QSC_PREFIX:-/usr/local}"

# Use sudo when the install prefix is not writable by the current user.
if [ -w "$PREFIX" ] || { [ ! -e "$PREFIX" ] && [ -w "$(dirname "$PREFIX")" ]; }; then
    SUDO=""
else
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
        echo "qsc: '$PREFIX' requires elevated privileges; using sudo"
    else
        echo "qsc: '$PREFIX' is not writable and 'sudo' is unavailable" >&2
        echo "qsc: re-run with QSC_PREFIX=\$HOME/.local to install without sudo" >&2
        exit 1
    fi
fi

# --- detect platform ---
os=$(uname -s)
arch=$(uname -m)

case "$os" in
    Linux)  plat_os="linux" ;;
    Darwin) plat_os="macos" ;;
    *) echo "qsc: unsupported OS: $os" >&2; exit 1 ;;
esac

case "$arch" in
    x86_64|amd64)  plat_arch="x86_64" ;;
    arm64|aarch64) plat_arch="aarch64" ;;
    *) echo "qsc: unsupported arch: $arch" >&2; exit 1 ;;
esac

asset="qsc-${plat_os}-${plat_arch}"

if [ "$TAG" = "latest" ]; then
    url="https://github.com/${REPO}/releases/latest/download/${asset}.tar.gz"
else
    url="https://github.com/${REPO}/releases/download/${TAG}/${asset}.tar.gz"
fi

command -v curl >/dev/null 2>&1 || { echo "qsc: 'curl' is required" >&2; exit 1; }
command -v tar  >/dev/null 2>&1 || { echo "qsc: 'tar' is required" >&2; exit 1; }
command -v gcc  >/dev/null 2>&1 || \
    echo "qsc: warning — 'gcc' not found; qsc requires it at runtime to link compiled programs" >&2

echo "qsc: downloading ${asset} (${TAG}) from ${REPO}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

curl -fsSL "$url" -o "$tmp/qsc.tar.gz"
tar -xzf "$tmp/qsc.tar.gz" -C "$tmp"

bin_dir="$PREFIX/bin"
share_dir="$PREFIX/share/qsc"
$SUDO mkdir -p "$bin_dir" "$share_dir"

$SUDO mv -f "$tmp/$asset"   "$share_dir/qsc-bin"
$SUDO mv -f "$tmp/runtime.c" "$share_dir/runtime.c"
$SUDO mv -f "$tmp/runtime.h" "$share_dir/runtime.h"
$SUDO mkdir -p "$share_dir/vendor"
$SUDO mv -f "$tmp/vendor/re.h" "$share_dir/vendor/re.h"
$SUDO mv -f "$tmp/vendor/re.c" "$share_dir/vendor/re.c"
$SUDO chmod +x "$share_dir/qsc-bin"

# On macOS, strip Gatekeeper quarantine flag from the downloaded binary.
if [ "$plat_os" = "macos" ]; then
    $SUDO xattr -d com.apple.quarantine "$share_dir/qsc-bin" 2>/dev/null || true
fi

# Wrapper: ensure QSC_RUNTIME_DIR points at the installed runtime.
wrapper="$tmp/qsc-wrapper"
cat > "$wrapper" <<EOF
#!/bin/sh
exec env QSC_RUNTIME_DIR="\${QSC_RUNTIME_DIR:-$share_dir}" "$share_dir/qsc-bin" "\$@"
EOF
chmod +x "$wrapper"
$SUDO mv -f "$wrapper" "$bin_dir/qsc"

echo "qsc: installed → $bin_dir/qsc"

case ":${PATH:-}:" in
    *":$bin_dir:"*) ;;
    *) echo "qsc: add '$bin_dir' to your PATH to use the 'qsc' command" ;;
esac
