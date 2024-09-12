// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020 Google LLC
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"

#include "adapter.h"
#include "btd.h"
#include "dbus-common.h"
#include "device.h"
#include "log.h"
#include "src/error.h"
#include "src/shared/mgmt.h"
#include "src/shared/queue.h"
#include "src/shared/timeout.h"
#include "src/shared/util.h"

#include "adv_monitor.h"

#define ADV_MONITOR_INTERFACE		"org.bluez.AdvertisementMonitor1"
#define ADV_MONITOR_MGR_INTERFACE	"org.bluez.AdvertisementMonitorManager1"

#define ADV_MONITOR_UNSET_RSSI		127	/* dBm */
#define ADV_MONITOR_MAX_RSSI		20	/* dBm */
#define ADV_MONITOR_MIN_RSSI		-127	/* dBm */
#define ADV_MONITOR_UNSET_TIMEOUT	0	/* second */
#define ADV_MONITOR_MIN_TIMEOUT		1	/* second */
#define ADV_MONITOR_MAX_TIMEOUT		300	/* second */
#define ADV_MONITOR_DEFAULT_LOW_TIMEOUT	5	/* second */
#define ADV_MONITOR_DEFAULT_HIGH_TIMEOUT 10	/* second */
#define ADV_MONITOR_UNSET_SAMPLING_PERIOD 256	/* 100 ms */
#define ADV_MONITOR_MAX_SAMPLING_PERIOD	255	/* 100 ms */

struct btd_adv_monitor_manager {
	struct btd_adapter *adapter;
	struct mgmt *mgmt;
	uint16_t adapter_id;

	uint32_t supported_features;	/* MGMT_ADV_MONITOR_FEATURE_MASK_* */
	uint32_t enabled_features;	/* MGMT_ADV_MONITOR_FEATURE_MASK_* */
	uint16_t max_num_monitors;
	uint8_t max_num_patterns;

	struct queue *apps;	/* apps who registered for Adv monitoring */
	struct queue *merged_patterns;
};

struct adv_monitor_app {
	struct btd_adv_monitor_manager *manager;
	char *owner;
	char *path;

	DBusMessage *reg;
	GDBusClient *client;

	struct queue *monitors;
};

enum monitor_type {
	MONITOR_TYPE_NONE,
	MONITOR_TYPE_OR_PATTERNS,
};

enum monitor_state {
	MONITOR_STATE_NEW,	/* New but not yet init'ed with actual values */
	MONITOR_STATE_FAILED,	/* Failed to be init'ed */
	MONITOR_STATE_INITED,	/* Init'ed but not yet sent to kernel */
	MONITOR_STATE_ACTIVE,	/* Accepted by kernel */
	MONITOR_STATE_REMOVED,	/* Removed from kernel */
	MONITOR_STATE_RELEASED,	/* Dbus Object removed by app */
};

enum merged_pattern_state {
	MERGED_PATTERN_STATE_ADDING,	/* Adding pattern to kernel */
	MERGED_PATTERN_STATE_REMOVING,	/* Removing pattern from kernel */
	MERGED_PATTERN_STATE_STABLE,	/* Idle */
};

struct rssi_parameters {
	int8_t high_rssi;		/* High RSSI threshold */
	uint16_t high_rssi_timeout;	/* High RSSI threshold timeout */
	int8_t low_rssi;		/* Low RSSI threshold */
	uint16_t low_rssi_timeout;	/* Low RSSI threshold timeout */
	uint16_t sampling_period;	/* Merge packets in the same timeslot.
					 * Currenly unimplemented in user space.
					 * Used only to pass data to kernel.
					 */
};

struct adv_monitor {
	struct adv_monitor_app *app;
	GDBusProxy *proxy;
	char *path;

	enum monitor_state state;	/* MONITOR_STATE_* */

	struct rssi_parameters rssi;	/* RSSI parameter for this monitor */
	struct adv_monitor_merged_pattern *merged_pattern;

	struct queue *devices;		/* List of adv_monitor_device objects */
};

/* Some chipsets doesn't support multiple monitors with the same pattern.
 * To solve that and to generally ease their task, we merge monitors with the
 * same pattern, so those monitors will only be sent once to the kernel.
 */
struct adv_monitor_merged_pattern {
	struct btd_adv_monitor_manager *manager;
	uint16_t monitor_handle;	/* Kernel Monitor Handle */
	struct rssi_parameters rssi;	/* Merged RSSI parameter for |monitors|,
					 * this will be sent to the kernel.
					 */
	struct queue *monitors;		/* List of adv_monitor objects which
					 * have this pattern
					 */
	enum monitor_type type;		/* MONITOR_TYPE_* */
	struct queue *patterns;		/* List of bt_ad_pattern objects */
	enum merged_pattern_state current_state; /* MERGED_PATTERN_STATE_* */
	enum merged_pattern_state next_state;	 /* MERGED_PATTERN_STATE_* */
};

/* Some data like last_seen, timer/timeout values need to be maintained
 * per device. struct adv_monitor_device maintains such data.
 */
struct adv_monitor_device {
	struct adv_monitor *monitor;
	struct btd_device *device;

	time_t high_rssi_first_seen;	/* Start time when RSSI climbs above
					 * the high RSSI threshold
					 */
	time_t low_rssi_first_seen;	/* Start time when RSSI drops below
					 * the low RSSI threshold
					 */
	time_t last_seen;		/* Time when last Adv was received */
	bool found;			/* State of the device - lost/found */
	unsigned int lost_timer;	/* Timer to track if the device goes
					 * offline/out-of-range
					 */
};

struct app_match_data {
	const char *owner;
	const char *path;
};

struct adv_content_filter_info {
	struct bt_ad *ad;
	struct queue *matched_monitors;	/* List of matched monitors */
};

struct adv_rssi_filter_info {
	struct btd_device *device;
	int8_t rssi;
};

struct monitored_device_info {
	uint16_t monitor_handle;	/* Kernel Monitor Handle */
	struct btd_device *device;
};

static void monitor_device_free(void *data);
static void adv_monitor_filter_rssi(struct adv_monitor *monitor,
					struct btd_device *device, int8_t rssi);

static void merged_pattern_send_add(
			struct adv_monitor_merged_pattern *merged_pattern);
static void merged_pattern_send_remove(
			struct adv_monitor_merged_pattern *merged_pattern);

const struct adv_monitor_type {
	enum monitor_type type;
	const char *name;
} supported_types[] = {
	{ MONITOR_TYPE_OR_PATTERNS, "or_patterns" },
	{ },
};

static void rssi_unset(struct rssi_parameters *rssi)
{
	rssi->high_rssi = ADV_MONITOR_UNSET_RSSI;
	rssi->high_rssi_timeout = ADV_MONITOR_UNSET_TIMEOUT;
	rssi->low_rssi = ADV_MONITOR_UNSET_RSSI;
	rssi->low_rssi_timeout = ADV_MONITOR_UNSET_TIMEOUT;
	rssi->sampling_period = ADV_MONITOR_UNSET_SAMPLING_PERIOD;
}

static bool rssi_is_unset(const struct rssi_parameters *rssi)
{
	return rssi->high_rssi == ADV_MONITOR_UNSET_RSSI &&
		rssi->low_rssi == ADV_MONITOR_UNSET_RSSI &&
		rssi->high_rssi_timeout == ADV_MONITOR_UNSET_TIMEOUT &&
		rssi->low_rssi_timeout == ADV_MONITOR_UNSET_TIMEOUT &&
		rssi->sampling_period == ADV_MONITOR_UNSET_SAMPLING_PERIOD;
}

/* Replies to an app's D-Bus message and unref it */
static void app_reply_msg(struct adv_monitor_app *app, DBusMessage *reply)
{
	if (!app || !app->reg || !reply)
		return;

	g_dbus_send_message(btd_get_dbus_connection(), reply);
	dbus_message_unref(app->reg);
	app->reg = NULL;
}

/* Frees a pattern */
static void pattern_free(void *data)
{
	struct bt_ad_pattern *pattern = data;

	free(pattern);
}

static void merged_pattern_free(void *data)
{
	struct adv_monitor_merged_pattern *merged_pattern = data;

	queue_destroy(merged_pattern->patterns, pattern_free);
	queue_destroy(merged_pattern->monitors, NULL);

	if (merged_pattern->manager)
		queue_remove(merged_pattern->manager->merged_patterns,
							merged_pattern);
	free(merged_pattern);
}

/* Returns the smaller of the two integers |a| and |b| which is not equal to the
 * |unset| value. If both are unset, return unset.
 */
static int get_smaller_not_unset(int a, int b, int unset)
{
	if (a == unset)
		return b;
	if (b == unset)
		return a;

	return a < b ? a : b;
}

/* Merges two RSSI parameters, return the result. The result is chosen to be
 * whichever is more lenient of the two inputs, so we can pass that to the
 * kernel and still do additional filtering in the user space without loss of
 * information while still receiving benefit from offloading some filtering to
 * the hardware.
 * It is allowed for |a|, |b|, and |merged| to point to the same object.
 */
