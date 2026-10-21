#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "drm.h"
#include "evdev.h"
#include "hidraw.h"
#include "linked_list.h"
#include "poller.h"
#include "seat.h"
#include "server.h"
#include "terminal.h"
#include "test.h"
#include "wscons.h"

static int current_vt;
static int release_acks;
static int acquire_acks;
static int disable_events;
static int enable_events;

int terminal_open(int vt) {
	(void)vt;
	return open("/dev/null", O_RDWR | O_CLOEXEC);
}

int terminal_set_process_switching(int fd, bool enable) {
	(void)fd;
	(void)enable;
	return 0;
}

int terminal_current_vt(int fd) {
	(void)fd;
	return current_vt;
}

int terminal_switch_vt(int fd, int vt) {
	(void)fd;
	current_vt = vt;
	return 0;
}

int terminal_ack_release(int fd) {
	(void)fd;
	release_acks++;
	return 0;
}

int terminal_ack_acquire(int fd) {
	(void)fd;
	acquire_acks++;
	return 0;
}

int terminal_set_keyboard(int fd, bool enable) {
	(void)fd;
	(void)enable;
	return 0;
}

int terminal_set_graphics(int fd, bool enable) {
	(void)fd;
	(void)enable;
	return 0;
}

int client_send_enable_seat(struct client *client) {
	(void)client;
	enable_events++;
	return 0;
}

int client_send_disable_seat(struct client *client) {
	(void)client;
	disable_events++;
	return 0;
}

void client_destroy(struct client *client) {
	(void)client;
	abort();
}

int drm_set_master(int fd) {
	(void)fd;
	return 0;
}

int drm_drop_master(int fd) {
	(void)fd;
	return 0;
}

int path_is_drm(const char *path) {
	(void)path;
	return 0;
}

int evdev_revoke(int fd) {
	(void)fd;
	return 0;
}

int path_is_evdev(const char *path) {
	(void)path;
	return 0;
}

int hidraw_revoke(int fd) {
	(void)fd;
	return 0;
}

int path_is_hidraw(const char *path) {
	(void)path;
	return 0;
}

int path_is_wscons(const char *path) {
	(void)path;
	return 0;
}

struct fixture {
	struct server server;
	struct seat *seat;
};

static void fixture_create(struct fixture *fixture) {
	memset(fixture, 0, sizeof(*fixture));
	test_assert(poller_init(&fixture->server.poller) == 0);
	linked_list_init(&fixture->server.seats);
	linked_list_init(&fixture->server.idle_clients);
	fixture->seat = seat_create("seat0", true);
	test_assert(fixture->seat != NULL);
	linked_list_insert(&fixture->server.seats, &fixture->seat->link);
	current_vt = 7;
	release_acks = 0;
	acquire_acks = 0;
	disable_events = 0;
	enable_events = 0;
}

static void fixture_add_client(struct fixture *fixture, struct client *client, int session) {
	memset(client, 0, sizeof(*client));
	client->server = &fixture->server;
	client->seat = fixture->seat;
	client->session = session;
	client->state = CLIENT_NEW;
	linked_list_init(&client->devices);
	linked_list_insert(&fixture->seat->clients, &client->link);
}

static void fixture_destroy(struct fixture *fixture) {
	while (!linked_list_empty(&fixture->seat->clients)) {
		struct client *client = (struct client *)fixture->seat->clients.next;
		seat_remove_client(client);
	}
	seat_destroy(fixture->seat);
	poller_finish(&fixture->server.poller);
}

static void test_release_without_client_is_immediate(void) {
	struct fixture fixture;
	fixture_create(&fixture);
	test_assert(seat_vt_release(fixture.seat) == 0);
	test_assert(release_acks == 1);
	test_assert(!fixture.seat->vt_release_pending);
	test_assert(fixture.seat->cur_vt == -1);
	fixture_destroy(&fixture);
}

static void test_release_waits_for_ack(void) {
	struct fixture fixture;
	fixture_create(&fixture);
	struct client client;
	fixture_add_client(&fixture, &client, 7);
	test_assert(seat_open_client(fixture.seat, &client) == 0);

	test_assert(seat_vt_release(fixture.seat) == 0);
	test_assert(disable_events == 1);
	test_assert(client.state == CLIENT_PENDING_DISABLE);
	test_assert(release_acks == 0);
	test_assert(fixture.seat->active_client == &client);

	test_assert(seat_ack_disable_client(&client) == 0);
	test_assert(client.state == CLIENT_DISABLED);
	test_assert(fixture.seat->active_client == NULL);
	test_assert(release_acks == 1);
	test_assert(!fixture.seat->vt_release_pending);

	test_assert(seat_ack_disable_client(&client) == -1);
	test_assert(release_acks == 1);
	fixture_destroy(&fixture);
}

static void test_disconnect_completes_release(void) {
	struct fixture fixture;
	fixture_create(&fixture);
	struct client client;
	fixture_add_client(&fixture, &client, 7);
	test_assert(seat_open_client(fixture.seat, &client) == 0);
	test_assert(seat_vt_release(fixture.seat) == 0);
	test_assert(release_acks == 0);

	seat_remove_client(&client);
	test_assert(client.seat == NULL);
	test_assert(release_acks == 1);
	test_assert(!fixture.seat->vt_release_pending);
	fixture_destroy(&fixture);
}

static void test_timeout_forces_release(void) {
	struct fixture fixture;
	fixture_create(&fixture);
	struct client client;
	fixture_add_client(&fixture, &client, 7);
	test_assert(seat_open_client(fixture.seat, &client) == 0);
	test_assert(seat_vt_release(fixture.seat) == 0);
	test_assert(release_acks == 0);

	test_assert(poller_poll(&fixture.server.poller) == 0);
	test_assert(release_acks == 1);
	test_assert(client.state == CLIENT_DISABLED);
	test_assert(fixture.seat->active_client == NULL);
	test_assert(!fixture.seat->vt_release_pending);

	test_assert(seat_ack_disable_client(&client) == -1);
	test_assert(release_acks == 1);
	fixture_destroy(&fixture);
}

static void test_tty7_to_tty8_handoff_order(void) {
	struct fixture fixture;
	fixture_create(&fixture);
	struct client greeter;
	struct client session;
	fixture_add_client(&fixture, &greeter, 7);
	test_assert(seat_open_client(fixture.seat, &greeter) == 0);

	test_assert(seat_vt_release(fixture.seat) == 0);
	test_assert(disable_events == 1);
	test_assert(release_acks == 0);
	test_assert(seat_ack_disable_client(&greeter) == 0);
	test_assert(release_acks == 1);
	test_assert(fixture.seat->active_client == NULL);

	current_vt = 8;
	fixture_add_client(&fixture, &session, 8);
	test_assert(seat_vt_activate(fixture.seat) == 0);
	test_assert(acquire_acks == 1);
	test_assert(fixture.seat->active_client == &session);
	test_assert(session.state == CLIENT_ACTIVE);
	test_assert(enable_events == 2);
	fixture_destroy(&fixture);
}

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	test_run(test_release_without_client_is_immediate);
	test_run(test_release_waits_for_ack);
	test_run(test_disconnect_completes_release);
	test_run(test_timeout_forces_release);
	test_run(test_tty7_to_tty8_handoff_order);
}
