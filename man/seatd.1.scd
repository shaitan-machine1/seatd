seatd(1)

# NAME

seatd - A seat management daemon

# SYNOPSIS

*seatd* [options]

# OPTIONS

*-h*
	Show help message and quit.

*-n <fd>*
	FD to notify readiness on. A single newline will be written and the fd
	closed when seatd is ready to serve requests. This is compatible with
	s6's notification protocol.

*-u <user>*
	User to own the seatd socket.

*-g <group>*
	Group to own the seatd socket.

*-l <loglevel>*
	Log-level to use. Must be one of debug, info, error or silent. Defaults
	to error.

*-v*
	Show the version number and quit.

# DESCRIPTION

seatd provides central seat management, mediating access to shared resources
such as displays and input devices in a multi-session, multi-seat environment.

seatd operates over a UNIX domain socket, with *libseat* providing the
client-side of the protocol.

The socket location is set at compile-time unless overridden by *SEATD_SOCK*.

# ENVIRONMENT

*SEATD_SEAT*
	Name reported by this server instance. Defaults to "seat0". One seat is
	served per seatd process.

*SEATD_SOCK*
	Listening socket path for this server instance. When set, the socket is
	created mode 0600 unless *-u* or *-g* requests shared ownership.

*SEATD_VTBOUND*
	If set to "0", the seat will not be bound to a VT.

*SEATD_DEVICES*
	Comma-separated static allowlist of canonical device-node paths. Each
	opened fd is checked against the configured path, device type and device
	number before it is returned. An empty value denies all device opens. An
	unset value preserves the unrestricted upstream behavior.

# SEE ALSO

The libseat library, *<libseat.h>*, *seatd-launch*(1)

# AUTHORS

Maintained by Kenny Levinsen <contact@kl.wtf>, who is assisted by other
open-source contributors. For more information about seatd development, see
https://sr.ht/~kennylevinsen/seatd.