static void merge_rssi(const struct rssi_parameters *a,
			const struct rssi_parameters *b,
			struct rssi_parameters *merged)
{
	/* For low rssi, low_timeout, and high_rssi, choose the minimum of the
	 * two values. Filtering the higher values is done on userspace.
	 */
	merged->low_rssi = get_smaller_not_unset(a->low_rssi, b->low_rssi,
						ADV_MONITOR_UNSET_RSSI);
	merged->high_rssi = get_smaller_not_unset(a->high_rssi, b->high_rssi,
						ADV_MONITOR_UNSET_RSSI);
	merged->low_rssi_timeout = get_smaller_not_unset(a->low_rssi_timeout,
						b->low_rssi_timeout,
						ADV_MONITOR_UNSET_TIMEOUT);

	/* High timeout doesn't matter for now, it will be zeroed when it is
	 * forwarded to kernel anyway.
	 */
	merged->high_rssi_timeout = 0;

	/* Sampling period is not implemented yet in userspace. There is no
	 * good value if the two values are different, so just choose 0 for
	 * always reporting, to avoid missing packets.
	 */
	if (a->sampling_period != b->sampling_period)
		merged->sampling_period = 0;
	else
		merged->sampling_period = a->sampling_period;
}

/* Two merged_pattern are considered equal if all the following are true:
 * (1) both has the same monitor_type
 * (2) both has exactly the same pattern in the same order
 * Therefore, patterns A+B and B+A are considered different, as well as patterns
 * A and A+A. This shouldn't cause any issue, but solving this issue is a
 * potential improvement.
 */
static bool merged_pattern_is_equal(const void *data, const void *match_data)
{
	const struct adv_monitor_merged_pattern *a = data;
	const struct adv_monitor_merged_pattern *b = match_data;
	const struct queue_entry *a_entry, *b_entry;
	struct bt_ad_pattern *a_data, *b_data;

	if (a->type != b->type)
		return false;

	if (queue_length(a->patterns) != queue_length(b->patterns))
		return false;

	a_entry = queue_get_entries(a->patterns);
	b_entry = queue_get_entries(b->patterns);

	while (a_entry) {
		a_data = a_entry->data;
		b_data = b_entry->data;

		if (a_data->type != b_data->type ||
		    a_data->offset != b_data->offset ||
		    a_data->len != b_data->len ||
		    memcmp(a_data->data, b_data->data, a_data->len) != 0)
			return false;

		a_entry = a_entry->next;
		b_entry = b_entry->next;
	}

	return true;
}

static char *get_merged_pattern_state_name(enum merged_pattern_state state)
{
	switch (state) {
	case MERGED_PATTERN_STATE_ADDING:
		return "Adding";
	case MERGED_PATTERN_STATE_REMOVING:
		return "Removing";
	case MERGED_PATTERN_STATE_STABLE:
		return "Stable";
	}

	return NULL;
}

/* Adds a new merged pattern */
static void merged_pattern_add(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	/* This is only called when no merged_pattern found. Therefore, the
	 * state must be stable.
	 */
	if (merged_pattern->current_state != MERGED_PATTERN_STATE_STABLE) {
		btd_error(merged_pattern->manager->adapter_id,
			"Add merged_pattern request when state is not stable");
		return;
	}

	merged_pattern->current_state = MERGED_PATTERN_STATE_ADDING;
	merged_pattern_send_add(merged_pattern);

	DBG("Monitor state: %s -> %s",
		get_merged_pattern_state_name(merged_pattern->current_state),
		get_merged_pattern_state_name(merged_pattern->next_state));
}

/* Removes merged pattern, or queues for removal if busy */
static void merged_pattern_remove(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	rssi_unset(&merged_pattern->rssi);

	/* If we currently are removing, cancel subsequent ADD command if any */
	if (merged_pattern->current_state == MERGED_PATTERN_STATE_REMOVING) {
		merged_pattern->next_state = MERGED_PATTERN_STATE_STABLE;
		goto print_state;
	}

	/* If stable, we can proceed with removal right away */
	if (merged_pattern->current_state == MERGED_PATTERN_STATE_STABLE) {
		merged_pattern->current_state = MERGED_PATTERN_STATE_REMOVING;
		merged_pattern_send_remove(merged_pattern);
	} else {
		/* otherwise queue the removal */
		merged_pattern->next_state = MERGED_PATTERN_STATE_REMOVING;
	}

print_state:
	DBG("Monitor state: %s -> %s",
		get_merged_pattern_state_name(merged_pattern->current_state),
		get_merged_pattern_state_name(merged_pattern->next_state));
}

/* Replaces (removes and re-adds) merged pattern, or queues it if busy */
static void merged_pattern_replace(
			struct adv_monitor_merged_pattern *merged_pattern,
			const struct rssi_parameters *rssi)
{
	/* If the RSSI are the same then nothing needs to be done, except on
	 * the case where pattern is being removed. In that case, we need to
	 * re-add the pattern.
	 * high_rssi_timeout is purposedly left out in the comparison since
	 * the value is ignored upon submission to kernel.
	 */
	if (merged_pattern->rssi.high_rssi == rssi->high_rssi &&
	    merged_pattern->rssi.low_rssi == rssi->low_rssi &&
	    merged_pattern->rssi.low_rssi_timeout == rssi->low_rssi_timeout &&
	    merged_pattern->rssi.sampling_period == rssi->sampling_period &&
	    merged_pattern->current_state != MERGED_PATTERN_STATE_REMOVING &&
	    merged_pattern->next_state != MERGED_PATTERN_STATE_REMOVING)
		return;

	merged_pattern->rssi = *rssi;

	/* If stable, we can proceed with replacement. */
	if (merged_pattern->current_state == MERGED_PATTERN_STATE_STABLE) {
		/* Replacement is done by first removing, then re-adding */
		merged_pattern->current_state = MERGED_PATTERN_STATE_REMOVING;
		merged_pattern->next_state = MERGED_PATTERN_STATE_ADDING;
		merged_pattern_send_remove(merged_pattern);
	} else {
		/* otherwise queue the replacement */
		merged_pattern->next_state = MERGED_PATTERN_STATE_ADDING;
	}

	DBG("Monitor state: %s -> %s",
		get_merged_pattern_state_name(merged_pattern->current_state),
		get_merged_pattern_state_name(merged_pattern->next_state));
}

/* Current_state of merged_pattern is done, proceed to the next_state */
static void merged_pattern_process_next_step(
					struct adv_monitor_merged_pattern *mp)
{
	if (mp->current_state == MERGED_PATTERN_STATE_STABLE) {
		btd_error(mp->manager->adapter_id,
				"Merged pattern invalid current state");
		return;
	}

	if (mp->current_state == MERGED_PATTERN_STATE_REMOVING) {
		/* We might need to follow-up with re-adding the pattern */
		if (mp->next_state == MERGED_PATTERN_STATE_ADDING) {
			mp->current_state = MERGED_PATTERN_STATE_ADDING;
			mp->next_state = MERGED_PATTERN_STATE_STABLE;
			merged_pattern_send_add(mp);
			goto print_state;
		}

		/* We should never end up with remove-remove sequence */
		if (mp->next_state == MERGED_PATTERN_STATE_REMOVING)
			btd_error(mp->manager->adapter_id,
				"Merged pattern can't be removed again");

		/* No more operations */
		mp->current_state = MERGED_PATTERN_STATE_STABLE;
		mp->next_state = MERGED_PATTERN_STATE_STABLE;
		goto print_state;
	}

	/* current_state == MERGED_PATTERN_STATE_ADDING */
	if (mp->next_state == MERGED_PATTERN_STATE_REMOVING) {
		mp->current_state = MERGED_PATTERN_STATE_REMOVING;
		mp->next_state = MERGED_PATTERN_STATE_STABLE;
		merged_pattern_send_remove(mp);
		goto print_state;
	} else if (mp->next_state == MERGED_PATTERN_STATE_ADDING) {
		/* To re-add a just added pattern, we need to remove it first */
		mp->current_state = MERGED_PATTERN_STATE_REMOVING;
		mp->next_state = MERGED_PATTERN_STATE_ADDING;
		merged_pattern_send_remove(mp);
		goto print_state;
	}

	/* No more operations */
	mp->current_state = MERGED_PATTERN_STATE_STABLE;
	mp->next_state = MERGED_PATTERN_STATE_STABLE;

print_state:
	DBG("Monitor state: %s -> %s",
			get_merged_pattern_state_name(mp->current_state),
			get_merged_pattern_state_name(mp->next_state));
}

/* Frees a monitor object */
static void monitor_free(struct adv_monitor *monitor)
{
	g_dbus_proxy_unref(monitor->proxy);
	g_free(monitor->path);

	queue_destroy(monitor->devices, monitor_device_free);
	monitor->devices = NULL;

	free(monitor);
}

