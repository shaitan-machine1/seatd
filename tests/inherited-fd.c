#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
	struct iovec iov = {.iov_base = &header, .iov_len = sizeof header};
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr message = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof control,
	};
	if (recvmsg(fd, &message, MSG_CMSG_CLOEXEC) != sizeof header ||
	    header.opcode != CLIENT_OPEN_SEAT || header.size != 0) {
		return 1;
	}
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
	if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
		return 2;
	}
	int pidfd;
	memcpy(&pidfd, CMSG_DATA(cmsg), sizeof pidfd);
	char info_path[64];
	snprintf(info_path, sizeof info_path, "/proc/self/fdinfo/%d", pidfd);
	FILE *info = fopen(info_path, "r");
	if (info == NULL) {
		return 3;
	}
	bool found_pid = false;
	char line[128];
	while (fgets(line, sizeof line, info) != NULL) {
		found_pid |= strncmp(line, "Pid:\t", 5) == 0;
	}
	fclose(info);
	close(pidfd);
	if (!found_pid) {
		return 4;
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
		return 5;
	}
	if (read(fd, &header, sizeof header) != sizeof header ||
	    header.opcode != CLIENT_CLOSE_SEAT || header.size != 0) {
		return 6;
	}
	return send_packet(fd, SERVER_SEAT_CLOSED, NULL, 0) == -1 ? 7 : 0;
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
	char value[32];
	snprintf(value, sizeof value, "%d", pair[0]);
	setenv("SEATD_FD", value, 1);
	setenv("LIBSEAT_BACKEND", "seatd", 1);
	struct libseat_seat_listener listener = {
		.enable_seat = enable,
		.disable_seat = disable,
	};
	int active = 0;
	struct libseat *seat = libseat_open_seat(&listener, &active);
	if (seat == NULL || getenv("SEATD_FD") != NULL) {
		return 8;
	}
	while (active == 0) {
		if (libseat_dispatch(seat, 1000) == -1) {
			return 8;
		}
	}
	int descriptor_flags = fcntl(pair[0], F_GETFD);
	int status_flags = fcntl(pair[0], F_GETFL);
	if ((descriptor_flags & FD_CLOEXEC) == 0 || (status_flags & O_NONBLOCK) == 0) {
		return 9;
	}
	if (libseat_close_seat(seat) == -1) {
		return 10;
	}
	int status;
	if (waitpid(server, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return 11;
	}
	return 0;
}
