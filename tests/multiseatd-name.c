#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libseat.h"
#include "multiseatd.h"

static void event(struct libseat *seat, void *data) {
	(void)seat;
	(void)data;
	abort();
}

int main(void) {
	char service[256];
	const char *vectors[][2] = {
		{"seat", "org.SGLogin.Multiseatd1.Seat_seat"},
		{"seat0", "org.SGLogin.Multiseatd1.Seat_seat0"},
		{"seat1", "org.SGLogin.Multiseatd1.Seat_seat1"},
		{"seat-a_b", "org.SGLogin.Multiseatd1.Seat_seat_2da_5fb"},
		{"seat_2da", "org.SGLogin.Multiseatd1.Seat_seat_5f2da"},
		{"seatA", "org.SGLogin.Multiseatd1.Seat_seatA"},
	};
	for (size_t i = 0; i < sizeof vectors / sizeof *vectors; i++) {
		assert(multiseatd_service_name(vectors[i][0], service, sizeof service) == 0);
		assert(strcmp(service, vectors[i][1]) == 0);
	}
	const char *invalid[] = {NULL, "", "other", "Seat0", "seat.a", "seat/a", "seat a", "seat\xc3\xa9"};
	for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
		assert(multiseatd_service_name(invalid[i], service, sizeof service) == -1);
		assert(errno == EINVAL);
	}
	char longest[MAX_SEAT_LEN + 1];
	memset(longest, 'a', sizeof longest);
	memcpy(longest, "seat", 4);
	longest[MAX_SEAT_LEN - 1] = 0;
	assert(multiseatd_service_name(longest, service, sizeof service) == 0);
	longest[MAX_SEAT_LEN - 1] = 'a';
	longest[MAX_SEAT_LEN] = 0;
	assert(multiseatd_service_name(longest, service, sizeof service) == -1);
	assert(multiseatd_service_name("seat0", service, 4) == -1);
	assert(errno == ENAMETOOLONG);

	assert(setenv("LIBSEAT_BACKEND", "multiseatd", 1) == 0);
	assert(setenv("XDG_SEAT", "seat0", 1) == 0);
	struct libseat_seat_listener listener = {.enable_seat = event, .disable_seat = event};
	const char *bad_fds[] = {"", "garbage", "-1", "+3", " 3", "0", "1", "2", "2147483648"};
	for (size_t i = 0; i < sizeof bad_fds / sizeof *bad_fds; i++) {
		assert(setenv("MULTISEATD_AUTHORITY_FD", bad_fds[i], 1) == 0);
		assert(libseat_open_seat(&listener, NULL) == NULL);
		assert(getenv("MULTISEATD_AUTHORITY_FD") == NULL);
	}
	/* A syntactically valid guess must not close an unrelated ordinary file. */
	int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	assert(fd >= 3);
	char number[32];
	snprintf(number, sizeof number, "%d", fd);
	assert(setenv("MULTISEATD_AUTHORITY_FD", number, 1) == 0);
	assert(libseat_open_seat(&listener, NULL) == NULL);
	assert(getenv("MULTISEATD_AUTHORITY_FD") == NULL);
	assert(fcntl(fd, F_GETFD) != -1);
#ifdef MULTISEATD_ONLY
	/* Automatic probing must not consume native authority or contact D-Bus. */
	assert(setenv("MULTISEATD_AUTHORITY_FD", number, 1) == 0);
	assert(unsetenv("LIBSEAT_BACKEND") == 0);
	assert(libseat_open_seat(&listener, NULL) == NULL);
	assert(getenv("MULTISEATD_AUTHORITY_FD") != NULL);
	assert(fcntl(fd, F_GETFD) != -1);
	assert(setenv("LIBSEAT_BACKEND", "multiseatd", 1) == 0);
	assert(unsetenv("MULTISEATD_AUTHORITY_FD") == 0);
#endif
	close(fd);
	assert(libseat_open_seat(&listener, NULL) == NULL);
	return 0;
}