/* Calls Release() method of the remote Adv Monitor */
static void monitor_release(struct adv_monitor *monitor)
{
	/* Release() method on a monitor can be called when -
	 * 1. monitor initialization failed
	 * 2. app calls UnregisterMonitor and monitors held by app are released,
	 *    it may or may not be activated at this point
	 * 3. monitor is removed by kernel
	 */
	if (monitor->state != MONITOR_STATE_FAILED &&
	    monitor->state != MONITOR_STATE_INITED &&
	    monitor->state != MONITOR_STATE_ACTIVE &&
	    monitor->state != MONITOR_STATE_REMOVED) {
		return;
	}

	DBG("Calling Release() on Adv Monitor of owner %s at path %s",
		monitor->app->owner, monitor->path);

	g_dbus_proxy_method_call(monitor->proxy, "Release", NULL, NULL, NULL,
					NULL);
}

/* Removes monitor from the merged_pattern. This would result in removing it
 * from the kernel if there is only one such monitor with that pattern.
 */
static void monitor_remove(struct adv_monitor *monitor)
{
	struct adv_monitor_app *app = monitor->app;
	uint16_t adapter_id = app->manager->adapter_id;
	struct adv_monitor_merged_pattern *merged_pattern;
	const struct queue_entry *e;
	struct rssi_parameters rssi;

	/* Monitor from kernel can be removed when -
	 * 1. monitor object is deleted by app - may or may not be activated
	 * 2. app is destroyed and monitors held by app are marked as released
	 */
	if (monitor->state != MONITOR_STATE_INITED &&
	    monitor->state != MONITOR_STATE_ACTIVE &&
	    monitor->state != MONITOR_STATE_RELEASED) {
		return;
	}

	monitor->state = MONITOR_STATE_REMOVED;

	if (!monitor->merged_pattern) {
		btd_error(adapter_id,
			"Merged_pattern not found when removing monitor");
		return;
	}

	merged_pattern = monitor->merged_pattern;
	monitor->merged_pattern = NULL;
	queue_remove(merged_pattern->monitors, monitor);

	/* No more monitors - just remove the pattern entirely */
	if (queue_length(merged_pattern->monitors) == 0) {
		merged_pattern_remove(merged_pattern);
		return;
	}

	/* Calculate the merge result of the RSSIs of the monitors with the
	 * same pattern, minus the monitor being removed.
	 */
	rssi_unset(&rssi);
	for (e = queue_get_entries(merged_pattern->monitors); e; e = e->next) {
		struct adv_monitor *m = e->data;

		merge_rssi(&rssi, &m->rssi, &rssi);
	}

	merged_pattern_replace(merged_pattern, &rssi);
}

/* Destroys monitor object */
static void monitor_destroy(void *data)
{
	struct adv_monitor *monitor = data;

	if (!monitor)
		return;

	queue_remove(monitor->app->monitors, monitor);

	monitor_release(monitor);
	monitor_remove(monitor);
	monitor_free(monitor);
}

/* Destroys an app object along with related D-Bus handlers */
static void app_destroy(void *data)
{
	struct adv_monitor_app *app = data;

	if (!app)
		return;

	DBG("Destroy Adv Monitor app %s at path %s", app->owner, app->path);

	queue_destroy(app->monitors, monitor_destroy);

	if (app->reg) {
		app_reply_msg(app, btd_error_failed(app->reg,
						"Adv Monitor app destroyed"));
	}

	if (app->client) {
		g_dbus_client_set_disconnect_watch(app->client, NULL, NULL);
		g_dbus_client_set_proxy_handlers(app->client, NULL, NULL, NULL,
							NULL);
		g_dbus_client_set_ready_watch(app->client, NULL, NULL);
		g_dbus_client_unref(app->client);
	}

	g_free(app->owner);
	g_free(app->path);

	free(app);
}

/* Updates monitor state to 'released' */
static void monitor_state_released(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;

	if (!monitor || (monitor->state != MONITOR_STATE_INITED
				&& monitor->state != MONITOR_STATE_ACTIVE))
		return;

	monitor->state = MONITOR_STATE_RELEASED;
}

/* Updates monitor state to 'active' */
static void monitor_state_active(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;

	if (!monitor || monitor->state != MONITOR_STATE_INITED)
		return;

	monitor->state = MONITOR_STATE_ACTIVE;

	DBG("Calling Activate() on Adv Monitor of owner %s at path %s",
		monitor->app->owner, monitor->path);

	g_dbus_proxy_method_call(monitor->proxy, "Activate", NULL,
				NULL, NULL, NULL);
}

/* Handles a D-Bus disconnection event of an app */
static void app_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct adv_monitor_app *app = user_data;

	if (!app) {
		error("Unexpected NULL app object upon app disconnect");
		return;
	}

	btd_info(app->manager->adapter_id,
			"Adv Monitor app %s disconnected from D-Bus",
			app->owner);

	if (queue_remove(app->manager->apps, app)) {
		queue_foreach(app->monitors, monitor_state_released, NULL);
		app_destroy(app);
	}
}

/* Handles the ready signal of Adv Monitor app */
static void app_ready_cb(GDBusClient *client, void *user_data)
{
	struct adv_monitor_app *app = user_data;
	uint16_t adapter_id = app->manager->adapter_id;

	btd_info(adapter_id, "Path %s reserved for Adv Monitor app %s",
			app->path, app->owner);

	app_reply_msg(app, dbus_message_new_method_return(app->reg));
}

/* Allocates an Adv Monitor */
static struct adv_monitor *monitor_new(struct adv_monitor_app *app,
						GDBusProxy *proxy)
{
	struct adv_monitor *monitor;

	if (!app || !proxy)
		return NULL;

	monitor = new0(struct adv_monitor, 1);
	if (!monitor)
		return NULL;

	monitor->app = app;
	monitor->proxy = g_dbus_proxy_ref(proxy);
	monitor->path = g_strdup(g_dbus_proxy_get_path(proxy));

	monitor->state = MONITOR_STATE_NEW;

	rssi_unset(&monitor->rssi);
	monitor->devices = queue_new();

	return monitor;
}

/* Matches a monitor based on its D-Bus path */
static bool monitor_match(const void *a, const void *b)
{
	const GDBusProxy *proxy = b;
	const struct adv_monitor *monitor = a;

	if (!proxy || !monitor)
		return false;

	if (g_strcmp0(g_dbus_proxy_get_path(proxy), monitor->path) != 0)
		return false;

	return true;
}

/* Retrieves Type from the remote Adv Monitor object, verifies the value and
 * update the local Adv Monitor
 */
