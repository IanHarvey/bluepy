// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  Intel Corporation
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>

#include <glib.h>
#include <dbus/dbus.h>

#include "gdbus/gdbus.h"
#include "src/shared/util.h"
#include "src/shared/queue.h"
#include "src/shared/ad.h"
#include "src/shared/crypto.h"

#include "log.h"
#include "error.h"
#include "adapter.h"
#include "device.h"
#include "dbus-common.h"
#include "set.h"

static struct queue *set_list;

struct btd_device_set {
	struct btd_adapter *adapter;
	char *path;
	uint8_t sirk[16];
	uint8_t size;
	bool auto_connect;
	struct queue *devices;
	struct btd_device *device;
};

static DBusMessage *set_disconnect(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	/* TODO */
	return NULL;
}

static DBusMessage *set_connect(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	/* TODO */
	return NULL;
}

static const GDBusMethodTable set_methods[] = {
	{ GDBUS_EXPERIMENTAL_ASYNC_METHOD("Disconnect", NULL, NULL,
						set_disconnect) },
	{ GDBUS_EXPERIMENTAL_ASYNC_METHOD("Connect", NULL, NULL,
						set_connect) },
	{}
};

static gboolean get_adapter(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device_set *set = data;
	const char *path = adapter_get_path(set->adapter);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	return TRUE;
}

static gboolean get_auto_connect(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device_set *set = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN,
						&set->auto_connect);

	return TRUE;
}

static void set_auto_connect(const GDBusPropertyTable *property,
					DBusMessageIter *iter,
					 GDBusPendingPropertySet id, void *data)
{
	struct btd_device_set *set = data;
	dbus_bool_t b;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_BOOLEAN) {
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	dbus_message_iter_get_basic(iter, &b);

	set->auto_connect = b ? true : false;

	g_dbus_pending_property_success(id);
}

static void append_device(void *data, void *user_data)
{
	struct btd_device *device = data;
	const char *path = device_get_path(device);
	DBusMessageIter *entry = user_data;

	dbus_message_iter_append_basic(entry, DBUS_TYPE_OBJECT_PATH, &path);
}

static gboolean get_devices(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device_set *set = data;
	DBusMessageIter entry;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_OBJECT_PATH_AS_STRING,
					&entry);

	queue_foreach(set->devices, append_device, &entry);

	dbus_message_iter_close_container(iter, &entry);

	return TRUE;
}

static gboolean get_size(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device_set *set = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &set->size);

	return TRUE;
}

