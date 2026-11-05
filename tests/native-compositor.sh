#!/bin/sh
# Opt-in native bootstrap/inheritance check; no live DRM/input, PAM or services.
set -eu
for tool in cc pkg-config dbus-run-session sudo; do
    command -v "$tool" >/dev/null 2>&1 || exit 77
done
[ "$(id -u)" != 0 ] || { echo 'build this fixture as the normal user' >&2; exit 77; }
if [ -z "${CAGE_BINARY:-}" ] || [ -z "${LIBSEAT_BUILD:-}" ]; then
    echo 'supply absolute CAGE_BINARY and native-enabled LIBSEAT_BUILD paths' >&2
    exit 77
fi
case "$CAGE_BINARY:$LIBSEAT_BUILD" in /*:/*) ;; *) exit 77 ;; esac
[ -x "$CAGE_BINARY" ] && [ -x "$LIBSEAT_BUILD/multiseatd_bootstrap_test" ] || exit 77
pkg-config --exists wlroots-0.20 wayland-server wayland-client || exit 77
sudo -n true || exit 77
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d /tmp/libseat-native-fixture.XXXXXX)
trap 'rm -f -- "$work/headless.so" "$work/native-ui"; rmdir -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
# The credential-dropped compositor must be able to load these public fixtures.
chmod 755 "$work"
# pkg-config output intentionally supplies separate compiler arguments.
# shellcheck disable=SC2046
cc -Wall -Wextra -Werror -fPIC -shared "$repo/tests/native-headless-session.c" \
    -I"$repo/include" -L"$LIBSEAT_BUILD" -lseat \
    $(pkg-config --cflags --libs wlroots-0.20 wayland-server) -o "$work/headless.so"
# shellcheck disable=SC2046
cc -Wall -Wextra -Werror "$repo/tests/native-ui.c" \
    $(pkg-config --cflags --libs wayland-client) -o "$work/native-ui"
sudo -n dbus-run-session --config-file="$repo/tests/multiseatd-bus.conf" -- \
    "$LIBSEAT_BUILD/multiseatd_bootstrap_test" "$CAGE_BINARY" \
    "$work/native-ui" "$work/headless.so" "$LIBSEAT_BUILD"
echo 'PASS: native stock-Cage initialization and UI inheritance (headless fixture, not physical acceptance)'