static bool parse_monitor_type(struct adv_monitor *monitor, const char *path)
{
	DBusMessageIter iter;
	const struct adv_monitor_type *t;
	const char *type_str;
	uint16_t adapter_id = monitor->app->manager->adapter_id;

	if (!g_dbus_proxy_get_property(monitor->proxy, "Type", &iter)) {
		btd_error(adapter_id,
				"Failed to retrieve property Type from the "
				"Adv Monitor at path %s", path);
		return false;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		goto failed;

	dbus_message_iter_get_basic(&iter, &type_str);

	for (t = supported_types; t->name; t++) {
		if (strcmp(t->name, type_str) == 0) {
			monitor->merged_pattern->type = t->type;
			return true;
		}
	}

failed:
	btd_error(adapter_id,
			"Invalid argument of property Type of the Adv Monitor "
			"at path %s", path);

	return false;
}

/* Retrieves RSSI thresholds and timeouts from the remote Adv Monitor object,
 * verifies the values and update the local Adv Monitor
 */
static bool parse_rssi_and_timeout(struct adv_monitor *monitor,
					const char *path)
{
	DBusMessageIter iter;
	GDBusProxy *proxy = monitor->proxy;
	int16_t h_rssi = ADV_MONITOR_UNSET_RSSI;
	int16_t l_rssi = ADV_MONITOR_UNSET_RSSI;
	uint16_t h_rssi_timeout = ADV_MONITOR_UNSET_TIMEOUT;
	uint16_t l_rssi_timeout = ADV_MONITOR_UNSET_TIMEOUT;
	uint16_t sampling_period = ADV_MONITOR_UNSET_SAMPLING_PERIOD;
	uint16_t adapter_id = monitor->app->manager->adapter_id;

	/* Extract RSSIHighThreshold */
	if (g_dbus_proxy_get_property(proxy, "RSSIHighThreshold", &iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT16)
			goto failed;
		dbus_message_iter_get_basic(&iter, &h_rssi);
	}

	/* Extract RSSIHighTimeout */
	if (g_dbus_proxy_get_property(proxy, "RSSIHighTimeout", &iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT16)
			goto failed;
		dbus_message_iter_get_basic(&iter, &h_rssi_timeout);
	}

	/* Extract RSSILowThreshold */
	if (g_dbus_proxy_get_property(proxy, "RSSILowThreshold", &iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT16)
			goto failed;
		dbus_message_iter_get_basic(&iter, &l_rssi);
	}

	/* Extract RSSILowTimeout */
	if (g_dbus_proxy_get_property(proxy, "RSSILowTimeout", &iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT16)
			goto failed;
		dbus_message_iter_get_basic(&iter, &l_rssi_timeout);
	}

	/* Extract RSSISamplingPeriod */
	if (g_dbus_proxy_get_property(proxy, "RSSISamplingPeriod", &iter)) {
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT16)
			goto failed;
		dbus_message_iter_get_basic(&iter, &sampling_period);
	}

	/* Verify the values of RSSIs and their timeouts. All fields should be
	 * either set to the unset values or are set within valid ranges.
	 * If the fields are only partially set, we would try our best to fill
	 * in with some sane values.
	 */
	if (h_rssi == ADV_MONITOR_UNSET_RSSI &&
		l_rssi == ADV_MONITOR_UNSET_RSSI &&
		h_rssi_timeout == ADV_MONITOR_UNSET_TIMEOUT &&
		l_rssi_timeout == ADV_MONITOR_UNSET_TIMEOUT &&
		sampling_period == ADV_MONITOR_UNSET_SAMPLING_PERIOD) {
		goto done;
	}

	if (l_rssi == ADV_MONITOR_UNSET_RSSI)
		l_rssi = ADV_MONITOR_MIN_RSSI;

	if (h_rssi == ADV_MONITOR_UNSET_RSSI)
		h_rssi = l_rssi;

	if (l_rssi_timeout == ADV_MONITOR_UNSET_TIMEOUT)
		l_rssi_timeout = ADV_MONITOR_DEFAULT_LOW_TIMEOUT;

	if (h_rssi_timeout == ADV_MONITOR_UNSET_TIMEOUT)
		h_rssi_timeout = ADV_MONITOR_DEFAULT_HIGH_TIMEOUT;

	if (sampling_period == ADV_MONITOR_UNSET_SAMPLING_PERIOD)
		sampling_period = btd_opts.advmon.rssi_sampling_period;

	if (h_rssi < ADV_MONITOR_MIN_RSSI || h_rssi > ADV_MONITOR_MAX_RSSI ||
		l_rssi < ADV_MONITOR_MIN_RSSI ||
		l_rssi > ADV_MONITOR_MAX_RSSI || h_rssi < l_rssi) {
		goto failed;
	}

	if (h_rssi_timeout < ADV_MONITOR_MIN_TIMEOUT ||
		h_rssi_timeout > ADV_MONITOR_MAX_TIMEOUT ||
		l_rssi_timeout < ADV_MONITOR_MIN_TIMEOUT ||
		l_rssi_timeout > ADV_MONITOR_MAX_TIMEOUT) {
		goto failed;
	}

	if (sampling_period > ADV_MONITOR_MAX_SAMPLING_PERIOD)
		goto failed;

	monitor->rssi.high_rssi = h_rssi;
	monitor->rssi.low_rssi = l_rssi;
	monitor->rssi.high_rssi_timeout = h_rssi_timeout;
	monitor->rssi.low_rssi_timeout = l_rssi_timeout;
	monitor->rssi.sampling_period = sampling_period;

done:
	DBG("Adv Monitor at %s initiated with high RSSI threshold %d, high "
		"RSSI threshold timeout %d, low RSSI threshold %d, low RSSI "
		"threshold timeout %d, sampling period %d", path,
		monitor->rssi.high_rssi, monitor->rssi.high_rssi_timeout,
		monitor->rssi.low_rssi, monitor->rssi.low_rssi_timeout,
		monitor->rssi.sampling_period);

	monitor->merged_pattern->rssi = monitor->rssi;

	return true;

failed:
	btd_error(adapter_id,
			"Invalid argument of RSSI thresholds and timeouts "
			"of the Adv Monitor at path %s",
			path);

	return false;
}

/* Retrieves Patterns from the remote Adv Monitor object, verifies the values
 * and update the local Adv Monitor
 */
static bool parse_patterns(struct adv_monitor *monitor, const char *path)
{
	DBusMessageIter array, array_iter;
	uint16_t adapter_id = monitor->app->manager->adapter_id;

	if (!g_dbus_proxy_get_property(monitor->proxy, "Patterns", &array)) {
		btd_error(adapter_id,
				"Failed to retrieve property Patterns from the "
				"Adv Monitor at path %s", path);
		return false;
	}

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY ||
		dbus_message_iter_get_element_type(&array) !=
		DBUS_TYPE_STRUCT) {
		goto failed;
	}

	monitor->merged_pattern->patterns = queue_new();

	dbus_message_iter_recurse(&array, &array_iter);

	while (dbus_message_iter_get_arg_type(&array_iter) ==
		DBUS_TYPE_STRUCT) {
		int value_len;
		uint8_t *value;
		uint8_t offset, ad_type;
		struct bt_ad_pattern *pattern;
		DBusMessageIter struct_iter, value_iter;

		dbus_message_iter_recurse(&array_iter, &struct_iter);

		// Extract start position
		if (dbus_message_iter_get_arg_type(&struct_iter) !=
			DBUS_TYPE_BYTE) {
			goto failed;
		}
		dbus_message_iter_get_basic(&struct_iter, &offset);
		if (!dbus_message_iter_next(&struct_iter))
			goto failed;

		// Extract AD data type
		if (dbus_message_iter_get_arg_type(&struct_iter) !=
			DBUS_TYPE_BYTE) {
			goto failed;
		}
		dbus_message_iter_get_basic(&struct_iter, &ad_type);
		if (!dbus_message_iter_next(&struct_iter))
			goto failed;

		// Extract value of a pattern
		if (dbus_message_iter_get_arg_type(&struct_iter) !=
			DBUS_TYPE_ARRAY) {
			goto failed;
		}
		dbus_message_iter_recurse(&struct_iter, &value_iter);
		dbus_message_iter_get_fixed_array(&value_iter, &value,
							&value_len);

		pattern = bt_ad_pattern_new(ad_type, offset, value_len, value);
		if (!pattern)
			goto failed;

		queue_push_tail(monitor->merged_pattern->patterns, pattern);

		dbus_message_iter_next(&array_iter);
	}

	/* There must be at least one pattern. */
	if (queue_isempty(monitor->merged_pattern->patterns))
		goto failed;

	return true;

failed:
	btd_error(adapter_id, "Invalid argument of property Patterns of the "
			"Adv Monitor at path %s", path);

	return false;
}

/* Processes the content of the remote Adv Monitor */
static bool monitor_process(struct adv_monitor *monitor)
{
	const char *path = g_dbus_proxy_get_path(monitor->proxy);

	monitor->state = MONITOR_STATE_FAILED;

	monitor->merged_pattern = malloc0(sizeof(*monitor->merged_pattern));
	monitor->merged_pattern->current_state = MERGED_PATTERN_STATE_STABLE;
	monitor->merged_pattern->next_state = MERGED_PATTERN_STATE_STABLE;

	if (!parse_monitor_type(monitor, path))
		goto fail;

	if (!parse_rssi_and_timeout(monitor, path))
		goto fail;

	if (monitor->merged_pattern->type != MONITOR_TYPE_OR_PATTERNS ||
					!parse_patterns(monitor, path))
		goto fail;

	monitor->state = MONITOR_STATE_INITED;
	monitor->merged_pattern->monitors = queue_new();
	queue_push_tail(monitor->merged_pattern->monitors, monitor);

	return true;

fail:
	merged_pattern_free(monitor->merged_pattern);
	monitor->merged_pattern = NULL;
	return false;
}

static void merged_pattern_destroy_monitors(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	const struct queue_entry *e;

	for (e = queue_get_entries(merged_pattern->monitors); e; e = e->next) {
		struct adv_monitor *monitor = e->data;

		monitor->merged_pattern = NULL;
		monitor_destroy(monitor);
	}
}

/* Handles the callback of Remove Adv Monitor command */
static void remove_adv_monitor_cb(uint8_t status, uint16_t length,
				const void *param, void *user_data)
{
	const struct mgmt_rp_remove_adv_monitor *rp = param;
	struct adv_monitor_merged_pattern *merged_pattern = user_data;

	if (status != MGMT_STATUS_SUCCESS || !param) {
		error("Failed to Remove Adv Monitor with status 0x%02x",
				status);
		goto fail;
	}

	if (length < sizeof(*rp)) {
		error("Wrong size of Remove Adv Monitor response");
		goto fail;
	}

	DBG("Adv monitor with handle:0x%04x removed from kernel",
		le16_to_cpu(rp->monitor_handle));

	merged_pattern_process_next_step(merged_pattern);

	if (merged_pattern->current_state == MERGED_PATTERN_STATE_STABLE)
		merged_pattern_free(merged_pattern);

	return;

fail:
	merged_pattern_destroy_monitors(merged_pattern);
	merged_pattern_free(merged_pattern);
}

