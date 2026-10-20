# Compatible multiseatd extensions

This fork keeps upstream libseat's public headers, exported symbol set, soname,
wire headers and normal `SEATD_SOCK` behavior unchanged. It adds two private,
backward-compatible seatd-backend extensions:

1. `SEATD_FD=<n>` adopts a preconnected Unix stream. The value is parsed
   strictly and removed from the environment, the fd must be connected to a
   root-owned peer, and `FD_CLOEXEC`/`O_NONBLOCK` are set. An invalid explicit
   value fails rather than falling back.
2. On Linux, `libseat_open_seat()` opens a pidfd for itself and attaches it via
   `SCM_RIGHTS` to the otherwise unchanged `CLIENT_OPEN_SEAT` message.
   multiseatd uses this to end the client when the actual compositor exits,
   even if a helper leaked a socket duplicate. An old seatd ignores and later
   closes the unexpected descriptor. If pidfd creation is unavailable, libseat
   logs and retains socket-lifetime behavior.

Build and test as upstream:

```sh
meson setup build \
  -Dlibseat-seatd=enabled -Dlibseat-logind=disabled \
  -Dlibseat-builtin=disabled -Dserver=enabled -Dexamples=enabled
meson compile -C build
meson test -C build --print-errorlogs
sudo ./build/inherited_fd_test
```

The inherited-fd test skips without root because peer-UID validation is part
of the contract. The patched and upstream libraries must have identical
`nm -D --defined-only` symbol lists before release.
