#define _GNU_SOURCE
#include <assert.h>
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libseat.h"
#include "protocol.h"

#define SERVICE "org.SGLogin.Multiseatd1.Seat_seat_2da_5fb"
#define INTERFACE "org.SGLogin.Multiseatd1.Manager"
#define PATH "/org/SGLogin/Multiseatd1"

static void receive(int fd, void *buffer, size_t size) {
	while (size != 0) {
		ssize_t count = read(fd, buffer, size);
		assert(count > 0);
		buffer = (char *)buffer + count;
		size -= (size_t)count;
	}
}

static void send_bytes(int fd, const void *buffer, size_t size) {
	assert(write(fd, buffer, size) == (ssize_t)size);
}

static void response(int fd, uint16_t opcode) {
	struct proto_header header = {.opcode = opcode};
	send_bytes(fd, &header, sizeof header);
}

static void wire_server(int fd, const char *mode) {
	bool wrong_seat = strcmp(mode, "wrong-seat") == 0;
	struct proto_header header;
	receive(fd, &header, sizeof header);
	assert(header.opcode == CLIENT_OPEN_SEAT && header.size == 0);
	if (strcmp(mode, "silent-wire") == 0) {
		sleep(11);
		char byte;
		assert(read(fd, &byte, 1) == 0);
		return;
	}
	const char *name = wrong_seat ? "seatOther" : "seat-a_b";
	if (wrong_seat) {
		/* Even a queued enable must not execute before seat-name validation. */
		response(fd, SERVER_ENABLE_SEAT);
	}
	struct proto_server_seat_opened opened = {.seat_name_len = (uint16_t)(strlen(name) + 1)};
	header.opcode = SERVER_SEAT_OPENED;
	header.size = (uint16_t)(sizeof opened + opened.seat_name_len);
	send_bytes(fd, &header, sizeof header);
	send_bytes(fd, &opened, sizeof opened);
	send_bytes(fd, name, opened.seat_name_len);
	if (wrong_seat) {
		char byte;
		assert(read(fd, &byte, 1) == 0);
		return;
	}
	response(fd, SERVER_ENABLE_SEAT);
	for (;;) {
		receive(fd, &header, sizeof header);
		switch (header.opcode) {
		case CLIENT_OPEN_DEVICE: {
			struct proto_client_open_device request;
			receive(fd, &request, sizeof request);
			assert(header.size == sizeof request + request.path_len);
			assert(request.path_len <= MAX_PATH_LEN);
			char path[MAX_PATH_LEN];
			receive(fd, path, request.path_len);
			assert(strcmp(path, "/dev/null") == 0);
			int device = open("/dev/null", O_RDONLY | O_CLOEXEC);
			assert(device >= 0);
			struct proto_header reply_header = {
				.opcode = SERVER_DEVICE_OPENED, .size = sizeof(struct proto_server_device_opened)};
			struct proto_server_device_opened reply = {.device_id = 7};
			struct iovec iov[] = {{&reply_header, sizeof reply_header}, {&reply, sizeof reply}};
			union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
			memset(&control, 0, sizeof control);
			struct msghdr message = {.msg_iov = iov, .msg_iovlen = 2,
				.msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
			struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
			rights->cmsg_level = SOL_SOCKET;
			rights->cmsg_type = SCM_RIGHTS;
			rights->cmsg_len = CMSG_LEN(sizeof device);
			memcpy(CMSG_DATA(rights), &device, sizeof device);
			assert(sendmsg(fd, &message, 0) == (ssize_t)(sizeof reply_header + sizeof reply));
			close(device);
			break;
		}
		case CLIENT_CLOSE_DEVICE: {
			struct proto_client_close_device request;
			assert(header.size == sizeof request);
			receive(fd, &request, sizeof request);
			assert(request.device_id == 7);
			response(fd, SERVER_DEVICE_CLOSED);
			response(fd, SERVER_DISABLE_SEAT);
			break;
		}
		case CLIENT_DISABLE_SEAT:
			assert(header.size == 0);
			response(fd, SERVER_SEAT_DISABLED);
			break;
		case CLIENT_SWITCH_SESSION: {
			struct proto_client_switch_session request;
			assert(header.size == sizeof request);
			receive(fd, &request, sizeof request);
			assert(request.session == 2);
			response(fd, SERVER_SESSION_SWITCHED);
			break;
		}
		case CLIENT_PING:
			assert(header.size == 0);
			response(fd, SERVER_PONG);
			break;
		case CLIENT_CLOSE_SEAT:
			assert(header.size == 0);
			response(fd, SERVER_SEAT_CLOSED);
			return;
		default:
			abort();
		}
	}
}

static void mock_service(int ready, uint64_t cookie, const char *mode) {
	alarm(25);
	if (strcmp(mode, "nonroot-owner") == 0) {
		assert(setgid(65534) == 0 && setuid(65534) == 0);
	}
	/* Explicit address also after setuid: never allow libdbus's privileged-process
	 * environment filtering to substitute the real system bus in this fixture. */
	DBusConnection *bus = dbus_connection_open_private(getenv("DBUS_SESSION_BUS_ADDRESS"), NULL);
	assert(bus != NULL && dbus_bus_register(bus, NULL));
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	assert(dbus_bus_request_name(bus, SERVICE, DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL) ==
		DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
	send_bytes(ready, "R", 1);
	close(ready);
	DBusMessage *request = NULL;
	for (int i = 0; i < 200 && request == NULL; i++) {
		assert(dbus_connection_read_write(bus, 100));
		DBusMessage *message;
		while ((message = dbus_connection_pop_message(bus)) != NULL) {
			if (dbus_message_is_method_call(message, INTERFACE, "CreateConnection")) {
				assert(strcmp(dbus_message_get_path(message), PATH) == 0);
				request = message;
				break;
			}
			dbus_message_unref(message);
		}
		if (strcmp(mode, "nonroot-owner") == 0 && i == 10) {
			assert(request == NULL);
			dbus_connection_close(bus);
			dbus_connection_unref(bus);
			return;
		}
	}
	assert(request != NULL && dbus_message_has_signature(request, "h"));
	int authority = -1;
	assert(dbus_message_get_args(request, NULL, DBUS_TYPE_UNIX_FD, &authority, DBUS_TYPE_INVALID));
	uint64_t actual;
	socklen_t size = sizeof actual;
	assert(getsockopt(authority, SOL_SOCKET, SO_COOKIE, &actual, &size) == 0 && actual == cookie);
	close(authority);
	if (strcmp(mode, "owner-loss") == 0 || strcmp(mode, "service-timeout") == 0) {
		if (strcmp(mode, "service-timeout") == 0) {
			sleep(11);
			/* The ten-second client deadline must have closed its private bus,
			 * rather than merely waiting for our later disconnect. */
			DBusError error = DBUS_ERROR_INIT;
			dbus_bus_get_unix_user(bus, dbus_message_get_sender(request), &error);
			assert(dbus_error_is_set(&error));
			dbus_error_free(&error);
		}
		dbus_message_unref(request);
		dbus_connection_close(bus);
		dbus_connection_unref(bus);
		return;
	}
	DBusMessage *reply;
	if (strcmp(mode, "service-error") == 0) {
		reply = dbus_message_new_error(request, "org.SGLogin.Multiseatd1.Error.NotAuthorized", "fixture");
		assert(reply != NULL && dbus_connection_send(bus, reply, NULL));
		dbus_message_unref(reply);
		dbus_message_unref(request);
		dbus_connection_flush(bus);
	} else {
		int sockets[2];
		if (strcmp(mode, "bad-peer") == 0) {
			assert(setgid(65534) == 0 && setuid(65534) == 0);
		}
		assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
		int endpoint = sockets[1];
		if (strcmp(mode, "bad-endpoint") == 0) {
			close(endpoint);
			endpoint = open("/dev/null", O_RDONLY | O_CLOEXEC);
			assert(endpoint >= 0);
		}
		reply = dbus_message_new_method_return(request);
		dbus_uint32_t handle = strcmp(mode, "bad-handle") == 0 ? UINT32_MAX : (dbus_uint32_t)sockets[0];
		assert(reply != NULL);
		assert(dbus_message_append_args(reply, DBUS_TYPE_UNIX_FD, &endpoint,
			DBUS_TYPE_UINT32, &handle, DBUS_TYPE_INVALID));
		if (strcmp(mode, "bad-signature") == 0) {
			assert(dbus_message_append_args(reply, DBUS_TYPE_UINT32, &handle, DBUS_TYPE_INVALID));
		}
		assert(dbus_connection_send(bus, reply, NULL));
		dbus_message_unref(reply);
		dbus_message_unref(request);
		close(endpoint);
		dbus_connection_flush(bus);
		if (strcmp(mode, "good") == 0 || strcmp(mode, "wrong-seat") == 0 ||
		    strcmp(mode, "silent-wire") == 0) {
			wire_server(sockets[0], mode);
		} else {
			char byte;
			assert(read(sockets[0], &byte, 1) == 0);
		}
		close(sockets[0]);
	}
	dbus_connection_close(bus);
	dbus_connection_unref(bus);
}

struct events { int enabled; int disabled; };

static void enable(struct libseat *seat, void *data) {
	(void)seat;
	((struct events *)data)->enabled++;
}

static void disable(struct libseat *seat, void *data) {
	((struct events *)data)->disabled++;
	assert(libseat_disable_seat(seat) == 0);
}

static void run_compositor(int authority, char **arguments) {
	char runtime[] = "/tmp/libseat-native.XXXXXX";
	assert(mkdtemp(runtime) != NULL && chown(runtime, 65534, 65534) == 0);
	struct stat metadata;
	assert(fstat(authority, &metadata) == 0);
	char inode[64];
	snprintf(inode, sizeof inode, "%llu", (unsigned long long)metadata.st_ino);
	pid_t parent = getpid();
	pid_t child = fork();
	assert(child != -1);
	if (child == 0) {
		assert(setsid() != -1);
		assert(setgroups(0, NULL) == 0 && setgid(65534) == 0 && setuid(65534) == 0);
		assert(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() == parent);
		/* Only the deliberately delegated capability crosses compositor exec. */
		if (authority > 3) {
			assert(close_range(3, (unsigned)authority - 1, 0) == 0);
		}
		assert(close_range((unsigned)authority + 1, UINT_MAX, 0) == 0);
		assert(fcntl(authority, F_SETFD, 0) == 0);
		assert(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
		assert(setenv("LIBSEAT_TEST_AUTHORITY_INODE", inode, 1) == 0);
		assert(setenv("LD_PRELOAD", arguments[2], 1) == 0);
		assert(setenv("LD_LIBRARY_PATH", arguments[3], 1) == 0);
		assert(setenv("WLR_RENDERER", "pixman", 1) == 0);
		unsetenv("SEATD_FD");
		unsetenv("DISPLAY");
		unsetenv("WAYLAND_DISPLAY");
		execl(arguments[0], arguments[0], "--", arguments[1], NULL);
		_exit(127);
	}
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	char marker[sizeof runtime + 32];
	snprintf(marker, sizeof marker, "%s/native-ui-ok", runtime);
	int check = open(marker, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	assert(check >= 0 && fstat(check, &metadata) == 0 && metadata.st_uid == 65534);
	char result[2];
	receive(check, result, sizeof result);
	assert(memcmp(result, "ok", 2) == 0);
	close(check);
	assert(unlink(marker) == 0);
	/* Cage may leave its private lock file; no caller-owned path is removed. */
	char lock[sizeof runtime + 32];
	snprintf(lock, sizeof lock, "%s/wayland-0.lock", runtime);
	unlink(lock);
	assert(rmdir(runtime) == 0);
}

static void exercise(const char *mode, char **compositor) {
	int authority[2], ready[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, authority) == 0);
	int retained = fcntl(authority[0], F_DUPFD_CLOEXEC, 3);
	assert(retained >= 3);
	uint64_t cookie;
	socklen_t size = sizeof cookie;
	assert(getsockopt(authority[0], SOL_SOCKET, SO_COOKIE, &cookie, &size) == 0);
	assert(pipe2(ready, O_CLOEXEC) == 0);
	pid_t child = fork();
	assert(child != -1);
	if (child == 0) {
		close(ready[0]);
		close(authority[0]);
		close(authority[1]);
		close(retained);
		mock_service(ready[1], cookie, mode);
		_exit(0);
	}
	close(ready[1]);
	char byte;
	receive(ready[0], &byte, 1);
	close(ready[0]);
	char number[32];
	snprintf(number, sizeof number, "%d", authority[0]);
	assert(setenv("MULTISEATD_AUTHORITY_FD", number, 1) == 0);
	assert(setenv("XDG_SEAT", "seat-a_b", 1) == 0);
	struct events events = {0};
	struct libseat_seat_listener listener = {.enable_seat = enable, .disable_seat = disable};
	if (compositor != NULL) {
		run_compositor(authority[0], compositor);
		close(authority[0]);
	} else {
		struct libseat *seat = libseat_open_seat(&listener, &events);
		if (strcmp(mode, "silent-wire") == 0) {
			assert(seat == NULL && errno == ETIMEDOUT);
		}
		assert(getenv("MULTISEATD_AUTHORITY_FD") == NULL);
		assert(fcntl(authority[0], F_GETFD) == -1 && errno == EBADF);
		assert(fcntl(retained, F_GETFD) != -1);
		if (strcmp(mode, "good") == 0) {
			assert(seat != NULL && strcmp(libseat_seat_name(seat), "seat-a_b") == 0);
			int fd = libseat_get_fd(seat);
			assert(fd >= 3 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
			assert((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
			while (events.enabled == 0) {
				assert(libseat_dispatch(seat, 1000) >= 0);
			}
			int device = -1;
			assert(libseat_open_device(seat, "/dev/null", &device) == 7);
			assert(device >= 0);
			close(device);
			assert(libseat_switch_session(seat, 2) == 0);
			assert(libseat_close_device(seat, 7) == 0);
			while (events.disabled == 0) {
				assert(libseat_dispatch(seat, 1000) >= 0);
			}
			assert(libseat_close_seat(seat) == 0);
		} else {
			assert(seat == NULL && events.enabled == 0 && events.disabled == 0);
		}
	}
	close(retained);
	close(authority[1]);
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static size_t descriptor_count(void) {
	DIR *directory = opendir("/proc/self/fd");
	assert(directory != NULL);
	size_t count = 0;
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] != '.') {
			count++;
		}
	}
	closedir(directory);
	return count;
}

static void missing_owner(void) {
	int authority[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, authority) == 0);
	char number[32];
	snprintf(number, sizeof number, "%d", authority[0]);
	assert(setenv("MULTISEATD_AUTHORITY_FD", number, 1) == 0);
	assert(setenv("XDG_SEAT", "seatNoOwner", 1) == 0);
	struct events events = {0};
	struct libseat_seat_listener listener = {.enable_seat = enable, .disable_seat = disable};
	assert(libseat_open_seat(&listener, &events) == NULL);
	assert(events.enabled == 0 && events.disabled == 0);
	assert(getenv("MULTISEATD_AUTHORITY_FD") == NULL);
	assert(fcntl(authority[0], F_GETFD) == -1 && errno == EBADF);
	close(authority[1]);
}

int main(int argc, char **argv) {
	if (getuid() != 0) {
		return 77;
	}
	/* Run only inside dbus-run-session, never against the host system bus. */
	const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (address == NULL) {
		fputs("run this fixture with dbus-run-session and the private bus config\n", stderr);
		return 77;
	}
	assert(setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1) == 0);
	assert(setenv("LIBSEAT_BACKEND", "multiseatd", 1) == 0);
	alarm(60);
	if (argc == 5) {
		exercise("good", argv + 1);
		return 0;
	}
	assert(argc == 1);
	const char *modes[] = {"good", "wrong-seat", "bad-signature", "bad-endpoint", "bad-peer", "bad-handle",
		"service-error", "nonroot-owner", "owner-loss", "service-timeout", "silent-wire"};
	/* This process is single-threaded and each child is joined: no FD-count race. */
	size_t initial = descriptor_count();
	for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
		exercise(modes[i], NULL);
		assert(descriptor_count() == initial);
	}
	missing_owner();
	assert(descriptor_count() == initial);
	return 0;
}