/* sends MGMT_OP_REMOVE_ADV_MONITOR */
static void merged_pattern_send_remove(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	struct mgmt_cp_remove_adv_monitor cp;
	struct btd_adv_monitor_manager *manager = merged_pattern->manager;

	cp.monitor_handle = cpu_to_le16(merged_pattern->monitor_handle);

	if (!mgmt_send(manager->mgmt, MGMT_OP_REMOVE_ADV_MONITOR,
			manager->adapter_id, sizeof(cp), &cp,
			remove_adv_monitor_cb, merged_pattern, NULL)) {
		btd_error(merged_pattern->manager->adapter_id,
				"Unable to send Remove Advt Monitor command");
	}
}

/* Handles the callback of Add Adv Patterns Monitor command */
static void add_adv_patterns_monitor_cb(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_add_adv_patterns_monitor *rp = param;
	struct adv_monitor_merged_pattern *merged_pattern = user_data;
	uint16_t adapter_id = merged_pattern->manager->adapter_id;

	if (status != MGMT_STATUS_SUCCESS || !param) {
		btd_error(adapter_id,
				"Failed to Add Adv Patterns Monitor with status"
				" 0x%02x", status);
		goto fail;
	}

	if (length < sizeof(*rp)) {
		btd_error(adapter_id, "Wrong size of Add Adv Patterns Monitor "
				"response");
		goto fail;
	}

	merged_pattern->monitor_handle = le16_to_cpu(rp->monitor_handle);
	DBG("Adv monitor with handle:0x%04x added",
						merged_pattern->monitor_handle);

	merged_pattern_process_next_step(merged_pattern);

	if (merged_pattern->current_state != MERGED_PATTERN_STATE_STABLE)
		return;

	queue_foreach(merged_pattern->monitors, monitor_state_active, NULL);

	return;

fail:
	merged_pattern_destroy_monitors(merged_pattern);
	merged_pattern_free(merged_pattern);
}

/* sends MGMT_OP_ADD_ADV_PATTERNS_MONITOR */
static bool merged_pattern_send_add_pattern(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	struct mgmt_cp_add_adv_monitor *cp = NULL;
	uint8_t pattern_count, cp_len;
	const struct queue_entry *e;
	bool success = true;

	pattern_count = queue_length(merged_pattern->patterns);
	cp_len = sizeof(*cp) + pattern_count * sizeof(struct mgmt_adv_pattern);

	cp = malloc0(cp_len);
	if (!cp)
		return false;

	for (e = queue_get_entries(merged_pattern->patterns); e; e = e->next) {
		struct bt_ad_pattern *pattern = e->data;

		memcpy(&cp->patterns[cp->pattern_count++], pattern,
							sizeof(*pattern));
	}

	if (!mgmt_send(merged_pattern->manager->mgmt,
			MGMT_OP_ADD_ADV_PATTERNS_MONITOR,
			merged_pattern->manager->adapter_id, cp_len, cp,
			add_adv_patterns_monitor_cb, merged_pattern, NULL)) {
		error("Unable to send Add Adv Patterns Monitor command");
		success = false;
	}

	free(cp);
	return success;
}

/* sends MGMT_OP_ADD_ADV_PATTERNS_MONITOR_RSSI */
static bool merged_pattern_send_add_pattern_rssi(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	struct mgmt_cp_add_adv_patterns_monitor_rssi *cp = NULL;
	uint8_t pattern_count, cp_len;
	const struct queue_entry *e;
	bool success = true;

	pattern_count = queue_length(merged_pattern->patterns);
	cp_len = sizeof(*cp) + pattern_count * sizeof(struct mgmt_adv_pattern);

	cp = malloc0(cp_len);
	if (!cp)
		return false;

	cp->rssi.high_threshold = merged_pattern->rssi.high_rssi;
	/* High threshold timeout is unsupported in kernel. Value must be 0. */
	cp->rssi.high_threshold_timeout = 0;
	cp->rssi.low_threshold = merged_pattern->rssi.low_rssi;
	cp->rssi.low_threshold_timeout =
				htobs(merged_pattern->rssi.low_rssi_timeout);
	cp->rssi.sampling_period = merged_pattern->rssi.sampling_period;

	for (e = queue_get_entries(merged_pattern->patterns); e; e = e->next) {
		struct bt_ad_pattern *pattern = e->data;

		memcpy(&cp->patterns[cp->pattern_count++], pattern,
							sizeof(*pattern));
	}

	if (!mgmt_send(merged_pattern->manager->mgmt,
			MGMT_OP_ADD_ADV_PATTERNS_MONITOR_RSSI,
			merged_pattern->manager->adapter_id, cp_len, cp,
			add_adv_patterns_monitor_cb, merged_pattern, NULL)) {
		error("Unable to send Add Adv Patterns Monitor RSSI command");
		success = false;
	}

	free(cp);
	return success;
}

/* Sends mgmt command to kernel for adding monitor */
static void merged_pattern_send_add(
			struct adv_monitor_merged_pattern *merged_pattern)
{
	if (rssi_is_unset(&merged_pattern->rssi))
		merged_pattern_send_add_pattern(merged_pattern);
	else
		merged_pattern_send_add_pattern_rssi(merged_pattern);
}

/* Handles an Adv Monitor D-Bus proxy added event */
static void monitor_proxy_added_cb(GDBusProxy *proxy, void *user_data)
{
	struct adv_monitor *monitor;
	struct adv_monitor_app *app = user_data;
	struct adv_monitor_merged_pattern *existing_pattern;
	uint16_t adapter_id = app->manager->adapter_id;
	const char *path = g_dbus_proxy_get_path(proxy);
	const char *iface = g_dbus_proxy_get_interface(proxy);
	struct rssi_parameters rssi;

	if (strcmp(iface, ADV_MONITOR_INTERFACE) != 0 ||
		!g_str_has_prefix(path, app->path)) {
		return;
	}

	if (queue_find(app->monitors, monitor_match, proxy)) {
		btd_error(adapter_id,
				"Adv Monitor proxy already exists with path %s",
				path);
		return;
	}

	monitor = monitor_new(app, proxy);
	if (!monitor) {
		btd_error(adapter_id,
				"Failed to allocate an Adv Monitor for the "
				"object at %s", path);
		return;
	}

	if (!monitor_process(monitor)) {
		monitor_destroy(monitor);
		DBG("Adv Monitor at path %s released due to invalid content",
			path);
		return;
	}

	queue_push_tail(app->monitors, monitor);

	existing_pattern = queue_find(monitor->app->manager->merged_patterns,
					merged_pattern_is_equal,
					monitor->merged_pattern);

	if (!existing_pattern) {
		monitor->merged_pattern->manager = monitor->app->manager;
		queue_push_tail(monitor->app->manager->merged_patterns,
						monitor->merged_pattern);
		merged_pattern_add(monitor->merged_pattern);
	} else {
		/* Since there is a matching pattern, abandon the one we have */
		merged_pattern_free(monitor->merged_pattern);
		monitor->merged_pattern = existing_pattern;
		queue_push_tail(existing_pattern->monitors, monitor);

		merge_rssi(&existing_pattern->rssi, &monitor->rssi, &rssi);
		merged_pattern_replace(existing_pattern, &rssi);

		/* Stable means request is not forwarded to kernel */
		if (existing_pattern->current_state ==
						MERGED_PATTERN_STATE_STABLE)
			monitor_state_active(monitor, NULL);
	}

	DBG("Adv Monitor allocated for the object at path %s", path);
}

/* Handles the removal of an Adv Monitor D-Bus proxy */
static void monitor_proxy_removed_cb(GDBusProxy *proxy, void *user_data)
{
	struct adv_monitor *monitor;
	struct adv_monitor_app *app = user_data;

	monitor = queue_find(app->monitors, monitor_match, proxy);

	if (!monitor)
		return;

	DBG("Adv Monitor removed in state %02x with path %s", monitor->state,
		monitor->path);

	monitor_state_released(monitor, NULL);
	monitor_destroy(monitor);
}

/* Creates an app object, initiates it and sets D-Bus event handlers */
static struct adv_monitor_app *app_create(DBusConnection *conn,
					DBusMessage *msg, const char *sender,
					const char *path,
					struct btd_adv_monitor_manager *manager)
{
	struct adv_monitor_app *app;

	if (!path || !sender || !manager)
		return NULL;

	app = new0(struct adv_monitor_app, 1);
	if (!app)
		return NULL;

	app->owner = g_strdup(sender);
	app->path = g_strdup(path);
	app->manager = manager;
	app->reg = NULL;

	app->client = g_dbus_client_new_full(conn, sender, path, path);
	if (!app->client) {
		app_destroy(app);
		return NULL;
	}

	app->monitors = queue_new();

	app->reg = dbus_message_ref(msg);

	g_dbus_client_set_disconnect_watch(app->client, app_disconnect_cb, app);

	/* Note that any property changes on a monitor object would not affect
	 * the content of the corresponding monitor.
	 */
	g_dbus_client_set_proxy_handlers(app->client, monitor_proxy_added_cb,
						monitor_proxy_removed_cb, NULL,
						app);

