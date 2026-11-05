# Compatible multiseatd extensions

This fork keeps upstream libseat's public headers, exported symbol set, soname,
wire headers and normal `SEATD_SOCK` behavior unchanged. It adds an inherited-FD
seatd extension and an optional, explicitly selected native multiseatd backend.

`SEATD_FD=<n>` adopts a preconnected Unix stream. The value is parsed strictly
and removed from the environment, the fd must be connected to a root-owned
peer, and `FD_CLOEXEC`/`O_NONBLOCK` are set. An invalid explicit value fails
rather than falling back to `SEATD_SOCK`.

No pidfd or ancillary descriptor is attached to the seatd protocol. Socket EOF
is authoritative for transport lifetime, not proof of device revocation, so
launchers must close every duplicate outside the intended
compositor lineage. The seatd backend reports EOF as a terminal disconnection
after dispatching any complete events already buffered; later dispatch calls
continue to fail rather than leaving a dead readable fd in the caller's event
loop.

## Native multiseatd bootstrap

Build with `-Dlibseat-multiseatd=enabled` (optional libdbus dependency). Select
`LIBSEAT_BACKEND=multiseatd` explicitly and supply trusted `XDG_SEAT` and
`MULTISEATD_AUTHORITY_FD`, a delegated Unix socket descriptor at least 3.
This requires multiseatd's new per-seat `org.SGLogin.Multiseatd1` interface,
not its older global v2 interface.

The backend immediately consumes/unsets the authority variable and marks a
validated descriptor CLOEXEC. It resolves the conventional seat service,
verifies root ownership, pins the unique owner and calls `CreateConnection(h)`
on a private blocking libdbus connection without autostart. RPCs share a
ten-second budget (libdbus connection setup has its own standard timeout).
The returned root-peer socket is CLOEXEC/nonblocking; seatd wire initialization
checks the exact seat name before invoking listeners, with a ten-second initial
wire-response deadline. Subsequent operations use the same seatd implementation.
The temporary bus and consumed authority copy close on success and failure.
Malformed descriptor text/types must not close guessed unrelated descriptors.

There is no background thread, ongoing D-Bus subscription, Rust dependency or
change to libseat's public API/ABI. Automatic backend probing skips multiseatd;
explicit failure never falls back to seatd/logind/noop. Existing seatd inherited
FD/socket behavior remains unchanged. A second native open requires deliberate
fresh authority delegation; protocol operations on an existing connection are
unaffected. Controllers must retain their independent authority copy.

Native mode trusts compositor loader/init/pre-open helpers with FULL context
authority. Consumption during libseat open is weaker than an external strict
launcher that closes authority before compositor exec. Audit pre-open helper,
module and argument configuration; never give the capability to an ordinary UI.
The optional stock-Cage fixture below checks actual consumption before UI exec,
not arbitrary compositor configuration or physical device isolation.

## Standalone server

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

On a VT-bound seat, seatd does not acknowledge a kernel VT release until the
old client acknowledges disable or disconnects. A monotonic two-second timeout
forces the old client disabled and completes the release if it is unresponsive,
so serialization cannot wedge VT switching indefinitely.

Build and test as upstream:

```sh
meson setup build \
  -Dlibseat-seatd=enabled -Dlibseat-logind=disabled \
  -Dlibseat-builtin=disabled -Dserver=enabled -Dexamples=enabled
meson compile -C build
meson test -C build --print-errorlogs
sudo ./build/inherited_fd_test
```

For native bootstrap tests, enable the backend in the Meson build, then run:

```sh
sudo dbus-run-session --config-file="$PWD/tests/multiseatd-bus.conf" -- \
  ./build/multiseatd_bootstrap_test
CAGE_BINARY=/absolute/path/to/stock/cage LIBSEAT_BUILD="$PWD/build" \
  sh tests/native-compositor.sh
```

These use a private bus, socketpairs and `/dev/null`, not host device ACLs/PAM
or services. The optional Cage fixture needs wlroots-0.20/Wayland development
files, substitutes a disposable headless output and tests a real wlroots/libseat
session plus an unprivileged Wayland client's FD/authority/terminal inheritance.
It does not establish physical graphics/input or authenticated login acceptance.
Missing/root-only prerequisites return 77, a skip rather than a pass.

The inherited-fd test skips without root because peer-UID validation is part
of the contract. It exercises an arbitrary descriptor number and verifies EOF
without pidfd assistance. The patched and upstream libraries must have
identical `nm -D --defined-only` symbol lists before release.
