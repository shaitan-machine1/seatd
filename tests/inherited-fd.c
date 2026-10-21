#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

#define TEST_FD 57

static int send_packet(int fd, uint16_t opcode, const void *body, uint16_t size) {
	struct proto_header header = {.opcode = opcode, .size = size};
	struct iovec iov[] = {
		{.iov_base = &header, .iov_len = sizeof header},
		{.iov_base = (void *)body, .iov_len = size},
	};
	return writev(fd, iov, size == 0 ? 1 : 2) == (ssize_t)(sizeof header + size) ? 0 : -1;
}

static int fake_server(int fd) {
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
	if (send_packet(fd, SERVER_SEAT_OPENED, &response, sizeof response) == -1 ||
	    send_packet(fd, SERVER_ENABLE_SEAT, NULL, 0) == -1) {
		return 2;
	}
	if (read(fd, &header, sizeof header) != sizeof header ||
	    header.opcode != CLIENT_CLOSE_SEAT || header.size != 0) {
		return 3;
	}
	if (send_packet(fd, SERVER_SEAT_CLOSED, NULL, 0) == -1) {
		return 4;
	}
	return read(fd, &header, sizeof header) == 0 ? 0 : 5;
}

static void enable(struct libseat *seat, void *data) {
	(void)seat;
	(*(int *)data)++;
}

static void disable(struct libseat *seat, void *data) {
	(void)data;
	libseat_disable_seat(seat);
}

int main(void) {
	if (geteuid() != 0) {
		fprintf(stderr, "SKIP: inherited fd peer-UID validation requires root\n");
		return 77;
	}
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == -1) {
		return 1;
	}
	pid_t server = fork();
	if (server == -1) {
		return 1;
	}
	if (server == 0) {
		close(pair[0]);
		_exit(fake_server(pair[1]));
	}
	close(pair[1]);
	if (pair[0] != TEST_FD) {
		if (dup2(pair[0], TEST_FD) == -1) {
			return 2;
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
	int active = 0;
	struct libseat *seat = libseat_open_seat(&listener, &active);
	if (seat == NULL || getenv("SEATD_FD") != NULL) {
		return 3;
	}
	while (active == 0) {
		if (libseat_dispatch(seat, 1000) == -1) {
			return 4;
		}
	}
	int descriptor_flags = fcntl(TEST_FD, F_GETFD);
	int status_flags = fcntl(TEST_FD, F_GETFL);
	if ((descriptor_flags & FD_CLOEXEC) == 0 || (status_flags & O_NONBLOCK) == 0) {
		return 5;
	}
	if (libseat_close_seat(seat) == -1) {
		return 6;
	}
	int status;
	if (waitpid(server, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return 7;
	}
	return 0;
}