	g_dbus_client_set_ready_watch(app->client, app_ready_cb, app);

	return app;
}

/* Matches an app based on its owner and path */
static bool app_match(const void *a, const void *b)
{
	const struct adv_monitor_app *app = a;
	const struct app_match_data *match = b;

	if (match->owner && strcmp(app->owner, match->owner))
		return false;

	if (match->path && strcmp(app->path, match->path))
		return false;

	return true;
}

/* Handles a RegisterMonitor D-Bus call */
static DBusMessage *register_monitor(DBusConnection *conn, DBusMessage *msg,
					void *user_data)
{
	DBusMessageIter args;
	struct app_match_data match;
	struct adv_monitor_app *app;
	struct btd_adv_monitor_manager *manager = user_data;

	if (!dbus_message_iter_init(msg, &args))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_OBJECT_PATH)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&args, &match.path);

	if (!strlen(match.path) || !g_str_has_prefix(match.path, "/"))
		return btd_error_invalid_args(msg);

	match.owner = dbus_message_get_sender(msg);

	if (queue_find(manager->apps, app_match, &match))
		return btd_error_already_exists(msg);

	app = app_create(conn, msg, match.owner, match.path, manager);
	if (!app) {
		btd_error(manager->adapter_id,
				"Failed to reserve %s for Adv Monitor app %s",
				match.path, match.owner);
		return btd_error_failed(msg,
					"Failed to create Adv Monitor app");
	}

	queue_push_tail(manager->apps, app);

	return NULL;
}

/* Handles UnregisterMonitor D-Bus call */
static DBusMessage *unregister_monitor(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBusMessageIter args;
	struct app_match_data match;
	struct adv_monitor_app *app;
	struct btd_adv_monitor_manager *manager = user_data;

	if (!dbus_message_iter_init(msg, &args))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_OBJECT_PATH)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&args, &match.path);

	if (!strlen(match.path) || !g_str_has_prefix(match.path, "/"))
		return btd_error_invalid_args(msg);

	match.owner = dbus_message_get_sender(msg);

	app = queue_find(manager->apps, app_match, &match);
	if (!app)
		return btd_error_does_not_exist(msg);

	queue_remove(manager->apps, app);
	app_destroy(app);

	btd_info(manager->adapter_id,
			"Path %s removed along with Adv Monitor app %s",
			match.path, match.owner);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable adv_monitor_methods[] = {
	{ GDBUS_EXPERIMENTAL_ASYNC_METHOD("RegisterMonitor",
					GDBUS_ARGS({ "application", "o" }),
					NULL, register_monitor) },
	{ GDBUS_EXPERIMENTAL_ASYNC_METHOD("UnregisterMonitor",
					GDBUS_ARGS({ "application", "o" }),
					NULL, unregister_monitor) },
	{ }
};

/* Gets SupportedMonitorTypes property */
static gboolean get_supported_monitor_types(const GDBusPropertyTable *property,
						DBusMessageIter *iter,
						void *data)
{
	DBusMessageIter entry;
	const struct adv_monitor_type *t;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
						DBUS_TYPE_STRING_AS_STRING,
						&entry);

	for (t = supported_types; t->name; t++) {
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
						&t->name);
	}

	dbus_message_iter_close_container(iter, &entry);

	return TRUE;
}

const struct adv_monitor_feature {
	uint32_t mask;
	const char *name;
} supported_features[] = {
	{ MGMT_ADV_MONITOR_FEATURE_MASK_OR_PATTERNS, "controller-patterns" },
	{ }
};

/* Gets SupportedFeatures property */
static gboolean get_supported_features(const GDBusPropertyTable *property,
						DBusMessageIter *iter,
						void *data)
{
	DBusMessageIter entry;
	const struct adv_monitor_feature *f;
	struct btd_adv_monitor_manager *manager = data;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
						DBUS_TYPE_STRING_AS_STRING,
						&entry);

	for (f = supported_features; f->name; f++) {
		if (manager->supported_features & f->mask) {
			dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
							&f->name);
		}
	}

	dbus_message_iter_close_container(iter, &entry);

	return TRUE;
}

static const GDBusPropertyTable adv_monitor_properties[] = {
	{"SupportedMonitorTypes", "as", get_supported_monitor_types, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL},
	{"SupportedFeatures", "as", get_supported_features, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL},
	{ }
};

/* Updates monitor state to 'removed' */
static void monitor_state_removed(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;

	if (!monitor || (monitor->state != MONITOR_STATE_INITED
				&& monitor->state != MONITOR_STATE_ACTIVE))
		return;

	monitor->state = MONITOR_STATE_REMOVED;
	monitor->merged_pattern = NULL;
}

/* Remove the matched merged_pattern and remove the monitors */
static void remove_merged_pattern(void *data, void *user_data)
{
	struct adv_monitor_merged_pattern *merged_pattern = data;
	uint16_t *handle = user_data;

	if (!handle)
		return;

	/* handle = 0 indicates kernel has removed all monitors */
	if (handle != 0 && *handle != merged_pattern->monitor_handle)
		return;

	DBG("Adv monitor with handle:0x%04x removed by kernel",
		merged_pattern->monitor_handle);

	queue_foreach(merged_pattern->monitors, monitor_state_removed, NULL);
	queue_destroy(merged_pattern->monitors, monitor_destroy);
	merged_pattern_free(merged_pattern);
}

/* Processes Adv Monitor removed event from kernel */
static void adv_monitor_removed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adv_monitor_manager *manager = user_data;
	const struct mgmt_ev_adv_monitor_removed *ev = param;
	uint16_t handle = ev->monitor_handle;
	const uint16_t adapter_id = manager->adapter_id;

	if (length < sizeof(*ev)) {
		btd_error(adapter_id,
				"Wrong size of Adv Monitor Removed event");
		return;
	}

	/* Traverse the merged_patterns to find matching pattern */
	queue_foreach(manager->merged_patterns, remove_merged_pattern, &handle);

	DBG("Adv Monitor removed event with handle 0x%04x processed",
		ev->monitor_handle);
}

/* Includes found/lost device's object path into the dbus message */
static void report_device_state_setup(DBusMessageIter *iter, void *user_data)
{
	const char *path = device_get_path(user_data);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
}

/* Invokes DeviceFound on the matched monitor */
static void notify_device_found_per_monitor(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;
	struct monitored_device_info *info = user_data;

	if (monitor->merged_pattern->monitor_handle == info->monitor_handle) {
		DBG("Calling DeviceFound() on Adv Monitor of owner %s "
		    "at path %s", monitor->app->owner, monitor->path);

		g_dbus_proxy_method_call(monitor->proxy, "DeviceFound",
					 report_device_state_setup, NULL,
					 info->device, NULL);
	}
}

/* Checks all monitors for match in the app to invoke DeviceFound */
static void notify_device_found_per_app(void *data, void *user_data)
{
	struct adv_monitor_app *app = data;

	queue_foreach(app->monitors, notify_device_found_per_monitor,
		      user_data);
}

/* Processes Adv Monitor Device Found event from kernel */
static void adv_monitor_device_found_callback(uint16_t index, uint16_t length,
						const void *param,
						void *user_data)
{
	const struct mgmt_ev_adv_monitor_device_found *ev = param;
	struct btd_adv_monitor_manager *manager = user_data;
	const uint16_t adapter_id = manager->adapter_id;
	struct btd_adapter *adapter = manager->adapter;
	uint16_t handle = le16_to_cpu(ev->monitor_handle);
	struct monitored_device_info info;
	const uint8_t *ad_data = NULL;
	uint16_t ad_data_len;
	uint32_t flags;
	char addr[18];

	if (length < sizeof(*ev)) {
		btd_error(adapter_id,
				"Too short Adv Monitor Device Found event");
		return;
	}

	ad_data_len = btohs(ev->ad_data_len);
	if (length != sizeof(*ev) + ad_data_len) {
		btd_error(adapter_id,
				"Wrong size of Adv Monitor Device Found event");
		return;
	}

	if (ad_data_len > 0)
		ad_data = ev->ad_data;

	flags = le32_to_cpu(ev->flags);

	ba2str(&ev->addr.bdaddr, addr);
	DBG("hci%u addr %s, rssi %d flags 0x%04x ad_data_len %u",
			index, addr, ev->rssi, flags, ad_data_len);

	btd_adapter_device_found(adapter, &ev->addr.bdaddr,
					ev->addr.type, ev->rssi, flags, ad_data,
					ad_data_len, true);

	if (handle) {
		DBG("Adv Monitor with handle 0x%04x started tracking "
		    "the device %s", handle, addr);

		info.device = btd_adapter_find_device(adapter, &ev->addr.bdaddr,
						      ev->addr.type);
		if (!info.device) {
			btd_error(adapter_id, "Device object not found for %s",
				  addr);
			return;
		}

		/* Check for matched monitor in all apps */
		info.monitor_handle = handle;
		queue_foreach(manager->apps, notify_device_found_per_app,
			      &info);
	}
}

