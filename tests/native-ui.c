#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

int main(void) {
	assert(getuid() == 65534 && geteuid() == 65534);
	assert(getenv("MULTISEATD_AUTHORITY_FD") == NULL && getenv("SEATD_FD") == NULL);
	assert(getenv("LIBSEAT_TEST_AUTHORITY_INODE") != NULL);
	assert(getenv("LIBSEAT_TEST_CONNECTION_INODE") != NULL);
	unsigned long long authority = strtoull(getenv("LIBSEAT_TEST_AUTHORITY_INODE"), NULL, 10);
	unsigned long long connection = strtoull(getenv("LIBSEAT_TEST_CONNECTION_INODE"), NULL, 10);
	assert(authority != 0 && connection != 0);
	DIR *directory = opendir("/proc/self/fd");
	assert(directory != NULL);
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		char *end;
		long fd = strtol(entry->d_name, &end, 10);
		if (*entry->d_name == 0 || *end != 0) {
			continue;
		}
		struct stat metadata;
		assert(fstat((int)fd, &metadata) == 0);
		assert((unsigned long long)metadata.st_ino != authority);
		assert((unsigned long long)metadata.st_ino != connection);
	}
	closedir(directory);
	assert(open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC) == -1);
	struct wl_display *display = wl_display_connect(NULL);
	assert(display != NULL && wl_display_roundtrip(display) >= 0);
	wl_display_disconnect(display);
	char path[256];
	assert(snprintf(path, sizeof path, "%s/native-ui-ok", getenv("XDG_RUNTIME_DIR")) < (int)sizeof path);
	int marker = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	assert(marker >= 0 && write(marker, "ok", 2) == 2);
	close(marker);
	puts("native Cage: Wayland roundtrip and UI FD/authority/terminal isolation passed");
	return 0;
}
