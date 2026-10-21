#include <errno.h>
#include <stdlib.h>

#include "libseat.h"

static void enable(struct libseat *seat, void *data) {
	(void)seat;
	(void)data;
}

static void disable(struct libseat *seat, void *data) {
	(void)seat;
	(void)data;
}

int main(void) {
	setenv("LIBSEAT_BACKEND", "seatd", 1);
	setenv("SEATD_FD", "not-an-fd", 1);
	setenv("SEATD_SOCK", "/definitely/not/a/seatd/socket", 1);
	struct libseat_seat_listener listener = {
		.enable_seat = enable,
		.disable_seat = disable,
	};
	errno = 0;
	if (libseat_open_seat(&listener, NULL) != NULL) {
		return 1;
	}
	if (getenv("SEATD_FD") != NULL) {
		return 2;
	}
	return errno == EINVAL ? 0 : 3;
}