/* Invokes DeviceLost on the matched monitor */
static void notify_device_lost_per_monitor(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;
	struct monitored_device_info *info = user_data;

	if (monitor->merged_pattern->monitor_handle == info->monitor_handle) {
		DBG("Calling DeviceLost() on Adv Monitor of owner %s "
		    "at path %s", monitor->app->owner, monitor->path);

		g_dbus_proxy_method_call(monitor->proxy, "DeviceLost",
					 report_device_state_setup, NULL,
					 info->device, NULL);
	}
}

/* Checks all monitors for match in the app to invoke DeviceLost */
static void notify_device_lost_per_app(void *data, void *user_data)
{
	struct adv_monitor_app *app = data;

	queue_foreach(app->monitors, notify_device_lost_per_monitor,
		      user_data);
}

/* Processes Adv Monitor Device Lost event from kernel */
static void adv_monitor_device_lost_callback(uint16_t index, uint16_t length,
						const void *param,
						void *user_data)
{
	struct btd_adv_monitor_manager *manager = user_data;
	const struct mgmt_ev_adv_monitor_device_lost *ev = param;
	uint16_t handle = le16_to_cpu(ev->monitor_handle);
	const uint16_t adapter_id = manager->adapter_id;
	struct btd_adapter *adapter = manager->adapter;
	struct monitored_device_info info;
	char addr[18];

	if (length < sizeof(*ev)) {
		btd_error(adapter_id,
				"Wrong size of Adv Monitor Device Lost event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	DBG("Adv Monitor with handle 0x%04x stopped tracking the device %s",
		handle, addr);

	info.device = btd_adapter_find_device(adapter, &ev->addr.bdaddr,
					      ev->addr.type);
	if (!info.device) {
		btd_error(adapter_id, "Device object not found for %s", addr);
		return;
	}

	/* Check for matched monitor in all apps */
	info.monitor_handle = handle;
	queue_foreach(manager->apps, notify_device_lost_per_app, &info);
}

/* Allocates a manager object */
static struct btd_adv_monitor_manager *manager_new(
						struct btd_adapter *adapter,
						struct mgmt *mgmt)
{
	struct btd_adv_monitor_manager *manager;

	if (!adapter || !mgmt)
		return NULL;

	manager = new0(struct btd_adv_monitor_manager, 1);
	if (!manager)
		return NULL;

	manager->adapter = adapter;
	manager->mgmt = mgmt_ref(mgmt);
	manager->adapter_id = btd_adapter_get_index(adapter);
	manager->apps = queue_new();
	manager->merged_patterns = queue_new();

	mgmt_register(manager->mgmt, MGMT_EV_ADV_MONITOR_REMOVED,
			manager->adapter_id, adv_monitor_removed_callback,
			manager, NULL);

	mgmt_register(manager->mgmt, MGMT_EV_ADV_MONITOR_DEVICE_FOUND,
			manager->adapter_id, adv_monitor_device_found_callback,
			manager, NULL);

	mgmt_register(manager->mgmt, MGMT_EV_ADV_MONITOR_DEVICE_LOST,
			manager->adapter_id, adv_monitor_device_lost_callback,
			manager, NULL);

	return manager;
}

/* Frees a manager object */
static void manager_free(struct btd_adv_monitor_manager *manager)
{
	mgmt_unref(manager->mgmt);

	queue_destroy(manager->apps, app_destroy);
	queue_destroy(manager->merged_patterns, merged_pattern_free);

	free(manager);
}

/* Destroys a manager object and unregisters its D-Bus interface */
static void manager_destroy(struct btd_adv_monitor_manager *manager)
{
	if (!manager)
		return;

	g_dbus_unregister_interface(btd_get_dbus_connection(),
					adapter_get_path(manager->adapter),
					ADV_MONITOR_MGR_INTERFACE);

	manager_free(manager);
}

/* Initiates manager's members based on the return of
 * MGMT_OP_READ_ADV_MONITOR_FEATURES
 */
static void read_adv_monitor_features_cb(uint8_t status, uint16_t length,
						const void *param,
						void *user_data)
{
	const struct mgmt_rp_read_adv_monitor_features *rp = param;
	struct btd_adv_monitor_manager *manager = user_data;

	if (status != MGMT_STATUS_SUCCESS || !param) {
		btd_error(manager->adapter_id,
				"Failed to Read Adv Monitor Features with "
				"status 0x%02x", status);
		return;
	}

	if (length < sizeof(*rp)) {
		btd_error(manager->adapter_id,
				"Wrong size of Read Adv Monitor Features "
				"response");
		return;
	}

	manager->supported_features = le32_to_cpu(rp->supported_features);
	manager->enabled_features = le32_to_cpu(rp->enabled_features);
	manager->max_num_monitors = le16_to_cpu(rp->max_num_handles);
	manager->max_num_patterns = rp->max_num_patterns;

	btd_info(manager->adapter_id, "Adv Monitor Manager created with "
			"supported features:0x%08x, enabled features:0x%08x, "
			"max number of supported monitors:%d, "
			"max number of supported patterns:%d",
			manager->supported_features, manager->enabled_features,
			manager->max_num_monitors, manager->max_num_patterns);
}

/* Creates a manager and registers its D-Bus interface */
struct btd_adv_monitor_manager *btd_adv_monitor_manager_create(
						struct btd_adapter *adapter,
						struct mgmt *mgmt)
{
	struct btd_adv_monitor_manager *manager;

	manager = manager_new(adapter, mgmt);
	if (!manager)
		return NULL;

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					adapter_get_path(manager->adapter),
					ADV_MONITOR_MGR_INTERFACE,
					adv_monitor_methods, NULL,
					adv_monitor_properties, manager,
					NULL)) {
		btd_error(manager->adapter_id,
				"Failed to register "
				ADV_MONITOR_MGR_INTERFACE);
		manager_free(manager);
		return NULL;
	}

	if (!mgmt_send(manager->mgmt, MGMT_OP_READ_ADV_MONITOR_FEATURES,
			manager->adapter_id, 0, NULL,
			read_adv_monitor_features_cb, manager, NULL)) {
		btd_error(manager->adapter_id,
				"Failed to send Read Adv Monitor Features");
		manager_destroy(manager);
		return NULL;
	}

	return manager;
}

/* Destroys a manager and unregisters its D-Bus interface */
void btd_adv_monitor_manager_destroy(struct btd_adv_monitor_manager *manager)
{
	if (!manager)
		return;

	btd_info(manager->adapter_id, "Destroy Adv Monitor Manager");

	manager_destroy(manager);
}

bool btd_adv_monitor_offload_enabled(struct btd_adv_monitor_manager *manager)
{
	if (!manager)
		return false;

	return !!(manager->enabled_features &
				MGMT_ADV_MONITOR_FEATURE_MASK_OR_PATTERNS);
}

/* Processes the content matching based pattern(s) of a monitor */
static void adv_match_per_monitor(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;
	struct adv_content_filter_info *info = user_data;
	struct queue *patterns;

	if (!monitor) {
		error("Unexpected NULL adv_monitor object upon match");
		return;
	}

	if (monitor->state != MONITOR_STATE_ACTIVE)
		return;

	if (!monitor->merged_pattern)
		return;

	patterns = monitor->merged_pattern->patterns;
	if (monitor->merged_pattern->type == MONITOR_TYPE_OR_PATTERNS &&
				bt_ad_pattern_match(info->ad, patterns)) {
		goto matched;
	}

	return;

matched:
	if (!info->matched_monitors)
		info->matched_monitors = queue_new();

	queue_push_tail(info->matched_monitors, monitor);
}

/* Processes the content matching for the monitor(s) of an app */
static void adv_match_per_app(void *data, void *user_data)
{
	struct adv_monitor_app *app = data;

	if (!app) {
		error("Unexpected NULL adv_monitor_app object upon match");
		return;
	}

	queue_foreach(app->monitors, adv_match_per_monitor, user_data);
}

/* Processes the content matching for every app without RSSI filtering and
 * notifying monitors. The caller is responsible of releasing the memory of the
 * list but not the ad data.
 * Returns the list of monitors whose content match the ad data.
 */
struct queue *btd_adv_monitor_content_filter(
				struct btd_adv_monitor_manager *manager,
				struct bt_ad *ad)
{
	struct adv_content_filter_info info;

	if (!manager || !ad)
		return NULL;

	info.ad = ad;
	info.matched_monitors = NULL;

	queue_foreach(manager->apps, adv_match_per_app, &info);

	return info.matched_monitors;
}

/* Wraps adv_monitor_filter_rssi() to processes the content-matched monitor with
 * RSSI filtering and notifies it on device found/lost event
 */
static void monitor_filter_rssi(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;
	struct adv_rssi_filter_info *info = user_data;

	if (!monitor || !info)
		return;

	adv_monitor_filter_rssi(monitor, info->device, info->rssi);
}

