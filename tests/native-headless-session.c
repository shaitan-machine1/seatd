/* Optional compositor feasibility fixture only; never installed. Real
 * wlroots/libseat session initialization, disposable SHM output, no device I/O. */
#define _GNU_SOURCE
#define WLR_USE_UNSTABLE
#include <libseat.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/session.h>

struct wlr_backend *wlr_backend_autocreate(struct wl_event_loop *loop, struct wlr_session **session) {
	if (getenv("MULTISEATD_AUTHORITY_FD") == NULL || getenv("SEATD_FD") != NULL) {
		return NULL;
	}
	*session = wlr_session_create(loop);
	if (*session == NULL || getenv("MULTISEATD_AUTHORITY_FD") != NULL) {
		return NULL;
	}
	struct stat metadata;
	if (fstat(libseat_get_fd((*session)->seat_handle), &metadata) == -1) {
		return NULL;
	}
	char inode[64];
	snprintf(inode, sizeof inode, "%llu", (unsigned long long)metadata.st_ino);
	if (setenv("LIBSEAT_TEST_CONNECTION_INODE", inode, 1) == -1) {
		return NULL;
	}
	struct wlr_backend *backend = wlr_headless_backend_create(loop);
	if (backend == NULL || wlr_headless_add_output(backend, 800, 600) == NULL) {
		return NULL;
	}
	return backend;
}