static const GDBusPropertyTable set_properties[] = {
	{ "Adapter", "o", get_adapter, NULL, NULL,
			G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "AutoConnect", "b", get_auto_connect, set_auto_connect, NULL,
			G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Devices", "ao", get_devices, NULL, NULL,
			G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Size", "y", get_size, NULL, NULL,
			G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{}
};

static void set_free(void *data)
{
	struct btd_device_set *set = data;

	queue_destroy(set->devices, NULL);
	g_free(set->path);
	free(set);
}

static struct btd_device_set *set_new(struct btd_device *device,
					uint8_t sirk[16], uint8_t size)
{
	struct btd_device_set *set;

	set = new0(struct btd_device_set, 1);
	set->adapter = device_get_adapter(device);
	memcpy(set->sirk, sirk, sizeof(set->sirk));
	set->size = size;
	set->auto_connect = true;
	set->devices = queue_new();
	queue_push_tail(set->devices, device);
	set->path = g_strdup_printf("%s/set_%02x%02x%02x%02x%02x%02x%02x%02x"
					"%02x%02x%02x%02x%02x%02x%02x%02x",
					adapter_get_path(set->adapter),
					sirk[15], sirk[14], sirk[13], sirk[12],
					sirk[11], sirk[10], sirk[9], sirk[8],
					sirk[7], sirk[6], sirk[5], sirk[4],
					sirk[3], sirk[2], sirk[1], sirk[0]);

	DBG("Creating set %s", set->path);

	if (g_dbus_register_interface(btd_get_dbus_connection(),
					set->path, BTD_DEVICE_SET_INTERFACE,
					set_methods, NULL,
					set_properties, set,
					set_free) == FALSE) {
		error("Unable to register set interface");
		set_free(set);
		return NULL;
	}

	return set;
}

static struct btd_device_set *set_find(struct btd_device *device,
						uint8_t sirk[16])
{
	struct btd_adapter *adapter = device_get_adapter(device);
	const struct queue_entry *entry;

	for (entry = queue_get_entries(set_list); entry; entry = entry->next) {
		struct btd_device_set *set = entry->data;

		if (set->adapter != adapter)
			continue;

		if (!memcmp(set->sirk, sirk, sizeof(set->sirk)))
			return set;
	}

	return NULL;
}

static void set_connect_next(struct btd_device_set *set)
{
	const struct queue_entry *entry;

	for (entry = queue_get_entries(set->devices); entry;
					entry = entry->next) {
		struct btd_device *device = entry->data;

		/* Only connect one at time(?) */
		if (!device_connect_le(device))
			return;
	}
}

static void set_add(struct btd_device_set *set, struct btd_device *device)
{
	/* Check if device is already part of the set then skip to connect */
	if (queue_find(set->devices, NULL, device))
		goto done;

	DBG("set %s device %s", set->path, device_get_path(device));

	queue_push_tail(set->devices, device);
	g_dbus_emit_property_changed(btd_get_dbus_connection(), set->path,
					BTD_DEVICE_SET_INTERFACE, "Devices");

done:
	/* Check if set is marked to auto-connect */
	if (btd_device_is_connected(device) && set->auto_connect)
		set_connect_next(set);
}

static void foreach_rsi(void *data, void *user_data)
{
	struct bt_ad_data *ad = data;
	struct btd_device_set *set = user_data;
	struct bt_crypto *crypto;
	uint8_t res[3];

	if (ad->type != BT_AD_CSIP_RSI || ad->len < 6)
		return;

	crypto = bt_crypto_new();
	if (!crypto)
		return;

	if (!bt_crypto_sih(crypto, set->sirk, ad->data + 3, res)) {
		bt_crypto_unref(crypto);
		return;
	}

	bt_crypto_unref(crypto);

	if (!memcmp(ad->data, res, sizeof(res)))
		device_connect_le(set->device);
}

static void foreach_device(struct btd_device *device, void *data)
{
	struct btd_device_set *set = data;

	/* Check if device is already part of the set then skip */
	if (queue_find(set->devices, NULL, device))
		return;

	set->device = device;

	btd_device_foreach_ad(device, foreach_rsi, set);
}

struct btd_device_set *btd_set_add_device(struct btd_device *device,
						uint8_t *key, uint8_t sirk[16],
						uint8_t size)
{
	struct btd_device_set *set;

	/* In case key has been set it means SIRK is encrypted */
	if (key) {
		struct bt_crypto *crypto = bt_crypto_new();

		if (!crypto)
			return NULL;

		/* sef and sdf are symmetric */
		bt_crypto_sef(crypto, key, sirk, sirk);

		bt_crypto_unref(crypto);
	}

	/* Check if DeviceSet already exists */
	set = set_find(device, sirk);
	if (set) {
		set_add(set, device);
		/* Check if there are new devices with RSI found */
		goto done;
	}

	set = set_new(device, sirk, size);
	if (!set)
		return NULL;

	if (!set_list)
		set_list = queue_new();

	queue_push_tail(set_list, set);

done:
	/* Attempt to add devices which have matching RSI */
	btd_adapter_for_each_device(device_get_adapter(device), foreach_device,
									set);

	return set;
}

bool btd_set_remove_device(struct btd_device_set *set,
						struct btd_device *device)
{
	if (!set || !device)
		return false;

	if (!queue_remove_if(set->devices, NULL, device))
		return false;

	if (!queue_isempty(set->devices)) {
		g_dbus_emit_property_changed(btd_get_dbus_connection(),
						set->path,
						BTD_DEVICE_SET_INTERFACE,
						"Devices");
		return true;
	}

	if (!queue_remove(set_list, set))
		return false;

	/* Unregister if there are no devices left in the set */
	g_dbus_unregister_interface(btd_get_dbus_connection(), set->path,
						BTD_DEVICE_SET_INTERFACE);

	return true;
}

const char *btd_set_get_path(struct btd_device_set *set)
{
	return set->path;
}