/* Processes every content-matched monitor with RSSI filtering and notifies on
 * device found/lost event. The caller is responsible of releasing the memory
 * of matched_monitors list but not its data.
 */
void btd_adv_monitor_notify_monitors(struct btd_adv_monitor_manager *manager,
					struct btd_device *device, int8_t rssi,
					struct queue *matched_monitors)
{
	struct adv_rssi_filter_info info;

	if (!manager || !device || !matched_monitors ||
		queue_isempty(matched_monitors)) {
		return;
	}

	info.device = device;
	info.rssi = rssi;

	queue_foreach(matched_monitors, monitor_filter_rssi, &info);
}

/* Matches a device based on btd_device object */
static bool monitor_device_match(const void *a, const void *b)
{
	const struct adv_monitor_device *dev = a;
	const struct btd_device *device = b;

	if (!dev) {
		error("Unexpected NULL adv_monitor_device object upon match");
		return false;
	}

	if (dev->device != device)
		return false;

	return true;
}

/* Frees a monitor device object */
static void monitor_device_free(void *data)
{
	struct adv_monitor_device *dev = data;

	if (!dev) {
		error("Unexpected NULL adv_monitor_device object upon free");
		return;
	}

	if (dev->lost_timer) {
		timeout_remove(dev->lost_timer);
		dev->lost_timer = 0;
	}

	dev->monitor = NULL;
	dev->device = NULL;

	free(dev);
}

/* Removes a device from monitor->devices list */
static void remove_device_from_monitor(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;
	struct btd_device *device = user_data;
	struct adv_monitor_device *dev = NULL;

	if (!monitor) {
		error("Unexpected NULL adv_monitor object upon device remove");
		return;
	}

	dev = queue_remove_if(monitor->devices, monitor_device_match, device);
	if (dev) {
		DBG("Device removed from the Adv Monitor at path %s",
		    monitor->path);
		monitor_device_free(dev);
	}
}

/* Removes a device from every monitor in an app */
static void remove_device_from_app(void *data, void *user_data)
{
	struct adv_monitor_app *app = data;
	struct btd_device *device = user_data;

	if (!app) {
		error("Unexpected NULL adv_monitor_app object upon device "
			"remove");
		return;
	}

	queue_foreach(app->monitors, remove_device_from_monitor, device);
}

/* Removes a device from every monitor in all apps */
void btd_adv_monitor_device_remove(struct btd_adv_monitor_manager *manager,
				   struct btd_device *device)
{
	if (!manager || !device)
		return;

	queue_foreach(manager->apps, remove_device_from_app, device);
}

/* Creates a device object to track the per-device information */
static struct adv_monitor_device *monitor_device_create(
			struct adv_monitor *monitor,
			struct btd_device *device)
{
	struct adv_monitor_device *dev = NULL;

	dev = new0(struct adv_monitor_device, 1);
	if (!dev)
		return NULL;

	dev->monitor = monitor;
	dev->device = device;

	queue_push_tail(monitor->devices, dev);

	return dev;
}

/* Handles a situation where the device goes offline/out-of-range */
static bool handle_device_lost_timeout(gpointer user_data)
{
	struct adv_monitor_device *dev = user_data;
	struct adv_monitor *monitor = dev->monitor;

	DBG("Device Lost timeout triggered for device %p. Calling DeviceLost() "
	    "on Adv Monitor of owner %s at path %s", dev->device,
					    monitor->app->owner, monitor->path);

	g_dbus_proxy_method_call(monitor->proxy, "DeviceLost",
				 report_device_state_setup,
				 NULL, dev->device, NULL);

	dev->lost_timer = 0;
	dev->found = false;

	return FALSE;
}

/* Filters an Adv based on its RSSI value */
static void adv_monitor_filter_rssi(struct adv_monitor *monitor,
				    struct btd_device *device, int8_t rssi)
{
	struct adv_monitor_device *dev = NULL;
	time_t curr_time = time(NULL);
	uint16_t adapter_id = monitor->app->manager->adapter_id;

	/* If the RSSI thresholds and timeouts are not specified, report the
	 * DeviceFound() event without tracking for the RSSI as the Adv has
	 * already matched the pattern filter.
	 */
	if (rssi_is_unset(&monitor->rssi)) {
		DBG("Calling DeviceFound() on Adv Monitor of owner %s "
		    "at path %s", monitor->app->owner, monitor->path);

		g_dbus_proxy_method_call(monitor->proxy, "DeviceFound",
					 report_device_state_setup, NULL,
					 device, NULL);

		return;
	}

	dev = queue_find(monitor->devices, monitor_device_match, device);
	if (!dev) {
		dev = monitor_device_create(monitor, device);
		if (!dev) {
			btd_error(adapter_id,
				"Failed to create Adv Monitor device object.");
			return;
		}
	}

	if (dev->lost_timer) {
		timeout_remove(dev->lost_timer);
		dev->lost_timer = 0;
	}

	/* Reset the timings of found/lost if a device has been offline for
	 * longer than the high/low timeouts.
	 */
	if (dev->last_seen) {
		if (difftime(curr_time, dev->last_seen) >
		    monitor->rssi.high_rssi_timeout) {
			dev->high_rssi_first_seen = 0;
		}

		if (difftime(curr_time, dev->last_seen) >
		    monitor->rssi.low_rssi_timeout) {
			dev->low_rssi_first_seen = 0;
		}
	}
	dev->last_seen = curr_time;

	/* Check for the found devices (if the device is not already found) */
	if (!dev->found && rssi > monitor->rssi.high_rssi) {
		if (dev->high_rssi_first_seen) {
			if (difftime(curr_time, dev->high_rssi_first_seen) >=
			    monitor->rssi.high_rssi_timeout) {
				dev->found = true;

				DBG("Calling DeviceFound() on Adv Monitor "
				    "of owner %s at path %s",
				    monitor->app->owner, monitor->path);

				g_dbus_proxy_method_call(
					monitor->proxy, "DeviceFound",
					report_device_state_setup, NULL,
					dev->device, NULL);
			}
		} else {
			dev->high_rssi_first_seen = curr_time;
		}
	} else {
		dev->high_rssi_first_seen = 0;
	}

	/* Check for the lost devices (only if the device is already found, as
	 * it doesn't make any sense to report the Device Lost event if the
	 * device is not found yet)
	 */
	if (dev->found && rssi < monitor->rssi.low_rssi) {
		if (dev->low_rssi_first_seen) {
			if (difftime(curr_time, dev->low_rssi_first_seen) >=
			    monitor->rssi.low_rssi_timeout) {
				dev->found = false;

				DBG("Calling DeviceLost() on Adv Monitor "
				    "of owner %s at path %s",
				    monitor->app->owner, monitor->path);

				g_dbus_proxy_method_call(
					monitor->proxy, "DeviceLost",
					report_device_state_setup, NULL,
					dev->device, NULL);
			}
		} else {
			dev->low_rssi_first_seen = curr_time;
		}
	} else {
		dev->low_rssi_first_seen = 0;
	}

	/* Setup a timer to track if the device goes offline/out-of-range, only
	 * if we are tracking for the Low RSSI Threshold. If we are tracking
	 * the High RSSI Threshold, nothing needs to be done.
	 */
	if (dev->found) {
		dev->lost_timer =
			timeout_add_seconds(monitor->rssi.low_rssi_timeout,
					    handle_device_lost_timeout, dev,
					    NULL);
	}
}

/* Clears running DeviceLost timer for a given device */
static void clear_device_lost_timer(void *data, void *user_data)
{
	struct adv_monitor_device *dev = data;
	struct adv_monitor *monitor = NULL;

	if (dev->lost_timer) {
		timeout_remove(dev->lost_timer);
		dev->lost_timer = 0;

		monitor = dev->monitor;

		DBG("Calling DeviceLost() for device %p on Adv Monitor "
				"of owner %s at path %s", dev->device,
				monitor->app->owner, monitor->path);

		g_dbus_proxy_method_call(monitor->proxy, "DeviceLost",
				report_device_state_setup,
				NULL, dev->device, NULL);
	}
}

/* Clears running DeviceLost timers from each monitor */
static void clear_lost_timers_from_monitor(void *data, void *user_data)
{
	struct adv_monitor *monitor = data;

	queue_foreach(monitor->devices, clear_device_lost_timer, NULL);
}

/* Clears running DeviceLost timers from each app */
static void clear_lost_timers_from_app(void *data, void *user_data)
{
	struct adv_monitor_app *app = data;

	queue_foreach(app->monitors, clear_lost_timers_from_monitor, NULL);
}

/* Handles bt power down scenario */
void btd_adv_monitor_power_down(struct btd_adv_monitor_manager *manager)
{
	if (!manager) {
		error("Unexpected NULL btd_adv_monitor_manager object upon "
				"power down");
		return;
	}

	/* Clear any running DeviceLost timers in case of power down */
	queue_foreach(manager->apps, clear_lost_timers_from_app, NULL);
}
