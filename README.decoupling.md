# Compatible multiseatd extensions

This fork keeps upstream libseat's public headers, exported symbol set, soname,
wire headers and normal `SEATD_SOCK` behavior unchanged. It adds one private,
backward-compatible seatd-backend extension:

`SEATD_FD=<n>` adopts a preconnected Unix stream. The value is parsed strictly
and removed from the environment, the fd must be connected to a root-owned
peer, and `FD_CLOEXEC`/`O_NONBLOCK` are set. An invalid explicit value fails
rather than falling back to `SEATD_SOCK`.

No pidfd or ancillary descriptor is attached to the seatd protocol. Socket EOF
is authoritative, so launchers must close every duplicate outside the intended
compositor lineage.

The standalone seatd server also accepts one-seat-per-process configuration:

- `SEATD_SEAT` selects the reported seat name (default `seat0`);
- `SEATD_SOCK` selects a root-only listening socket instead of the compiled
  default;
- existing `SEATD_VTBOUND=0` disables VT binding; and
- `SEATD_DEVICES` is a comma-separated, static device-node allowlist. An empty
  value denies every device; an unset value preserves upstream unrestricted
  behavior.

Allowed paths are canonicalized at startup. Every opened fd must match both the
configured canonical path and its snapshotted device type/number before it is
returned to a client. Restart the process to change the allowlist.

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
of the contract. It exercises an arbitrary descriptor number and verifies EOF
without pidfd assistance. The patched and upstream libraries must have
identical `nm -D --defined-only` symbol lists before release.
