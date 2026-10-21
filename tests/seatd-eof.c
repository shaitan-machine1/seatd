#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libseat.h"
#include "protocol.h"

#define TEST_FD 58

static int send_packet(int fd, uint16_t opcode, const void *body, uint16_t size) {
	struct proto_header header = {.opcode = opcode, .size = size};
	struct iovec iov[] = {
		{.iov_base = &header, .iov_len = sizeof header},
		{.iov_base = (void *)body, .iov_len = size},
	};
	return writev(fd, iov, size == 0 ? 1 : 2) == (ssize_t)(sizeof header + size) ? 0 : -1;
}

static int fake_server(int fd, bool send_enable) {
	struct proto_header header;
	if (read(fd, &header, sizeof header) != sizeof header ||
	    header.opcode != CLIENT_OPEN_SEAT || header.size != 0) {
		return 1;
	}

	const char name[] = "seat0";
	struct {
		struct proto_server_seat_opened opened;
		char name[sizeof name];
	} response = {
		.opened = {.seat_name_len = sizeof name},
	};
	memcpy(response.name, name, sizeof name);
	if (send_packet(fd, SERVER_SEAT_OPENED, &response, sizeof response) == -1) {
		return 2;
	}
	if (send_enable && send_packet(fd, SERVER_ENABLE_SEAT, NULL, 0) == -1) {
		return 3;
	}
	return 0;
}

static void enable(struct libseat *seat, void *data) {
	(void)seat;
	(*(int *)data)++;
}

static void disable(struct libseat *seat, void *data) {
	(void)seat;
	(void)data;
}

static int run_case(int timeout, bool send_enable) {
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == -1) {
		return 1;
	}
	pid_t server = fork();
	if (server == -1) {
		return 2;
	}
	if (server == 0) {
		close(pair[0]);
		int result = fake_server(pair[1], send_enable);
		close(pair[1]);
		_exit(result);
	}

	close(pair[1]);
	if (pair[0] != TEST_FD) {
		if (dup2(pair[0], TEST_FD) == -1) {
			return 3;
		}
		close(pair[0]);
	}
	char value[32];
	snprintf(value, sizeof value, "%d", TEST_FD);
	setenv("SEATD_FD", value, 1);
	setenv("LIBSEAT_BACKEND", "seatd", 1);
	struct libseat_seat_listener listener = {
		.enable_seat = enable,
		.disable_seat = disable,
	};
	int enabled = 0;
	struct libseat *seat = libseat_open_seat(&listener, &enabled);
	if (seat == NULL) {
		return 4;
	}

	int status;
	if (waitpid(server, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return 5;
	}

	int result = libseat_dispatch(seat, timeout);
	if (send_enable) {
		if (result <= 0 || enabled != 1) {
			return 6;
		}
		if (libseat_dispatch(seat, timeout) != -1 || errno != ENOTCONN) {
			return 7;
		}
	} else if (result != -1 || (errno != EPIPE && errno != ENOTCONN)) {
		return 8;
	}

	if (libseat_dispatch(seat, timeout) != -1 || errno != ENOTCONN) {
		return 9;
	}
	if (libseat_close_seat(seat) != -1 || errno != ENOTCONN) {
		return 10;
	}
	return 0;
}

int main(void) {
	if (geteuid() != 0) {
		fprintf(stderr, "SKIP: inherited fd peer-UID validation requires root\n");
		return 77;
	}

	int result = run_case(0, false);
	if (result != 0) {
		return result;
	}
	result = run_case(-1, false);
	if (result != 0) {
		return 20 + result;
	}
	result = run_case(0, true);
	return result == 0 ? 0 : 40 + result;
}
