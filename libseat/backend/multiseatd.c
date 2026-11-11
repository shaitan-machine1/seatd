#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "multiseatd.h"

#define SERVICE_PREFIX "org.SGLogin.Multiseatd1.Seat_"
#define MANAGER_PATH "/org/SGLogin/Multiseatd1"
#define MANAGER_INTERFACE "org.SGLogin.Multiseatd1.Manager"
#define BOOTSTRAP_TIMEOUT_MS 10000

int multiseatd_service_name(const char *seat, char *service, size_t size) {
	if (seat == NULL || strncmp(seat, "seat", 4) != 0 ||
	    strnlen(seat, MAX_SEAT_LEN) >= MAX_SEAT_LEN) {
		errno = EINVAL;
		return -1;
	}
	size_t length = sizeof(SERVICE_PREFIX) - 1;
	if (size <= length) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(service, SERVICE_PREFIX, length);
	const char hex[] = "0123456789abcdef";
	for (const unsigned char *p = (const unsigned char *)seat; *p != 0; p++) {
		bool alphanumeric = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
				    (*p >= '0' && *p <= '9');
		if (!alphanumeric && *p != '_' && *p != '-') {
			errno = EINVAL;
			return -1;
		}
		size_t count = alphanumeric ? 1 : 3;
		if (count >= size - length) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (alphanumeric) {
			service[length++] = (char)*p;
		} else {
			service[length++] = '_';
			service[length++] = hex[*p >> 4];
			service[length++] = hex[*p & 15];
		}
	}
	service[length] = 0;
	return 0;
}

/* Validate before taking ownership: malformed environment must not close a
 * guessed unrelated descriptor. The daemon, not these checks, verifies authority. */
static int validate_socket(int fd) {
	int type;
	socklen_t size = sizeof type;
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == -1) {
		return -1;
	}
	if (type != SOCK_STREAM) {
		errno = ENOTSOCK;
		return -1;
	}
	struct sockaddr_storage address;
	size = sizeof address;
	if (getpeername(fd, (struct sockaddr *)&address, &size) == -1) {
		return -1;
	}
	if (address.ss_family != AF_UNIX) {
		errno = EAFNOSUPPORT;
		return -1;
	}
#ifdef __linux__
	struct ucred credentials;
	size = sizeof credentials;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == -1) {
		return -1;
	}
	uid_t uid = credentials.uid;
#else
	uid_t uid;
	gid_t gid;
	if (getpeereid(fd, &uid, &gid) == -1) {
		return -1;
	}
#endif
	if (uid != 0) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

