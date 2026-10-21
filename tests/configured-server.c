#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libseat.h"

static void enable(struct libseat *seat, void *data) {
	(void)seat;
	*(bool *)data = true;
}

static void disable(struct libseat *seat, void *data) {
	(void)data;
	libseat_disable_seat(seat);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		return 1;
	}
	char directory[] = "/tmp/seatd-config-test.XXXXXX";
	if (mkdtemp(directory) == NULL) {
		return 2;
	}
	char socket_path[256];
	if (snprintf(socket_path, sizeof socket_path, "%s/seat-test.sock", directory) < 0) {
		return 3;
	}
	int ready[2];
	if (pipe(ready) == -1) {
		return 4;
	}
	pid_t server = fork();
	if (server == -1) {
		return 5;
	}
	if (server == 0) {
		close(ready[0]);
		char readiness[32];
		snprintf(readiness, sizeof readiness, "%d", ready[1]);
		setenv("SEATD_SEAT", "seat-test", 1);
		setenv("SEATD_SOCK", socket_path, 1);
		setenv("SEATD_VTBOUND", "0", 1);
		setenv("SEATD_DEVICES", "", 1);
		execl(argv[1], argv[1], "-l", "error", "-n", readiness, NULL);
		_exit(127);
	}
	close(ready[1]);
	char notification;
	if (read(ready[0], &notification, 1) != 1 || notification != '\n') {
		kill(server, SIGTERM);
		return 6;
	}
	close(ready[0]);

	struct stat socket_stat;
	if (stat(socket_path, &socket_stat) == -1 || (socket_stat.st_mode & 0777) != 0600) {
		kill(server, SIGTERM);
		return 7;
	}
	setenv("SEATD_SOCK", socket_path, 1);
	setenv("LIBSEAT_BACKEND", "seatd", 1);
	unsetenv("SEATD_FD");
	bool active = false;
	const struct libseat_seat_listener listener = {
		.enable_seat = enable,
		.disable_seat = disable,
	};
	struct libseat *seat = libseat_open_seat(&listener, &active);
	if (seat == NULL || strcmp(libseat_seat_name(seat), "seat-test") != 0) {
		kill(server, SIGTERM);
		return 8;
	}
	while (!active) {
		if (libseat_dispatch(seat, 1000) == -1) {
			kill(server, SIGTERM);
			return 9;
		}
	}
	if (libseat_close_seat(seat) == -1) {
		kill(server, SIGTERM);
		return 10;
	}
	if (kill(server, SIGTERM) == -1) {
		return 11;
	}
	int status;
	if (waitpid(server, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return 12;
	}
	if (rmdir(directory) == -1) {
		return 13;
	}
	return 0;
}
