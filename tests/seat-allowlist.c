#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "linked_list.h"
#include "seat.h"

static struct seat *test_seat(void) {
	struct seat *seat = seat_create("seat-test", false);
	if (seat == NULL) {
		return NULL;
	}
	static struct linked_list seats;
	linked_list_init(&seats);
	linked_list_insert(&seats, &seat->link);
	return seat;
}

int main(void) {
	int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	int zero_fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
	if (null_fd == -1 || zero_fd == -1) {
		return 1;
	}

	struct seat *unrestricted = test_seat();
	if (unrestricted == NULL || !seat_device_allowed(unrestricted, "/dev/zero", zero_fd)) {
		return 2;
	}
	seat_destroy(unrestricted);

	struct seat *restricted = test_seat();
	if (restricted == NULL || seat_set_device_allowlist(restricted, "/dev/null") == -1) {
		return 3;
	}
	if (!seat_device_allowed(restricted, "/dev/null", null_fd) ||
	    seat_device_allowed(restricted, "/dev/null", zero_fd) ||
	    seat_device_allowed(restricted, "/dev/zero", null_fd)) {
		return 4;
	}
	seat_destroy(restricted);

	struct seat *empty = test_seat();
	if (empty == NULL || seat_set_device_allowlist(empty, "") == -1 ||
	    seat_device_allowed(empty, "/dev/null", null_fd)) {
		return 5;
	}
	seat_destroy(empty);

	struct seat *malformed = test_seat();
	if (malformed == NULL || seat_set_device_allowlist(malformed, "/dev/null,") != -1) {
		return 6;
	}
	seat_destroy(malformed);

	close(null_fd);
	close(zero_fd);
	return 0;
}