static int consume_authority(void) {
	const char *value = getenv("MULTISEATD_AUTHORITY_FD");
	if (value == NULL) {
		errno = ENOENT;
		return -1;
	}
	bool digits = *value != 0;
	for (const char *p = value; *p != 0; p++) {
		digits = digits && *p >= '0' && *p <= '9';
	}
	errno = 0;
	char *end;
	long parsed = strtol(value, &end, 10);
	bool valid = digits && errno == 0 && *end == 0 && parsed >= 3 && parsed <= INT_MAX;
	if (unsetenv("MULTISEATD_AUTHORITY_FD") == -1) {
		return -1;
	}
	if (!valid) {
		errno = EINVAL;
		return -1;
	}
	int fd = (int)parsed;
	if (validate_socket(fd) == -1) {
		return -1;
	}
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

static int64_t monotonic_ms(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
		return -1;
	}
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static DBusMessage *call(DBusConnection *bus, const char *destination, const char *method,
			int type, const void *argument, int64_t deadline) {
	bool bus_method = strcmp(destination, DBUS_SERVICE_DBUS) == 0;
	DBusMessage *request = dbus_message_new_method_call(destination,
		bus_method ? DBUS_PATH_DBUS : MANAGER_PATH,
		bus_method ? DBUS_INTERFACE_DBUS : MANAGER_INTERFACE, method);
	if (request == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	dbus_message_set_auto_start(request, FALSE);
	if (!dbus_message_append_args(request, type, argument, DBUS_TYPE_INVALID)) {
		dbus_message_unref(request);
		errno = ENOMEM;
		return NULL;
	}
	int64_t now = monotonic_ms();
	int64_t remaining = deadline - now;
	if (now == -1 || remaining <= 0) {
		dbus_message_unref(request);
		errno = ETIMEDOUT;
		return NULL;
	}
	DBusError error = DBUS_ERROR_INIT;
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, request,
		(int)remaining, &error);
	dbus_message_unref(request);
	if (reply == NULL) {
		log_errorf("multiseatd bootstrap %s failed: %s", method,
			error.name != NULL ? error.name : "no reply");
		dbus_error_free(&error);
		errno = EIO;
		return NULL;
	}
	const char *sender = dbus_message_get_sender(reply);
	if (sender == NULL || strcmp(sender, destination) != 0) {
		dbus_message_unref(reply);
		errno = EACCES;
		return NULL;
	}
	return reply;
}

int multiseatd_connect(char seat[MAX_SEAT_LEN]) {
	int authority = consume_authority();
	if (authority == -1) {
		return -1;
	}
	int result = -1;
	int endpoint = -1;
	DBusConnection *bus = NULL;
	DBusMessage *reply = NULL;
	char *owner = NULL;
	char service[sizeof(SERVICE_PREFIX) + 3 * (MAX_SEAT_LEN - 1)];
	const char *environment_seat = getenv("XDG_SEAT");
	if (multiseatd_service_name(environment_seat, service, sizeof service) == -1) {
		goto done;
	}
	strcpy(seat, environment_seat);
	int64_t now = monotonic_ms();
	if (now == -1) {
		goto done;
	}
	int64_t deadline = now + BOOTSTRAP_TIMEOUT_MS;
	DBusError error = DBUS_ERROR_INIT;
	/* Private connection: never borrow, close or dispatch an application's bus.
	 * libdbus bounds its initial Hello; all subsequent RPCs share this deadline. */
	bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (bus == NULL) {
		dbus_error_free(&error);
		errno = ENOTCONN;
		goto done;
	}
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	const char *name = service;
	reply = call(bus, DBUS_SERVICE_DBUS, "GetNameOwner", DBUS_TYPE_STRING, &name, deadline);
	const char *resolved = NULL;
	if (reply == NULL) {
		goto done;
	}
	if (!dbus_message_has_signature(reply, "s") ||
	    !dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &resolved, DBUS_TYPE_INVALID) ||
	    resolved[0] != ':' || (owner = strdup(resolved)) == NULL) {
		errno = EBADMSG;
		goto done;
	}
	dbus_message_unref(reply);
	reply = call(bus, DBUS_SERVICE_DBUS, "GetConnectionUnixUser", DBUS_TYPE_STRING, &owner,
		deadline);
	dbus_uint32_t uid;
	if (reply == NULL) {
		goto done;
	}
	if (!dbus_message_has_signature(reply, "u") ||
	    !dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &uid, DBUS_TYPE_INVALID) || uid != 0) {
		errno = EACCES;
		goto done;
	}
	dbus_message_unref(reply);
	reply = call(bus, owner, "CreateConnection", DBUS_TYPE_UNIX_FD, &authority, deadline);
	dbus_uint32_t handle;
	if (reply == NULL) {
		goto done;
	}
	if (!dbus_message_has_signature(reply, "hu") ||
	    !dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &endpoint,
		DBUS_TYPE_UINT32, &handle, DBUS_TYPE_INVALID) || handle > INT_MAX) {
		errno = EBADMSG;
		goto done;
	}
	if (validate_socket(endpoint) == -1) {
		goto done;
	}
	/* get_args gives us an owned duplicate, separate from the reply's copy. */
	result = fcntl(endpoint, F_DUPFD_CLOEXEC, 3);
	if (result == -1) {
		goto done;
	}
	int flags = fcntl(result, F_GETFL);
	if (flags == -1 || fcntl(result, F_SETFL, flags | O_NONBLOCK) == -1) {
		int saved = errno;
		close(result);
		result = -1;
		errno = saved;
	}

done:;
	int saved = errno;
	if (endpoint != -1) {
		close(endpoint);
	}
	if (reply != NULL) {
		dbus_message_unref(reply);
	}
	if (bus != NULL) {
		dbus_connection_close(bus);
		dbus_connection_unref(bus);
	}
	free(owner);
	close(authority);
	errno = saved;
	return result;
}
