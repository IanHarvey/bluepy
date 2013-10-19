/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <dirent.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>
#include <btio/btio.h>

#include "log.h"

#include "lib/uuid.h"
#include "lib/mgmt.h"
#include "attrib/att.h"
#include "hcid.h"
#include "adapter.h"
#include "attrib/gattrib.h"
#include "attio.h"
#include "device.h"
#include "profile.h"
#include "dbus-common.h"
#include "error.h"
#include "glib-helper.h"
#include "sdp-client.h"
#include "attrib/gatt.h"
#include "agent.h"
#include "sdp-xml.h"
#include "storage.h"
#include "attrib-server.h"

#define IO_CAPABILITY_NOINPUTNOOUTPUT	0x03

#define DISCONNECT_TIMER	2
#define DISCOVERY_TIMER		1

static DBusConnection *dbus_conn = NULL;

struct btd_disconnect_data {
	guint id;
	disconnect_watch watch;
	void *user_data;
	GDestroyNotify destroy;
};

struct bonding_req {
	DBusMessage *msg;
	guint listener_id;
	struct btd_device *device;
	struct agent *agent;
};

typedef enum {
	AUTH_TYPE_PINCODE,
	AUTH_TYPE_PASSKEY,
	AUTH_TYPE_CONFIRM,
	AUTH_TYPE_NOTIFY_PASSKEY,
	AUTH_TYPE_NOTIFY_PINCODE,
} auth_type_t;

struct authentication_req {
	auth_type_t type;
	struct agent *agent;
	struct btd_device *device;
	uint32_t passkey;
	char *pincode;
	gboolean secure;
};

struct browse_req {
	DBusMessage *msg;
	struct btd_device *device;
	GSList *match_uuids;
	GSList *profiles_added;
	GSList *profiles_removed;
	sdp_list_t *records;
	int search_uuid;
	int reconnect_attempt;
	guint listener_id;
};

struct included_search {
	struct browse_req *req;
	GSList *services;
	GSList *current;
};

struct attio_data {
	guint id;
	attio_connect_cb cfunc;
	attio_disconnect_cb dcfunc;
	gpointer user_data;
};

struct svc_callback {
	unsigned int id;
	guint idle_id;
	struct btd_device *dev;
	device_svc_cb_t func;
	void *user_data;
};

typedef void (*attio_error_cb) (const GError *gerr, gpointer user_data);
typedef void (*attio_success_cb) (gpointer user_data);

struct att_callbacks {
	attio_error_cb error;		/* Callback for error */
	attio_success_cb success;	/* Callback for success */
	gpointer user_data;
};

struct btd_device {
	int ref_count;

	bdaddr_t	bdaddr;
	uint8_t		bdaddr_type;
	char		*path;
	bool		svc_resolved;
	GSList		*svc_callbacks;
	GSList		*eir_uuids;
	char		name[MAX_NAME_LENGTH + 1];
	char		*alias;
	uint32_t	class;
	uint16_t	vendor_src;
	uint16_t	vendor;
	uint16_t	product;
	uint16_t	version;
	uint16_t	appearance;
	char		*modalias;
	struct btd_adapter	*adapter;
	GSList		*uuids;
	GSList		*primaries;		/* List of primary services */
	GSList		*profiles;		/* Probed profiles */
	GSList		*pending;		/* Pending profiles */
	GSList		*watches;		/* List of disconnect_data */
	gboolean	temporary;
	guint		disconn_timer;
	guint		discov_timer;
	struct browse_req *browse;		/* service discover request */
	struct bonding_req *bonding;
	struct authentication_req *authr;	/* authentication request */
	GSList		*disconnects;		/* disconnects message */
	DBusMessage	*connect;		/* connect message */
	DBusMessage	*disconnect;		/* disconnect message */
	GAttrib		*attrib;
	GSList		*attios;
	GSList		*attios_offline;
	guint		attachid;		/* Attrib server attach */

	gboolean	connected;
	GSList		*connected_profiles;

	sdp_list_t	*tmp_records;

	gboolean	trusted;
	gboolean	paired;
	gboolean	blocked;
	gboolean	bonded;
	gboolean	auto_connect;
	gboolean	disable_auto_connect;
	gboolean	general_connect;

	bool		legacy;
	int8_t		rssi;

	GIOChannel	*att_io;
	guint		cleanup_id;
	guint		store_id;
};

static const uint16_t uuid_list[] = {
	L2CAP_UUID,
	PNP_INFO_SVCLASS_ID,
	PUBLIC_BROWSE_GROUP,
	0
};

static int device_browse_primary(struct btd_device *device, DBusMessage *msg,
							gboolean secure);
static int device_browse_sdp(struct btd_device *device, DBusMessage *msg,
							gboolean reverse);

static gboolean store_device_info_cb(gpointer user_data)
{
	struct btd_device *device = user_data;
	GKeyFile *key_file;
	char filename[PATH_MAX + 1];
	char adapter_addr[18];
	char device_addr[18];
	char *str;
	char class[9];
	char **uuids = NULL;
	gsize length = 0;

	device->store_id = 0;

	ba2str(adapter_get_address(device->adapter), adapter_addr);
	ba2str(&device->bdaddr, device_addr);
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/info", adapter_addr,
			device_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	g_key_file_set_string(key_file, "General", "Name", device->name);

	if (device->alias != NULL)
		g_key_file_set_string(key_file, "General", "Alias",
								device->alias);
	else
		g_key_file_remove_key(key_file, "General", "Alias", NULL);

	if (device->class) {
		sprintf(class, "0x%6.6x", device->class);
		g_key_file_set_string(key_file, "General", "Class", class);
	} else {
		g_key_file_remove_key(key_file, "General", "Class", NULL);
	}

	if (device->appearance) {
		sprintf(class, "0x%4.4x", device->appearance);
		g_key_file_set_string(key_file, "General", "Appearance", class);
	} else {
		g_key_file_remove_key(key_file, "General", "Appearance", NULL);
	}

	switch (device->bdaddr_type) {
	case BDADDR_BREDR:
		g_key_file_set_string(key_file, "General",
					"SupportedTechnologies", "BR/EDR");
		g_key_file_remove_key(key_file, "General",
					"AddressType", NULL);
		break;

	case BDADDR_LE_PUBLIC:
		g_key_file_set_string(key_file, "General",
					"SupportedTechnologies", "LE");
		g_key_file_set_string(key_file, "General",
					"AddressType", "public");
		break;

	case BDADDR_LE_RANDOM:
		g_key_file_set_string(key_file, "General",
					"SupportedTechnologies", "LE");
		g_key_file_set_string(key_file, "General",
					"AddressType", "static");
		break;

	default:
		g_key_file_remove_key(key_file, "General",
					"SupportedTechnologies", NULL);
		g_key_file_remove_key(key_file, "General",
					"AddressType", NULL);
	}

	g_key_file_set_boolean(key_file, "General", "Trusted",
							device->trusted);

	g_key_file_set_boolean(key_file, "General", "Blocked",
							device->blocked);

	if (device->uuids) {
		GSList *l;
		int i;

		uuids = g_new0(char *, g_slist_length(device->uuids) + 1);
		for (i = 0, l = device->uuids; l; l = g_slist_next(l), i++)
			uuids[i] = l->data;
		g_key_file_set_string_list(key_file, "General", "Services",
						(const char **)uuids, i);
	} else {
		g_key_file_remove_key(key_file, "General", "Services", NULL);
	}

	if (device->vendor_src) {
		g_key_file_set_integer(key_file, "DeviceID", "Source",
					device->vendor_src);
		g_key_file_set_integer(key_file, "DeviceID", "Vendor",
					device->vendor);
		g_key_file_set_integer(key_file, "DeviceID", "Product",
					device->product);
		g_key_file_set_integer(key_file, "DeviceID", "Version",
					device->version);
	} else {
		g_key_file_remove_group(key_file, "DeviceID", NULL);
	}

	create_file(filename, S_IRUSR | S_IWUSR);

	str = g_key_file_to_data(key_file, &length, NULL);
	g_file_set_contents(filename, str, length, NULL);
	g_free(str);

	g_key_file_free(key_file);
	g_free(uuids);

	return FALSE;
}

static void store_device_info(struct btd_device *device)
{
	if (device->temporary || device->store_id > 0)
		return;

	device->store_id = g_idle_add(store_device_info_cb, device);
}

static void browse_request_free(struct browse_req *req)
{
	if (req->listener_id)
		g_dbus_remove_watch(dbus_conn, req->listener_id);
	if (req->msg)
		dbus_message_unref(req->msg);
	if (req->device)
		btd_device_unref(req->device);
	g_slist_free_full(req->profiles_added, g_free);
	g_slist_free(req->profiles_removed);
	if (req->records)
		sdp_list_free(req->records, (sdp_free_func_t) sdp_record_free);

	g_free(req);
}

static void attio_cleanup(struct btd_device *device)
{
	if (device->attachid) {
		attrib_channel_detach(device->attrib, device->attachid);
		device->attachid = 0;
	}

	if (device->cleanup_id) {
		g_source_remove(device->cleanup_id);
		device->cleanup_id = 0;
	}

	if (device->att_io) {
		g_io_channel_shutdown(device->att_io, FALSE, NULL);
		g_io_channel_unref(device->att_io);
		device->att_io = NULL;
	}

	if (device->attrib) {
		g_attrib_unref(device->attrib);
		device->attrib = NULL;
	}
}

static void browse_request_cancel(struct browse_req *req)
{
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device->adapter;

	bt_cancel_discovery(adapter_get_address(adapter), &device->bdaddr);

	attio_cleanup(device);

	device->browse = NULL;
	browse_request_free(req);
}

static void svc_dev_remove(gpointer user_data)
{
	struct svc_callback *cb = user_data;

	if (cb->idle_id > 0)
		g_source_remove(cb->idle_id);

	cb->func(cb->dev, -ENODEV, cb->user_data);

	g_free(cb);
}

static void device_free(gpointer user_data)
{
	struct btd_device *device = user_data;

	g_slist_free_full(device->uuids, g_free);
	g_slist_free_full(device->primaries, g_free);
	g_slist_free_full(device->attios, g_free);
	g_slist_free_full(device->attios_offline, g_free);
	g_slist_free_full(device->svc_callbacks, svc_dev_remove);

	attio_cleanup(device);

	if (device->tmp_records)
		sdp_list_free(device->tmp_records,
					(sdp_free_func_t) sdp_record_free);

	if (device->disconn_timer)
		g_source_remove(device->disconn_timer);

	if (device->discov_timer)
		g_source_remove(device->discov_timer);

	if (device->connect)
		dbus_message_unref(device->connect);

	if (device->disconnect)
		dbus_message_unref(device->disconnect);

	DBG("%p", device);

	if (device->authr) {
		if (device->authr->agent)
			agent_unref(device->authr->agent);
		g_free(device->authr->pincode);
		g_free(device->authr);
	}

	if (device->eir_uuids)
		g_slist_free_full(device->eir_uuids, g_free);

	g_free(device->path);
	g_free(device->alias);
	g_free(device->modalias);
	g_free(device);
}

gboolean device_is_bredr(struct btd_device *device)
{
	return (device->bdaddr_type == BDADDR_BREDR);
}

gboolean device_is_le(struct btd_device *device)
{
	return (device->bdaddr_type != BDADDR_BREDR);
}

gboolean device_is_paired(struct btd_device *device)
{
	return device->paired;
}

gboolean device_is_bonded(struct btd_device *device)
{
	return device->bonded;
}

gboolean device_is_trusted(struct btd_device *device)
{
	return device->trusted;
}

static gboolean dev_property_get_address(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	char dstaddr[18];
	const char *ptr = dstaddr;

	ba2str(&device->bdaddr, dstaddr);
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &ptr);

	return TRUE;
}

static gboolean dev_property_get_name(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	const char *empty = "", *ptr;

	ptr = device->name ?: empty;
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &ptr);

	return TRUE;
}

static gboolean dev_property_exists_name(const GDBusPropertyTable *property,
								void *data)
{
	struct btd_device *dev = data;

	return device_name_known(dev);
}

static gboolean dev_property_get_alias(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	char dstaddr[18];
	const char *ptr;

	/* Alias (fallback to name or address) */
	if (device->alias != NULL)
		ptr = device->alias;
	else if (strlen(device->name) > 0) {
		ptr = device->name;
	} else {
		ba2str(&device->bdaddr, dstaddr);
		g_strdelimit(dstaddr, ":", '-');
		ptr = dstaddr;
	}

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &ptr);

	return TRUE;
}

static void set_alias(GDBusPendingPropertySet id, const char *alias,
								void *data)
{
	struct btd_device *device = data;

	/* No change */
	if ((device->alias == NULL && g_str_equal(alias, "")) ||
					g_strcmp0(device->alias, alias) == 0) {
		g_dbus_pending_property_success(id);
		return;
	}

	g_free(device->alias);
	device->alias = g_str_equal(alias, "") ? NULL : g_strdup(alias);

	store_device_info(device);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Alias");

	g_dbus_pending_property_success(id);
}

static void dev_property_set_alias(const GDBusPropertyTable *property,
					DBusMessageIter *value,
					GDBusPendingPropertySet id, void *data)
{
	const char *alias;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_STRING) {
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	dbus_message_iter_get_basic(value, &alias);

	set_alias(id, alias, data);
}

static gboolean dev_property_exists_class(const GDBusPropertyTable *property,
								void *data)
{
	struct btd_device *device = data;

	return device->class != 0;
}

static gboolean dev_property_get_class(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;

	if (device->class == 0)
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &device->class);

	return TRUE;
}

static gboolean get_appearance(const GDBusPropertyTable *property, void *data,
							uint16_t *appearance)
{
	struct btd_device *device = data;

	if (dev_property_exists_class(property, data))
		return FALSE;

	if (device->appearance) {
		*appearance = device->appearance;
		return TRUE;
	}

	return FALSE;
}

static gboolean dev_property_exists_appearance(
			const GDBusPropertyTable *property, void *data)
{
	uint16_t appearance;

	return get_appearance(property, data, &appearance);
}

static gboolean dev_property_get_appearance(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	uint16_t appearance;

	if (!get_appearance(property, data, &appearance))
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT16, &appearance);

	return TRUE;
}

static const char *get_icon(const GDBusPropertyTable *property, void *data)
{
	struct btd_device *device = data;
	const char *icon = NULL;
	uint16_t appearance;

	if (device->class != 0)
		icon = class_to_icon(device->class);
	else if (get_appearance(property, data, &appearance))
		icon = gap_appearance_to_icon(appearance);

	return icon;
}

static gboolean dev_property_exists_icon(
			const GDBusPropertyTable *property, void *data)
{
	return get_icon(property, data) != NULL;
}

static gboolean dev_property_get_icon(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	const char *icon;

	icon = get_icon(property, data);
	if (icon == NULL)
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &icon);

	return TRUE;
}

static gboolean dev_property_get_paired(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	gboolean val = device_is_paired(device);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &val);

	return TRUE;
}

static gboolean dev_property_get_legacy(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	dbus_bool_t val = device->legacy;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &val);

	return TRUE;
}

static gboolean dev_property_get_rssi(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *dev = data;
	dbus_int16_t val = dev->rssi;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_INT16, &val);

	return TRUE;
}

static gboolean dev_property_exists_rssi(const GDBusPropertyTable *property,
								void *data)
{
	struct btd_device *dev = data;

	if (dev->rssi == 0)
		return FALSE;

	return TRUE;
}

static gboolean dev_property_get_trusted(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	gboolean val = device_is_trusted(device);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &val);

	return TRUE;
}

static void set_trust(GDBusPendingPropertySet id, gboolean value, void *data)
{
	struct btd_device *device = data;

	device_set_trusted(device, value);

	g_dbus_pending_property_success(id);
}

static void dev_property_set_trusted(const GDBusPropertyTable *property,
					DBusMessageIter *value,
					GDBusPendingPropertySet id, void *data)
{
	dbus_bool_t b;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_BOOLEAN) {
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	dbus_message_iter_get_basic(value, &b);

	set_trust(id, b, data);
}

static gboolean dev_property_get_blocked(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN,
							&device->blocked);

	return TRUE;
}

static void set_blocked(GDBusPendingPropertySet id, gboolean value, void *data)
{
	struct btd_device *device = data;
	int err;

	if (value)
		err = device_block(device, FALSE);
	else
		err = device_unblock(device, FALSE, FALSE);

	switch (-err) {
	case 0:
		g_dbus_pending_property_success(id);
		break;
	case EINVAL:
		g_dbus_pending_property_error(id, ERROR_INTERFACE ".Failed",
					"Kernel lacks blacklist support");
		break;
	default:
		g_dbus_pending_property_error(id, ERROR_INTERFACE ".Failed",
							strerror(-err));
		break;
	}
}


static void dev_property_set_blocked(const GDBusPropertyTable *property,
					DBusMessageIter *value,
					GDBusPendingPropertySet id, void *data)
{
	dbus_bool_t b;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_BOOLEAN) {
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	dbus_message_iter_get_basic(value, &b);

	set_blocked(id, b, data);
}

static gboolean dev_property_get_connected(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN,
							&device->connected);

	return TRUE;
}

static gboolean dev_property_get_uuids(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	DBusMessageIter entry;
	GSList *l;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &entry);

	if (!device->svc_resolved)
		l = device->eir_uuids;
	else
		l = device->uuids;

	for (; l != NULL; l = l->next)
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
							&l->data);

	dbus_message_iter_close_container(iter, &entry);

	return TRUE;
}

static gboolean dev_property_get_modalias(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;

	if (!device->modalias)
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING,
							&device->modalias);

	return TRUE;
}

static gboolean dev_property_exists_modalias(const GDBusPropertyTable *property,
								void *data)
{
	struct btd_device *device = data;

	return device->modalias ? TRUE : FALSE;
}

static gboolean dev_property_get_adapter(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct btd_device *device = data;
	const char *str = adapter_get_path(device->adapter);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &str);

	return TRUE;
}

static void profile_remove(gpointer data, gpointer user_data)
{
	struct btd_profile *profile = data;
	struct btd_device *device = user_data;

	profile->device_remove(profile, device);
}

static gboolean do_disconnect(gpointer user_data)
{
	struct btd_device *device = user_data;

	device->disconn_timer = 0;

	btd_adapter_disconnect_device(device->adapter, &device->bdaddr,
							device->bdaddr_type);

	return FALSE;
}

int device_block(struct btd_device *device, gboolean update_only)
{
	int err = 0;

	if (device->blocked)
		return 0;

	if (device->connected)
		do_disconnect(device);

	g_slist_foreach(device->profiles, profile_remove, device);
	g_slist_free(device->profiles);
	device->profiles = NULL;

	if (!update_only)
		err = btd_adapter_block_address(device->adapter,
					&device->bdaddr, device->bdaddr_type);

	if (err < 0)
		return err;

	device->blocked = TRUE;

	store_device_info(device);

	device_set_temporary(device, FALSE);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Blocked");

	return 0;
}

int device_unblock(struct btd_device *device, gboolean silent,
							gboolean update_only)
{
	int err = 0;

	if (!device->blocked)
		return 0;

	if (!update_only)
		err = btd_adapter_unblock_address(device->adapter,
					&device->bdaddr, device->bdaddr_type);

	if (err < 0)
		return err;

	device->blocked = FALSE;

	store_device_info(device);

	if (!silent) {
		g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Blocked");
		device_probe_profiles(device, device->uuids);
	}

	return 0;
}

static void discover_services_req_exit(DBusConnection *conn, void *user_data)
{
	struct browse_req *req = user_data;

	DBG("DiscoverServices requestor exited");

	browse_request_cancel(req);
}

static void bonding_request_cancel(struct bonding_req *bonding)
{
	struct btd_device *device = bonding->device;
	struct btd_adapter *adapter = device->adapter;

	adapter_cancel_bonding(adapter, &device->bdaddr, device->bdaddr_type);
}

static void dev_disconn_profile(gpointer a, gpointer b)
{
	struct btd_profile *profile = a;
	struct btd_device *dev = b;

	if (!profile->disconnect)
		return;

	profile->disconnect(dev, profile);
}

void device_request_disconnect(struct btd_device *device, DBusMessage *msg)
{
	if (device->bonding)
		bonding_request_cancel(device->bonding);

	if (device->browse)
		browse_request_cancel(device->browse);

	if (device->connect) {
		DBusMessage *reply = btd_error_failed(device->connect,
								"Cancelled");
		g_dbus_send_message(dbus_conn, reply);
		dbus_message_unref(device->connect);
		device->connect = NULL;
	}

	if (msg)
		device->disconnects = g_slist_append(device->disconnects,
						dbus_message_ref(msg));

	if (device->disconn_timer)
		return;

	g_slist_foreach(device->connected_profiles, dev_disconn_profile,
								device);
	g_slist_free(device->connected_profiles);
	device->connected_profiles = NULL;

	g_slist_free(device->pending);
	device->pending = NULL;

	while (device->watches) {
		struct btd_disconnect_data *data = device->watches->data;

		if (data->watch)
			/* temporary is set if device is going to be removed */
			data->watch(device, device->temporary,
							data->user_data);

		/* Check if the watch has been removed by callback function */
		if (!g_slist_find(device->watches, data))
			continue;

		device->watches = g_slist_remove(device->watches, data);
		g_free(data);
	}

	device->disconn_timer = g_timeout_add_seconds(DISCONNECT_TIMER,
						do_disconnect, device);
}

static DBusMessage *disconnect(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct btd_device *device = user_data;

	if (!device->connected)
		return btd_error_not_connected(msg);

	/*
	 * Disable connections through passive scanning until
	 * Device1.Connect is called
	 */
	if (device->auto_connect)
		device->disable_auto_connect = TRUE;

	device_request_disconnect(device, msg);

	return NULL;
}

static int connect_next(struct btd_device *dev)
{
	struct btd_profile *profile;
	int err = -ENOENT;

	while (dev->pending) {
		int err;

		profile = dev->pending->data;

		err = profile->connect(dev, profile);
		if (err == 0)
			return 0;

		error("Failed to connect %s: %s", profile->name,
							strerror(-err));
		dev->pending = g_slist_remove(dev->pending, profile);
	}

	return err;
}

void device_profile_connected(struct btd_device *dev,
					struct btd_profile *profile, int err)
{
	struct btd_profile *pending;

	DBG("%s %s (%d)", profile->name, strerror(-err), -err);

	if (dev->pending == NULL)
		return;

	pending = dev->pending->data;
	dev->pending = g_slist_remove(dev->pending, profile);

	if (!err)
		dev->connected_profiles =
				g_slist_append(dev->connected_profiles,
								profile);

	/* Only continue connecting the next profile if it matches the first
	 * pending, otherwise it will trigger another connect to the same
	 * profile
	 */
	if (profile != pending)
		return;

	if (connect_next(dev) == 0)
		return;

	if (!dev->connect)
		return;

	if (!err && dbus_message_is_method_call(dev->connect, DEVICE_INTERFACE,
								"Connect"))
		dev->general_connect = TRUE;

	DBG("returning response to %s", dbus_message_get_sender(dev->connect));

	if (err && dev->connected_profiles == NULL)
		g_dbus_send_message(dbus_conn,
				btd_error_failed(dev->connect, strerror(-err)));
	else
		g_dbus_send_reply(dbus_conn, dev->connect, DBUS_TYPE_INVALID);

	g_slist_free(dev->pending);
	dev->pending = NULL;

	dbus_message_unref(dev->connect);
	dev->connect = NULL;
}

void device_add_eir_uuids(struct btd_device *dev, GSList *uuids)
{
	GSList *l;
	bool added = false;

	if (dev->svc_resolved)
		return;

	for (l = uuids; l != NULL; l = l->next) {
		const char *str = l->data;
		if (g_slist_find_custom(dev->eir_uuids, str, bt_uuid_strcmp))
			continue;
		added = true;
		dev->eir_uuids = g_slist_append(dev->eir_uuids, g_strdup(str));
	}

	if (added)
		g_dbus_emit_property_changed(dbus_conn, dev->path,
						DEVICE_INTERFACE, "UUIDs");
}

static int device_resolve_svc(struct btd_device *dev, DBusMessage *msg)
{
	if (device_is_bredr(dev))
		return device_browse_sdp(dev, msg, FALSE);
	else
		return device_browse_primary(dev, msg, FALSE);
}

static struct btd_profile *find_connectable_profile(struct btd_device *dev,
							const char *uuid)
{
	GSList *l;

	for (l = dev->profiles; l != NULL; l = g_slist_next(l)) {
		struct btd_profile *p = l->data;

		if (!p->connect || !p->remote_uuid)
			continue;

		if (strcasecmp(uuid, p->remote_uuid) == 0)
			return p;
	}

	return NULL;
}

static gint profile_prio_cmp(gconstpointer a, gconstpointer b)
{
	const struct btd_profile *p1 = a, *p2 = b;

	return p2->priority - p1->priority;
}

static DBusMessage *connect_profiles(struct btd_device *dev, DBusMessage *msg,
							const char *uuid)
{
	struct btd_profile *p;
	GSList *l;
	int err;

	DBG("%s %s, client %s", dev->path, uuid ? uuid : "(all)",
						dbus_message_get_sender(msg));

	if (dev->pending || dev->connect || dev->browse)
		return btd_error_in_progress(msg);

	device_set_temporary(dev, FALSE);

	if (!dev->svc_resolved) {
		err = device_resolve_svc(dev, msg);
		if (err < 0)
			return btd_error_failed(msg, strerror(-err));
		return NULL;
	}

	if (uuid) {
		p = find_connectable_profile(dev, uuid);
		if (!p)
			return btd_error_invalid_args(msg);

		dev->pending = g_slist_prepend(dev->pending, p);

		goto start_connect;
	}

	for (l = dev->profiles; l != NULL; l = g_slist_next(l)) {
		p = l->data;

		if (!p->auto_connect)
			continue;

		if (g_slist_find(dev->pending, p))
			continue;

		if (g_slist_find(dev->connected_profiles, p))
			continue;

		dev->pending = g_slist_insert_sorted(dev->pending, p,
							profile_prio_cmp);
	}

	if (!dev->pending)
		return btd_error_not_available(msg);

start_connect:
	err = connect_next(dev);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	dev->connect = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *dev_connect(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct btd_device *dev = user_data;

	if (device_is_le(dev)) {
		int err;

		if (device_is_connected(dev))
			return dbus_message_new_method_return(msg);

		device_set_temporary(dev, FALSE);

		dev->disable_auto_connect = FALSE;

		err = device_connect_le(dev);
		if (err < 0)
			return btd_error_failed(msg, strerror(-err));

		dev->connect = dbus_message_ref(msg);

		return NULL;
	}

	return connect_profiles(dev, msg, NULL);
}

static DBusMessage *connect_profile(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct btd_device *dev = user_data;
	const char *pattern;
	char *uuid;
	DBusMessage *reply;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	uuid = bt_name2string(pattern);
	reply = connect_profiles(dev, msg, uuid);
	g_free(uuid);

	return reply;
}

void device_profile_disconnected(struct btd_device *dev,
					struct btd_profile *profile, int err)
{
	dev->connected_profiles = g_slist_remove(dev->connected_profiles,
								profile);

	if (!dev->disconnect)
		return;

	if (err)
		g_dbus_send_message(dbus_conn,
					btd_error_failed(dev->disconnect,
							strerror(-err)));
	else
		g_dbus_send_reply(dbus_conn, dev->disconnect,
							DBUS_TYPE_INVALID);

	dbus_message_unref(dev->disconnect);
	dev->disconnect = NULL;
}

static DBusMessage *disconnect_profile(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct btd_device *dev = user_data;
	struct btd_profile *p;
	const char *pattern;
	char *uuid;
	int err;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	uuid = bt_name2string(pattern);
	if (uuid == NULL)
		return btd_error_invalid_args(msg);

	p = find_connectable_profile(dev, uuid);
	g_free(uuid);

	if (!p)
		return btd_error_invalid_args(msg);

	if (!p->disconnect)
		return btd_error_not_supported(msg);

	err = p->disconnect(dev, p);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	dev->disconnect = dbus_message_ref(msg);

	return NULL;
}

static void device_svc_resolved(struct btd_device *dev, int err)
{
	DBusMessage *reply;
	struct browse_req *req = dev->browse;

	dev->svc_resolved = true;
	dev->browse = NULL;

	g_slist_free_full(dev->eir_uuids, g_free);
	dev->eir_uuids = NULL;

	while (dev->svc_callbacks) {
		struct svc_callback *cb = dev->svc_callbacks->data;

		if (cb->idle_id > 0)
			g_source_remove(cb->idle_id);

		cb->func(dev, err, cb->user_data);

		dev->svc_callbacks = g_slist_delete_link(dev->svc_callbacks,
							dev->svc_callbacks);
		g_free(cb);
	}

	if (!req || !req->msg)
		return;

	if (dbus_message_is_method_call(req->msg, DEVICE_INTERFACE,
								"Pair")) {
		reply = dbus_message_new_method_return(req->msg);
		g_dbus_send_message(dbus_conn, reply);
		return;
	}

	if (err) {
		reply = btd_error_failed(req->msg, strerror(-err));
		g_dbus_send_message(dbus_conn, reply);
		return;
	}

	if (dbus_message_is_method_call(req->msg, DEVICE_INTERFACE, "Connect"))
		reply = dev_connect(dbus_conn, req->msg, dev);
	else if (dbus_message_is_method_call(req->msg, DEVICE_INTERFACE,
							"ConnectProfile"))
		reply = connect_profile(dbus_conn, req->msg, dev);
	else
		return;

	dbus_message_unref(req->msg);
	req->msg = NULL;

	if (reply)
		g_dbus_send_message(dbus_conn, reply);
}

static struct bonding_req *bonding_request_new(DBusMessage *msg,
						struct btd_device *device,
						struct agent *agent)
{
	struct bonding_req *bonding;
	char addr[18];

	ba2str(&device->bdaddr, addr);
	DBG("Requesting bonding for %s", addr);

	bonding = g_new0(struct bonding_req, 1);

	bonding->msg = dbus_message_ref(msg);

	if (agent)
		bonding->agent = agent_ref(agent);

	return bonding;
}

static void create_bond_req_exit(DBusConnection *conn, void *user_data)
{
	struct btd_device *device = user_data;
	char addr[18];

	ba2str(&device->bdaddr, addr);
	DBG("%s: requestor exited before bonding was completed", addr);

	if (device->authr)
		device_cancel_authentication(device, FALSE);

	if (device->bonding) {
		device->bonding->listener_id = 0;
		device_request_disconnect(device, NULL);
	}
}

static DBusMessage *pair_device(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct btd_device *device = data;
	struct btd_adapter *adapter = device->adapter;
	const char *sender;
	struct agent *agent;
	struct bonding_req *bonding;
	uint8_t io_cap;
	int err;

	device_set_temporary(device, FALSE);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	if (device->bonding)
		return btd_error_in_progress(msg);

	if (device_is_bonded(device))
		return btd_error_already_exists(msg);

	sender = dbus_message_get_sender(msg);

	agent = agent_get(sender);
	if (agent)
		io_cap = agent_get_io_capability(agent);
	else
		io_cap = IO_CAPABILITY_NOINPUTNOOUTPUT;

	bonding = bonding_request_new(msg, device, agent);

	if (agent)
		agent_unref(agent);

	bonding->listener_id = g_dbus_add_disconnect_watch(dbus_conn,
						sender, create_bond_req_exit,
						device, NULL);

	device->bonding = bonding;
	bonding->device = device;

	/* Due to a bug in the kernel we might loose out on ATT commands
	 * that arrive during the SMP procedure, so connect the ATT
	 * channel first and only then start pairing (there's code for
	 * this in the ATT connect callback)
	 */
	if (device_is_le(device) && !device_is_connected(device))
		err = device_connect_le(device);
	else
		err = adapter_create_bonding(adapter, &device->bdaddr,
						device->bdaddr_type, io_cap);

	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	return NULL;
}

static DBusMessage *new_authentication_return(DBusMessage *msg, uint8_t status)
{
	switch (status) {
	case MGMT_STATUS_SUCCESS:
		return dbus_message_new_method_return(msg);

	case MGMT_STATUS_CONNECT_FAILED:
		return dbus_message_new_error(msg,
				ERROR_INTERFACE ".ConnectionAttemptFailed",
				"Page Timeout");
	case MGMT_STATUS_TIMEOUT:
		return dbus_message_new_error(msg,
				ERROR_INTERFACE ".AuthenticationTimeout",
				"Authentication Timeout");
	case MGMT_STATUS_BUSY:
	case MGMT_STATUS_REJECTED:
		return dbus_message_new_error(msg,
				ERROR_INTERFACE ".AuthenticationRejected",
				"Authentication Rejected");
	case MGMT_STATUS_CANCELLED:
	case MGMT_STATUS_NO_RESOURCES:
	case MGMT_STATUS_DISCONNECTED:
		return dbus_message_new_error(msg,
				ERROR_INTERFACE ".AuthenticationCanceled",
				"Authentication Canceled");
	default:
		return dbus_message_new_error(msg,
				ERROR_INTERFACE ".AuthenticationFailed",
				"Authentication Failed");
	}
}

static void bonding_request_free(struct bonding_req *bonding)
{
	if (!bonding)
		return;

	if (bonding->listener_id)
		g_dbus_remove_watch(dbus_conn, bonding->listener_id);

	if (bonding->msg)
		dbus_message_unref(bonding->msg);

	if (bonding->agent) {
		agent_cancel(bonding->agent);
		agent_unref(bonding->agent);
		bonding->agent = NULL;
	}

	if (bonding->device)
		bonding->device->bonding = NULL;

	g_free(bonding);
}

static void device_cancel_bonding(struct btd_device *device, uint8_t status)
{
	struct bonding_req *bonding = device->bonding;
	DBusMessage *reply;
	char addr[18];

	if (!bonding)
		return;

	ba2str(&device->bdaddr, addr);
	DBG("Canceling bonding request for %s", addr);

	if (device->authr)
		device_cancel_authentication(device, FALSE);

	reply = new_authentication_return(bonding->msg, status);
	g_dbus_send_message(dbus_conn, reply);

	bonding_request_cancel(bonding);
	bonding_request_free(bonding);
}

static DBusMessage *cancel_pairing(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct btd_device *device = data;
	struct bonding_req *req = device->bonding;

	DBG("");

	if (!req)
		return btd_error_does_not_exist(msg);

	device_cancel_bonding(device, MGMT_STATUS_CANCELLED);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable device_methods[] = {
	{ GDBUS_ASYNC_METHOD("Disconnect", NULL, NULL, disconnect) },
	{ GDBUS_ASYNC_METHOD("Connect", NULL, NULL, dev_connect) },
	{ GDBUS_ASYNC_METHOD("ConnectProfile", GDBUS_ARGS({ "UUID", "s" }),
						NULL, connect_profile) },
	{ GDBUS_ASYNC_METHOD("DisconnectProfile", GDBUS_ARGS({ "UUID", "s" }),
						NULL, disconnect_profile) },
	{ GDBUS_ASYNC_METHOD("Pair", NULL, NULL, pair_device) },
	{ GDBUS_METHOD("CancelPairing", NULL, NULL, cancel_pairing) },
	{ }
};

static const GDBusPropertyTable device_properties[] = {
	{ "Address", "s", dev_property_get_address },
	{ "Name", "s", dev_property_get_name, NULL, dev_property_exists_name },
	{ "Alias", "s", dev_property_get_alias, dev_property_set_alias },
	{ "Class", "u", dev_property_get_class, NULL,
					dev_property_exists_class },
	{ "Appearance", "q", dev_property_get_appearance, NULL,
					dev_property_exists_appearance },
	{ "Icon", "s", dev_property_get_icon, NULL,
					dev_property_exists_icon },
	{ "Paired", "b", dev_property_get_paired },
	{ "Trusted", "b", dev_property_get_trusted, dev_property_set_trusted },
	{ "Blocked", "b", dev_property_get_blocked, dev_property_set_blocked },
	{ "LegacyPairing", "b", dev_property_get_legacy },
	{ "RSSI", "n", dev_property_get_rssi, NULL, dev_property_exists_rssi },
	{ "Connected", "b", dev_property_get_connected },
	{ "UUIDs", "as", dev_property_get_uuids },
	{ "Modalias", "s", dev_property_get_modalias, NULL,
						dev_property_exists_modalias },
	{ "Adapter", "o", dev_property_get_adapter },
	{ }
};

gboolean device_is_connected(struct btd_device *device)
{
	return device->connected;
}

void device_add_connection(struct btd_device *device)
{
	if (device->connected) {
		char addr[18];
		ba2str(&device->bdaddr, addr);
		error("Device %s is already connected", addr);
		return;
	}

	device_set_temporary(device, FALSE);

	device->connected = TRUE;

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Connected");
}

void device_remove_connection(struct btd_device *device)
{
	if (!device->connected) {
		char addr[18];
		ba2str(&device->bdaddr, addr);
		error("Device %s isn't connected", addr);
		return;
	}

	device->connected = FALSE;
	device->general_connect = FALSE;

	if (device->disconn_timer > 0) {
		g_source_remove(device->disconn_timer);
		device->disconn_timer = 0;
	}

	while (device->disconnects) {
		DBusMessage *msg = device->disconnects->data;

		g_dbus_send_reply(dbus_conn, msg, DBUS_TYPE_INVALID);
		device->disconnects = g_slist_remove(device->disconnects, msg);
		dbus_message_unref(msg);
	}

	if (device_is_paired(device) && !device_is_bonded(device))
		device_set_paired(device, FALSE);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Connected");
}

guint device_add_disconnect_watch(struct btd_device *device,
				disconnect_watch watch, void *user_data,
				GDestroyNotify destroy)
{
	struct btd_disconnect_data *data;
	static guint id = 0;

	data = g_new0(struct btd_disconnect_data, 1);
	data->id = ++id;
	data->watch = watch;
	data->user_data = user_data;
	data->destroy = destroy;

	device->watches = g_slist_append(device->watches, data);

	return data->id;
}

void device_remove_disconnect_watch(struct btd_device *device, guint id)
{
	GSList *l;

	for (l = device->watches; l; l = l->next) {
		struct btd_disconnect_data *data = l->data;

		if (data->id == id) {
			device->watches = g_slist_remove(device->watches,
							data);
			if (data->destroy)
				data->destroy(data->user_data);
			g_free(data);
			return;
		}
	}
}

static char *load_cached_name(struct btd_device *device, const char *local,
				const char *peer)
{
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char *str = NULL;
	int len;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/cache/%s", local, peer);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();

	if (!g_key_file_load_from_file(key_file, filename, 0, NULL))
		goto failed;

	str = g_key_file_get_string(key_file, "General", "Name", NULL);
	if (str) {
		len = strlen(str);
		if (len > HCI_MAX_NAME_LENGTH)
			str[HCI_MAX_NAME_LENGTH] = '\0';
	}

failed:
	g_key_file_free(key_file);

	return str;
}

static void load_info(struct btd_device *device, const char *local,
			const char *peer, GKeyFile *key_file)
{
	char *str;
	gboolean store_needed = FALSE;
	gboolean blocked;
	char **uuids;
	int source, vendor, product, version;
	char **techno, **t;
	gboolean bredr = FALSE;
	gboolean le = FALSE;

	/* Load device name from storage info file, if that fails fall back to
	 * the cache.
	 */
	str = g_key_file_get_string(key_file, "General", "Name", NULL);
	if (str == NULL) {
		str = load_cached_name(device, local, peer);
		if (str)
			store_needed = TRUE;
	}

	if (str) {
		strcpy(device->name, str);
		g_free(str);
	}

	/* Load alias */
	device->alias = g_key_file_get_string(key_file, "General", "Alias",
									NULL);

	/* Load class */
	str = g_key_file_get_string(key_file, "General", "Class", NULL);
	if (str) {
		uint32_t class;

		if (sscanf(str, "%x", &class) == 1)
			device->class = class;
		g_free(str);
	}

	/* Load appearance */
	str = g_key_file_get_string(key_file, "General", "Appearance", NULL);
	if (str) {
		device->appearance = strtol(str, NULL, 16);
		g_free(str);
	}

	/* Load device technology */
	techno = g_key_file_get_string_list(key_file, "General",
					"SupportedTechnologies", NULL, NULL);
	if (!techno)
		goto next;

	for (t = techno; *t; t++) {
		if (g_str_equal(*t, "BR/EDR"))
			bredr = TRUE;
		else if (g_str_equal(*t, "LE"))
			le = TRUE;
		else
			error("Unknown device technology");
	}

	if (bredr && le) {
		/* TODO: Add correct type for dual mode device */
	} else if (bredr) {
		device->bdaddr_type = BDADDR_BREDR;
	} else if (le) {
		str = g_key_file_get_string(key_file, "General",
						"AddressType", NULL);

		if (str && g_str_equal(str, "public"))
			device->bdaddr_type = BDADDR_LE_PUBLIC;
		else if (str && g_str_equal(str, "static"))
			device->bdaddr_type = BDADDR_LE_RANDOM;
		else
			error("Unknown LE device technology");

		g_free(str);
	}

	g_strfreev(techno);

next:
	/* Load trust */
	device->trusted = g_key_file_get_boolean(key_file, "General",
							"Trusted", NULL);

	/* Load device blocked */
	blocked = g_key_file_get_boolean(key_file, "General", "Blocked", NULL);
	if (blocked)
		device_block(device, FALSE);

	/* Load device profile list */
	uuids = g_key_file_get_string_list(key_file, "General", "Services",
						NULL, NULL);
	if (uuids) {
		char **uuid;

		for (uuid = uuids; *uuid; uuid++) {
			GSList *match;

			match = g_slist_find_custom(device->uuids, *uuid,
							bt_uuid_strcmp);
			if (match)
				continue;

			device->uuids = g_slist_insert_sorted(device->uuids,
								g_strdup(*uuid),
								bt_uuid_strcmp);
		}
		g_strfreev(uuids);

		/* Discovered services restored from storage */
		device->svc_resolved = true;
	}

	/* Load device id */
	source = g_key_file_get_integer(key_file, "DeviceID", "Source", NULL);
	if (source) {
		vendor = g_key_file_get_integer(key_file, "DeviceID",
							"Vendor", NULL);

		product = g_key_file_get_integer(key_file, "DeviceID",
							"Product", NULL);

		version = g_key_file_get_integer(key_file, "DeviceID",
							"Version", NULL);

		btd_device_set_pnpid(device, source, vendor, product, version);
	}

	if (store_needed)
		store_device_info(device);
}

static void load_att_info(struct btd_device *device, const char *local,
				const char *peer)
{
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char *prim_uuid, *str;
	char **groups, **handle, *service_uuid;
	struct gatt_primary *prim;
	uuid_t uuid;
	char tmp[3];
	int i;

	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	prim_uuid = bt_uuid2string(&uuid);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/attributes", local,
			peer);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);
	groups = g_key_file_get_groups(key_file, NULL);

	for (handle = groups; *handle; handle++) {
		gboolean uuid_ok;
		gint end;

		str = g_key_file_get_string(key_file, *handle, "UUID", NULL);
		if (!str)
			continue;

		uuid_ok = g_str_equal(str, prim_uuid);
		g_free(str);

		if (!uuid_ok)
			continue;

		str = g_key_file_get_string(key_file, *handle, "Value", NULL);
		if (!str)
			continue;

		end = g_key_file_get_integer(key_file, *handle,
						"EndGroupHandle", NULL);
		if (end == 0) {
			g_free(str);
			continue;
		}

		prim = g_new0(struct gatt_primary, 1);
		prim->range.start = atoi(*handle);
		prim->range.end = end;

		switch (strlen(str)) {
		case 4:
			uuid.type = SDP_UUID16;
			sscanf(str, "%04hx", &uuid.value.uuid16);
		break;
		case 8:
			uuid.type = SDP_UUID32;
			sscanf(str, "%08x", &uuid.value.uuid32);
			break;
		case 32:
			uuid.type = SDP_UUID128;
			memset(tmp, 0, sizeof(tmp));
			for (i = 0; i < 16; i++) {
				memcpy(tmp, str + (i * 2), 2);
				uuid.value.uuid128.data[i] =
						(uint8_t) strtol(tmp, NULL, 16);
			}
			break;
		default:
			g_free(str);
			g_free(prim);
			continue;
		}

		service_uuid = bt_uuid2string(&uuid);
		memcpy(prim->uuid, service_uuid, MAX_LEN_UUID_STR);
		g_free(service_uuid);
		g_free(str);

		device->primaries = g_slist_append(device->primaries, prim);
	}

	g_strfreev(groups);
	g_key_file_free(key_file);
	g_free(prim_uuid);
}

static struct btd_device *device_new(struct btd_adapter *adapter,
				const char *address)
{
	char *address_up;
	struct btd_device *device;
	const char *adapter_path = adapter_get_path(adapter);

	DBG("address %s", address);

	device = g_try_malloc0(sizeof(struct btd_device));
	if (device == NULL)
		return NULL;

	address_up = g_ascii_strup(address, -1);
	device->path = g_strdup_printf("%s/dev_%s", adapter_path, address_up);
	g_strdelimit(device->path, ":", '_');
	g_free(address_up);

	DBG("Creating device %s", device->path);

	if (g_dbus_register_interface(dbus_conn,
					device->path, DEVICE_INTERFACE,
					device_methods, NULL,
					device_properties, device,
					device_free) == FALSE) {
		error("Unable to register device interface for %s", address);
		device_free(device);
		return NULL;
	}

	str2ba(address, &device->bdaddr);
	device->adapter = adapter;

	return btd_device_ref(device);
}

struct btd_device *device_create_from_storage(struct btd_adapter *adapter,
				const char *address, GKeyFile *key_file)
{
	struct btd_device *device;
	const bdaddr_t *src;
	char srcaddr[18];

	DBG("address %s", address);

	device = device_new(adapter, address);
	if (device == NULL)
		return NULL;

	src = adapter_get_address(adapter);
	ba2str(src, srcaddr);

	load_info(device, srcaddr, address, key_file);
	load_att_info(device, srcaddr, address);

	return device;
}

struct btd_device *device_create(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, uint8_t bdaddr_type)
{
	struct btd_device *device;
	const bdaddr_t *sba;
	char src[18], dst[18];
	char *str;

	ba2str(bdaddr, dst);
	DBG("dst %s", dst);

	device = device_new(adapter, dst);
	if (device == NULL)
		return NULL;

	device->bdaddr_type = bdaddr_type;
	sba = adapter_get_address(adapter);
	ba2str(sba, src);

	str = load_cached_name(device, src, dst);
	if (str) {
		strcpy(device->name, str);
		g_free(str);
	}

	return device;
}

char *btd_device_get_storage_path(struct btd_device *device,
				const char *filename)
{
	char srcaddr[18], dstaddr[18];

	ba2str(adapter_get_address(device->adapter), srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	if (!filename)
		return g_strdup_printf(STORAGEDIR "/%s/%s", srcaddr, dstaddr);

	return g_strdup_printf(STORAGEDIR "/%s/%s/%s", srcaddr, dstaddr,
							filename);
}

void device_set_name(struct btd_device *device, const char *name)
{
	if (strncmp(name, device->name, MAX_NAME_LENGTH) == 0)
		return;

	DBG("%s %s", device->path, name);

	strncpy(device->name, name, MAX_NAME_LENGTH);

	store_device_info(device);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Name");

	if (device->alias != NULL)
		return;

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Alias");
}

void device_get_name(struct btd_device *device, char *name, size_t len)
{
	strncpy(name, device->name, len);
}

bool device_name_known(struct btd_device *device)
{
	return device->name[0] != '\0';
}

void device_set_class(struct btd_device *device, uint32_t class)
{
	if (device->class == class)
		return;

	DBG("%s 0x%06X", device->path, class);

	device->class = class;

	store_device_info(device);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Class");
}

uint16_t btd_device_get_vendor(struct btd_device *device)
{
	return device->vendor;
}

uint16_t btd_device_get_vendor_src(struct btd_device *device)
{
	return device->vendor_src;
}

uint16_t btd_device_get_product(struct btd_device *device)
{
	return device->product;
}

uint16_t btd_device_get_version(struct btd_device *device)
{
	return device->version;
}

static void delete_folder_tree(const char *dirname)
{
	DIR *dir;
	struct dirent *entry;
	char filename[PATH_MAX + 1];

	dir = opendir(dirname);
	if (dir == NULL)
		return;

	while ((entry = readdir(dir)) != NULL) {
		if (g_str_equal(entry->d_name, ".") ||
				g_str_equal(entry->d_name, ".."))
			continue;

		snprintf(filename, PATH_MAX, "%s/%s", dirname, entry->d_name);
		filename[PATH_MAX] = '\0';

		if (entry->d_type == DT_DIR)
			delete_folder_tree(filename);
		else
			unlink(filename);
	}
	closedir(dir);

	rmdir(dirname);
}

static void device_remove_stored(struct btd_device *device)
{
	const bdaddr_t *src = adapter_get_address(device->adapter);
	uint8_t dst_type = device->bdaddr_type;
	char adapter_addr[18];
	char device_addr[18];
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char *data;
	gsize length = 0;

	if (device_is_bonded(device)) {
		device_set_bonded(device, FALSE);
		device->paired = FALSE;
		btd_adapter_remove_bonding(device->adapter, &device->bdaddr,
								dst_type);
	}

	if (device->blocked)
		device_unblock(device, TRUE, FALSE);

	ba2str(src, adapter_addr);
	ba2str(&device->bdaddr, device_addr);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s", adapter_addr,
			device_addr);
	filename[PATH_MAX] = '\0';
	delete_folder_tree(filename);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/cache/%s", adapter_addr,
			device_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);
	g_key_file_remove_group(key_file, "ServiceRecords", NULL);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);
	g_key_file_free(key_file);
}

void device_remove(struct btd_device *device, gboolean remove_stored)
{

	DBG("Removing device %s", device->path);

	if (device->bonding) {
		uint8_t status;

		if (device->connected)
			status = MGMT_STATUS_DISCONNECTED;
		else
			status = MGMT_STATUS_CONNECT_FAILED;

		device_cancel_bonding(device, status);
	}

	if (device->browse)
		browse_request_cancel(device->browse);

	g_slist_foreach(device->connected_profiles, dev_disconn_profile,
								device);
	g_slist_free(device->connected_profiles);
	device->connected_profiles = NULL;

	g_slist_free(device->pending);
	device->pending = NULL;

	if (device->connected)
		do_disconnect(device);

	if (device->store_id > 0) {
		g_source_remove(device->store_id);
		device->store_id = 0;

		if (!remove_stored)
			store_device_info_cb(device);
	}

	if (remove_stored)
		device_remove_stored(device);

	g_slist_foreach(device->profiles, profile_remove, device);
	g_slist_free(device->profiles);
	device->profiles = NULL;

	btd_device_unref(device);
}

gint device_address_cmp(gconstpointer a, gconstpointer b)
{
	const struct btd_device *device = a;
	const char *address = b;
	char addr[18];

	ba2str(&device->bdaddr, addr);
	return strcasecmp(addr, address);
}

gint device_bdaddr_cmp(gconstpointer a, gconstpointer b)
{
	const struct btd_device *device = a;
	const bdaddr_t *bdaddr = b;

	return bacmp(&device->bdaddr, bdaddr);
}

static gboolean record_has_uuid(const sdp_record_t *rec,
				const char *profile_uuid)
{
	sdp_list_t *pat;

	for (pat = rec->pattern; pat != NULL; pat = pat->next) {
		char *uuid;
		int ret;

		uuid = bt_uuid2string(pat->data);
		if (!uuid)
			continue;

		ret = strcasecmp(uuid, profile_uuid);

		g_free(uuid);

		if (ret == 0)
			return TRUE;
	}

	return FALSE;
}

GSList *device_get_uuids(struct btd_device *device)
{
	return device->uuids;
}

static bool device_match_profile(struct btd_device *device,
					struct btd_profile *profile,
					GSList *uuids)
{
	if (profile->remote_uuid == NULL)
		return false;

	if (g_slist_find_custom(uuids, profile->remote_uuid,
							bt_uuid_strcmp) == NULL)
		return false;

	return true;
}

struct probe_data {
	struct btd_device *dev;
	GSList *uuids;
	char addr[18];
};

static void dev_probe(struct btd_profile *p, void *user_data)
{
	struct probe_data *d = user_data;
	GSList *probe_uuids;
	int err;

	if (p->device_probe == NULL)
		return;

	if (!device_match_profile(d->dev, p, d->uuids))
		return;

	probe_uuids = g_slist_append(NULL, (char *) p->remote_uuid);

	err = p->device_probe(p, d->dev, probe_uuids);
	if (err < 0) {
		error("%s profile probe failed for %s", p->name, d->addr);
		g_slist_free(probe_uuids);
		return;
	}

	d->dev->profiles = g_slist_append(d->dev->profiles, p);
	g_slist_free(probe_uuids);
}

void device_probe_profile(gpointer a, gpointer b)
{
	struct btd_device *device = a;
	struct btd_profile *profile = b;
	GSList *probe_uuids;
	char addr[18];
	int err;

	if (profile->device_probe == NULL)
		return;

	if (!device_match_profile(device, profile, device->uuids))
		return;

	probe_uuids = g_slist_append(NULL, (char *) profile->remote_uuid);

	ba2str(&device->bdaddr, addr);

	err = profile->device_probe(profile, device, probe_uuids);
	if (err < 0) {
		error("%s profile probe failed for %s", profile->name, addr);
		g_slist_free(probe_uuids);
		return;
	}

	device->profiles = g_slist_append(device->profiles, profile);
	g_slist_free(probe_uuids);

	if (!profile->auto_connect || !device->general_connect)
		return;

	device->pending = g_slist_append(device->pending, profile);

	if (g_slist_length(device->pending) == 1)
		connect_next(device);
}

void device_remove_profile(gpointer a, gpointer b)
{
	struct btd_device *device = a;
	struct btd_profile *profile = b;

	if (!g_slist_find(device->profiles, profile))
		return;

	device->connected_profiles = g_slist_remove(device->connected_profiles,
								profile);
	device->profiles = g_slist_remove(device->profiles, profile);

	profile->device_remove(profile, device);
}

void device_probe_profiles(struct btd_device *device, GSList *uuids)
{
	struct probe_data d = { device, uuids };
	GSList *l;

	ba2str(&device->bdaddr, d.addr);

	if (device->blocked) {
		DBG("Skipping profiles for blocked device %s", d.addr);
		goto add_uuids;
	}

	DBG("Probing profiles for device %s", d.addr);

	btd_profile_foreach(dev_probe, &d);

add_uuids:
	for (l = uuids; l != NULL; l = g_slist_next(l)) {
		GSList *match = g_slist_find_custom(device->uuids, l->data,
							bt_uuid_strcmp);
		if (match)
			continue;

		device->uuids = g_slist_insert_sorted(device->uuids,
						g_strdup(l->data),
						bt_uuid_strcmp);
	}

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "UUIDs");
}

static void device_remove_profiles(struct btd_device *device, GSList *uuids)
{
	char srcaddr[18], dstaddr[18];
	GSList *l, *next;

	ba2str(adapter_get_address(device->adapter), srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	DBG("Removing profiles for %s", dstaddr);

	g_slist_free(device->uuids);
	device->uuids = NULL;
	store_device_info(device);

	for (l = device->profiles; l != NULL; l = next) {
		struct btd_profile *profile = l->data;

		next = l->next;
		if (device_match_profile(device, profile, device->uuids))
			continue;

		profile->device_remove(profile, device);
		device->profiles = g_slist_remove(device->profiles, profile);
	}
}

static void store_sdp_record(GKeyFile *key_file, sdp_record_t *rec)
{
	char handle_str[11];
	sdp_buf_t buf;
	int size, i;
	char *str;

	sprintf(handle_str, "0x%8.8X", rec->handle);

	if (sdp_gen_record_pdu(rec, &buf) < 0)
		return;

	size = buf.data_size;

	str = g_malloc0(size*2+1);

	for (i = 0; i < size; i++)
		sprintf(str + (i * 2), "%02X", buf.data[i]);

	g_key_file_set_string(key_file, "ServiceRecords", handle_str, str);

	free(buf.data);
	g_free(str);
}

static void store_primaries_from_sdp_record(GKeyFile *key_file,
						sdp_record_t *rec)
{
	uuid_t uuid;
	char *att_uuid, *prim_uuid;
	uint16_t start = 0, end = 0, psm = 0;
	char handle[6], uuid_str[33];
	int i;

	sdp_uuid16_create(&uuid, ATT_UUID);
	att_uuid = bt_uuid2string(&uuid);

	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	prim_uuid = bt_uuid2string(&uuid);

	if (!record_has_uuid(rec, att_uuid))
		goto done;

	if (!gatt_parse_record(rec, &uuid, &psm, &start, &end))
		goto done;

	sprintf(handle, "%hu", start);
	switch (uuid.type) {
	case SDP_UUID16:
		sprintf(uuid_str, "%4.4X", uuid.value.uuid16);
		break;
	case SDP_UUID32:
		sprintf(uuid_str, "%8.8X", uuid.value.uuid32);
		break;
	case SDP_UUID128:
		for (i = 0; i < 16; i++)
			sprintf(uuid_str + (i * 2), "%2.2X",
					uuid.value.uuid128.data[i]);
		break;
	default:
		uuid_str[0] = '\0';
	}

	g_key_file_set_string(key_file, handle, "UUID", prim_uuid);
	g_key_file_set_string(key_file, handle, "Value", uuid_str);
	g_key_file_set_integer(key_file, handle, "EndGroupHandle", end);

done:
	g_free(prim_uuid);
	g_free(att_uuid);
}

static int rec_cmp(const void *a, const void *b)
{
	const sdp_record_t *r1 = a;
	const sdp_record_t *r2 = b;

	return r1->handle - r2->handle;
}

static void update_bredr_services(struct browse_req *req, sdp_list_t *recs)
{
	struct btd_device *device = req->device;
	sdp_list_t *seq;
	char srcaddr[18], dstaddr[18];
	char sdp_file[PATH_MAX + 1];
	char att_file[PATH_MAX + 1];
	GKeyFile *sdp_key_file = NULL;
	GKeyFile *att_key_file = NULL;
	char *data;
	gsize length = 0;

	ba2str(adapter_get_address(device->adapter), srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	if (!device->temporary) {
		snprintf(sdp_file, PATH_MAX, STORAGEDIR "/%s/cache/%s",
							srcaddr, dstaddr);
		sdp_file[PATH_MAX] = '\0';

		sdp_key_file = g_key_file_new();
		g_key_file_load_from_file(sdp_key_file, sdp_file, 0, NULL);

		snprintf(att_file, PATH_MAX, STORAGEDIR "/%s/%s/attributes",
							srcaddr, dstaddr);
		att_file[PATH_MAX] = '\0';

		att_key_file = g_key_file_new();
		g_key_file_load_from_file(att_key_file, att_file, 0, NULL);
	}

	for (seq = recs; seq; seq = seq->next) {
		sdp_record_t *rec = (sdp_record_t *) seq->data;
		sdp_list_t *svcclass = NULL;
		char *profile_uuid;
		GSList *l;

		if (!rec)
			break;

		if (sdp_get_service_classes(rec, &svcclass) < 0)
			continue;

		/* Check for empty service classes list */
		if (svcclass == NULL) {
			DBG("Skipping record with no service classes");
			continue;
		}

		/* Extract the first element and skip the remainning */
		profile_uuid = bt_uuid2string(svcclass->data);
		if (!profile_uuid) {
			sdp_list_free(svcclass, free);
			continue;
		}

		if (bt_uuid_strcmp(profile_uuid, PNP_UUID) == 0) {
			uint16_t source, vendor, product, version;
			sdp_data_t *pdlist;

			pdlist = sdp_data_get(rec, SDP_ATTR_VENDOR_ID_SOURCE);
			source = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_VENDOR_ID);
			vendor = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_PRODUCT_ID);
			product = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_VERSION);
			version = pdlist ? pdlist->val.uint16 : 0x0000;

			if (source || vendor || product || version)
				btd_device_set_pnpid(device, source, vendor,
							product, version);
		}

		/* Check for duplicates */
		if (sdp_list_find(req->records, rec, rec_cmp)) {
			g_free(profile_uuid);
			sdp_list_free(svcclass, free);
			continue;
		}

		if (sdp_key_file)
			store_sdp_record(sdp_key_file, rec);

		if (att_key_file)
			store_primaries_from_sdp_record(att_key_file, rec);

		/* Copy record */
		req->records = sdp_list_append(req->records,
							sdp_copy_record(rec));

		l = g_slist_find_custom(device->uuids, profile_uuid,
							(GCompareFunc) strcmp);
		if (!l)
			req->profiles_added =
					g_slist_append(req->profiles_added,
							profile_uuid);
		else {
			req->profiles_removed =
					g_slist_remove(req->profiles_removed,
							l->data);
			g_free(profile_uuid);
		}

		sdp_list_free(svcclass, free);
	}

	if (sdp_key_file) {
		data = g_key_file_to_data(sdp_key_file, &length, NULL);
		if (length > 0) {
			create_file(sdp_file, S_IRUSR | S_IWUSR);
			g_file_set_contents(sdp_file, data, length, NULL);
		}

		g_free(data);
		g_key_file_free(sdp_key_file);
	}

	if (att_key_file) {
		data = g_key_file_to_data(att_key_file, &length, NULL);
		if (length > 0) {
			create_file(att_file, S_IRUSR | S_IWUSR);
			g_file_set_contents(att_file, data, length, NULL);
		}

		g_free(data);
		g_key_file_free(att_key_file);
	}
}

static gint primary_cmp(gconstpointer a, gconstpointer b)
{
	return memcmp(a, b, sizeof(struct gatt_primary));
}

static void update_gatt_services(struct browse_req *req, GSList *current,
								GSList *found)
{
	GSList *l, *lmatch, *left = g_slist_copy(current);

	/* Added Profiles */
	for (l = found; l; l = g_slist_next(l)) {
		struct gatt_primary *prim = l->data;

		/* Entry found ? */
		lmatch = g_slist_find_custom(current, prim, primary_cmp);
		if (lmatch) {
			left = g_slist_remove(left, lmatch->data);
			continue;
		}

		/* New entry */
		req->profiles_added = g_slist_append(req->profiles_added,
							g_strdup(prim->uuid));

		DBG("UUID Added: %s", prim->uuid);
	}

	/* Removed Profiles */
	for (l = left; l; l = g_slist_next(l)) {
		struct gatt_primary *prim = l->data;
		req->profiles_removed = g_slist_append(req->profiles_removed,
							g_strdup(prim->uuid));

		DBG("UUID Removed: %s", prim->uuid);
	}

	g_slist_free(left);
}

static GSList *device_services_from_record(struct btd_device *device,
							GSList *profiles)
{
	GSList *l, *prim_list = NULL;
	char *att_uuid;
	uuid_t proto_uuid;

	sdp_uuid16_create(&proto_uuid, ATT_UUID);
	att_uuid = bt_uuid2string(&proto_uuid);

	for (l = profiles; l; l = l->next) {
		const char *profile_uuid = l->data;
		const sdp_record_t *rec;
		struct gatt_primary *prim;
		uint16_t start = 0, end = 0, psm = 0;
		uuid_t prim_uuid;

		rec = btd_device_get_record(device, profile_uuid);
		if (!rec)
			continue;

		if (!record_has_uuid(rec, att_uuid))
			continue;

		if (!gatt_parse_record(rec, &prim_uuid, &psm, &start, &end))
			continue;

		prim = g_new0(struct gatt_primary, 1);
		prim->range.start = start;
		prim->range.end = end;
		sdp_uuid2strn(&prim_uuid, prim->uuid, sizeof(prim->uuid));

		prim_list = g_slist_append(prim_list, prim);
	}

	g_free(att_uuid);

	return prim_list;
}

static void device_register_primaries(struct btd_device *device,
						GSList *prim_list, int psm)
{
	device->primaries = g_slist_concat(device->primaries, prim_list);
}

static void search_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct browse_req *req = user_data;
	struct btd_device *device = req->device;
	char addr[18];

	ba2str(&device->bdaddr, addr);

	if (err < 0) {
		error("%s: error updating services: %s (%d)",
				addr, strerror(-err), -err);
		goto send_reply;
	}

	update_bredr_services(req, recs);

	if (device->tmp_records)
		sdp_list_free(device->tmp_records,
					(sdp_free_func_t) sdp_record_free);

	device->tmp_records = req->records;
	req->records = NULL;

	if (!req->profiles_added && !req->profiles_removed) {
		DBG("%s: No service update", addr);
		goto send_reply;
	}

	/* Probe matching profiles for services added */
	if (req->profiles_added) {
		GSList *list;

		list = device_services_from_record(device, req->profiles_added);
		if (list)
			device_register_primaries(device, list, ATT_PSM);

		device_probe_profiles(device, req->profiles_added);
	}

	/* Remove profiles for services removed */
	if (req->profiles_removed)
		device_remove_profiles(device, req->profiles_removed);

	/* Propagate services changes */
	g_dbus_emit_property_changed(dbus_conn, req->device->path,
						DEVICE_INTERFACE, "UUIDs");

send_reply:
	device_svc_resolved(device, err);

	if (!device->temporary)
		store_device_info(device);

	browse_request_free(req);
}

static void browse_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct browse_req *req = user_data;
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device->adapter;
	uuid_t uuid;

	/* If we have a valid response and req->search_uuid == 2, then L2CAP
	 * UUID & PNP searching was successful -- we are done */
	if (err < 0 || (req->search_uuid == 2 && req->records)) {
		if (err == -ECONNRESET && req->reconnect_attempt < 1) {
			req->search_uuid--;
			req->reconnect_attempt++;
		} else
			goto done;
	}

	update_bredr_services(req, recs);

	/* Search for mandatory uuids */
	if (uuid_list[req->search_uuid]) {
		sdp_uuid16_create(&uuid, uuid_list[req->search_uuid++]);
		bt_search_service(adapter_get_address(adapter),
						&device->bdaddr, &uuid,
						browse_cb, user_data, NULL);
		return;
	}

done:
	search_cb(recs, err, user_data);
}

static void init_browse(struct browse_req *req, gboolean reverse)
{
	GSList *l;

	/* If we are doing reverse-SDP don't try to detect removed profiles
	 * since some devices hide their service records while they are
	 * connected
	 */
	if (reverse)
		return;

	for (l = req->device->uuids; l; l = l->next)
		req->profiles_removed = g_slist_append(req->profiles_removed,
						l->data);
}

static void store_services(struct btd_device *device)
{
	struct btd_adapter *adapter = device->adapter;
	char filename[PATH_MAX + 1];
	char src_addr[18], dst_addr[18];
	uuid_t uuid;
	char *prim_uuid;
	GKeyFile *key_file;
	GSList *l;
	char *data;
	gsize length = 0;

	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	prim_uuid = bt_uuid2string(&uuid);
	if (prim_uuid == NULL)
		return;

	ba2str(adapter_get_address(adapter), src_addr);
	ba2str(&device->bdaddr, dst_addr);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/attributes", src_addr,
								dst_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();

	for (l = device->primaries; l; l = l->next) {
		struct gatt_primary *primary = l->data;
		char handle[6], uuid_str[33];
		int i;

		sprintf(handle, "%hu", primary->range.start);

		bt_string2uuid(&uuid, primary->uuid);
		sdp_uuid128_to_uuid(&uuid);

		switch (uuid.type) {
		case SDP_UUID16:
			sprintf(uuid_str, "%4.4X", uuid.value.uuid16);
			break;
		case SDP_UUID32:
			sprintf(uuid_str, "%8.8X", uuid.value.uuid32);
			break;
		case SDP_UUID128:
			for (i = 0; i < 16; i++)
				sprintf(uuid_str + (i * 2), "%2.2X",
						uuid.value.uuid128.data[i]);
			break;
		default:
			uuid_str[0] = '\0';
		}

		g_key_file_set_string(key_file, handle, "UUID", prim_uuid);
		g_key_file_set_string(key_file, handle, "Value", uuid_str);
		g_key_file_set_integer(key_file, handle, "EndGroupHandle",
					primary->range.end);
	}

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(prim_uuid);
	g_free(data);
	g_key_file_free(key_file);
}

static bool device_get_auto_connect(struct btd_device *device)
{
	if (device->disable_auto_connect)
		return false;

	return device->auto_connect;
}

static void attio_connected(gpointer data, gpointer user_data)
{
	struct attio_data *attio = data;
	GAttrib *attrib = user_data;

	if (attio->cfunc)
		attio->cfunc(attrib, attio->user_data);
}

static void attio_disconnected(gpointer data, gpointer user_data)
{
	struct attio_data *attio = data;

	if (attio->dcfunc)
		attio->dcfunc(attio->user_data);
}

static gboolean attrib_disconnected_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct btd_device *device = user_data;
	int sock, err = 0;
	socklen_t len;

	if (device->browse)
		goto done;

	sock = g_io_channel_unix_get_fd(io);
	len = sizeof(err);
	getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);

	g_slist_foreach(device->attios, attio_disconnected, NULL);

	if (!device_get_auto_connect(device)) {
		DBG("Automatic connection disabled");
		goto done;
	}

	/*
	 * Keep scanning/re-connection active if disconnection reason
	 * is connection timeout, remote user terminated connection or local
	 * initiated disconnection.
	 */
	if (err == ETIMEDOUT || err == ECONNRESET || err == ECONNABORTED)
		adapter_connect_list_add(device->adapter, device);

done:
	attio_cleanup(device);

	return FALSE;
}

static void register_all_services(struct browse_req *req, GSList *services)
{
	struct btd_device *device = req->device;

	device_set_temporary(device, FALSE);

	update_gatt_services(req, device->primaries, services);
	g_slist_free_full(device->primaries, g_free);
	device->primaries = NULL;

	device_register_primaries(device, g_slist_copy(services), -1);
	if (req->profiles_removed)
		device_remove_profiles(device, req->profiles_removed);

	device_probe_profiles(device, req->profiles_added);

	if (device->attios == NULL && device->attios_offline == NULL)
		attio_cleanup(device);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "UUIDs");

	device_svc_resolved(device, 0);

	store_services(device);

	browse_request_free(req);
}

static int service_by_range_cmp(gconstpointer a, gconstpointer b)
{
	const struct gatt_primary *prim = a;
	const struct att_range *range = b;

	return memcmp(&prim->range, range, sizeof(*range));
}

static void find_included_cb(GSList *includes, uint8_t status,
						gpointer user_data)
{
	struct included_search *search = user_data;
	struct btd_device *device = search->req->device;
	struct gatt_primary *prim;
	GSList *l;

	if (status != 0) {
		error("Find included services failed: %s (%d)",
					att_ecode2str(status), status);
		goto done;
	}

	if (includes == NULL)
		goto done;

	for (l = includes; l; l = l->next) {
		struct gatt_included *incl = l->data;

		if (g_slist_find_custom(search->services, &incl->range,
						service_by_range_cmp))
			continue;

		prim = g_new0(struct gatt_primary, 1);
		memcpy(prim->uuid, incl->uuid, sizeof(prim->uuid));
		memcpy(&prim->range, &incl->range, sizeof(prim->range));

		search->services = g_slist_append(search->services, prim);
	}

done:
	search->current = search->current->next;
	if (search->current == NULL) {
		register_all_services(search->req, search->services);
		g_slist_free(search->services);
		g_free(search);
		return;
	}

	prim = search->current->data;
	gatt_find_included(device->attrib, prim->range.start, prim->range.end,
					find_included_cb, search);
}

static void find_included_services(struct browse_req *req, GSList *services)
{
	struct btd_device *device = req->device;
	struct included_search *search;
	struct gatt_primary *prim;

	if (services == NULL)
		return;

	search = g_new0(struct included_search, 1);
	search->req = req;
	search->services = g_slist_copy(services);
	search->current = search->services;

	prim = search->current->data;
	gatt_find_included(device->attrib, prim->range.start, prim->range.end,
					find_included_cb, search);

}

static void primary_cb(GSList *services, guint8 status, gpointer user_data)
{
	struct browse_req *req = user_data;

	if (status) {
		struct btd_device *device = req->device;

		if (req->msg) {
			DBusMessage *reply;
			reply = btd_error_failed(req->msg,
							att_ecode2str(status));
			g_dbus_send_message(dbus_conn, reply);
		}

		device->browse = NULL;
		browse_request_free(req);
		return;
	}

	find_included_services(req, services);
}

static void att_connect_cb(GIOChannel *io, GError *gerr, gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	DBusMessage *reply;
	uint8_t io_cap;
	GAttrib *attrib;
	int err = 0;

	g_io_channel_unref(device->att_io);
	device->att_io = NULL;

	if (gerr) {
		DBG("%s", gerr->message);

		if (attcb->error)
			attcb->error(gerr, user_data);

		err = -ECONNABORTED;
		goto done;
	}

	attrib = g_attrib_new(io);
	device->attachid = attrib_channel_attach(attrib);
	if (device->attachid == 0)
		error("Attribute server attach failure!");

	device->attrib = attrib;
	device->cleanup_id = g_io_add_watch(io, G_IO_HUP,
					attrib_disconnected_cb, device);

	if (attcb->success)
		attcb->success(user_data);

	if (!device->bonding)
		goto done;

	if (device->bonding->agent)
		io_cap = agent_get_io_capability(device->bonding->agent);
	else
		io_cap = IO_CAPABILITY_NOINPUTNOOUTPUT;

	err = adapter_create_bonding(device->adapter, &device->bdaddr,
					device->bdaddr_type, io_cap);
done:
	if (device->bonding && err < 0) {
		reply = btd_error_failed(device->bonding->msg, strerror(-err));
		g_dbus_send_message(dbus_conn, reply);
		bonding_request_cancel(device->bonding);
		bonding_request_free(device->bonding);
	}

	if (device->connect) {
		if (!device->svc_resolved)
			device_browse_primary(device, NULL, FALSE);

		if (err < 0)
			reply = btd_error_failed(device->connect,
							strerror(-err));
		else
			reply = dbus_message_new_method_return(device->connect);

		g_dbus_send_message(dbus_conn, reply);
		dbus_message_unref(device->connect);
		device->connect = NULL;
	}

	g_free(attcb);
}

static void att_error_cb(const GError *gerr, gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;

	if (g_error_matches(gerr, BT_IO_ERROR, ECONNABORTED))
		return;

	if (device_get_auto_connect(device)) {
		DBG("Enabling automatic connections");
		adapter_connect_list_add(device->adapter, device);
	}
}

static void att_success_cb(gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;

	if (device->attios == NULL)
		return;

	/*
	 * Remove the device from the connect_list and give the passive
	 * scanning another chance to be restarted in case there are
	 * other devices in the connect_list.
	 */
	adapter_connect_list_remove(device->adapter, device);

	g_slist_foreach(device->attios, attio_connected, device->attrib);
}

int device_connect_le(struct btd_device *dev)
{
	struct btd_adapter *adapter = dev->adapter;
	struct att_callbacks *attcb;
	BtIOSecLevel sec_level;
	GIOChannel *io;
	GError *gerr = NULL;
	char addr[18];

	/* There is one connection attempt going on */
	if (dev->att_io)
		return -EALREADY;

	ba2str(&dev->bdaddr, addr);

	DBG("Connection attempt to: %s", addr);

	attcb = g_new0(struct att_callbacks, 1);
	attcb->error = att_error_cb;
	attcb->success = att_success_cb;
	attcb->user_data = dev;

	if (dev->paired)
		sec_level = BT_IO_SEC_MEDIUM;
	else
		sec_level = BT_IO_SEC_LOW;

	/*
	 * This connection will help us catch any PDUs that comes before
	 * pairing finishes
	 */
	io = bt_io_connect(att_connect_cb, attcb, NULL, &gerr,
			BT_IO_OPT_SOURCE_BDADDR, adapter_get_address(adapter),
			BT_IO_OPT_DEST_BDADDR, &dev->bdaddr,
			BT_IO_OPT_DEST_TYPE, dev->bdaddr_type,
			BT_IO_OPT_CID, ATT_CID,
			BT_IO_OPT_SEC_LEVEL, sec_level,
			BT_IO_OPT_INVALID);

	if (io == NULL) {
		if (dev->bonding) {
			DBusMessage *reply = btd_error_failed(
					dev->bonding->msg, gerr->message);

			g_dbus_send_message(dbus_conn, reply);
			bonding_request_cancel(dev->bonding);
			bonding_request_free(dev->bonding);
		}

		error("ATT bt_io_connect(%s): %s", addr, gerr->message);
		g_error_free(gerr);
		g_free(attcb);
		return -EIO;
	}

	/* Keep this, so we can cancel the connection */
	dev->att_io = io;

	return 0;
}

static void att_browse_error_cb(const GError *gerr, gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	struct browse_req *req = device->browse;

	if (req->msg) {
		DBusMessage *reply;

		reply = btd_error_failed(req->msg, gerr->message);
		g_dbus_send_message(dbus_conn, reply);
	}

	device->browse = NULL;
	browse_request_free(req);
}

static void att_browse_cb(gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;

	gatt_discover_primary(device->attrib, NULL, primary_cb,
							device->browse);
}

static int device_browse_primary(struct btd_device *device, DBusMessage *msg,
								gboolean secure)
{
	struct btd_adapter *adapter = device->adapter;
	struct att_callbacks *attcb;
	struct browse_req *req;
	BtIOSecLevel sec_level;

	if (device->browse)
		return -EBUSY;

	req = g_new0(struct browse_req, 1);
	req->device = btd_device_ref(device);

	device->browse = req;

	if (device->attrib) {
		gatt_discover_primary(device->attrib, NULL, primary_cb, req);
		goto done;
	}

	sec_level = secure ? BT_IO_SEC_HIGH : BT_IO_SEC_LOW;

	attcb = g_new0(struct att_callbacks, 1);
	attcb->error = att_browse_error_cb;
	attcb->success = att_browse_cb;
	attcb->user_data = device;

	device->att_io = bt_io_connect(att_connect_cb,
				attcb, NULL, NULL,
				BT_IO_OPT_SOURCE_BDADDR,
				adapter_get_address(adapter),
				BT_IO_OPT_DEST_BDADDR, &device->bdaddr,
				BT_IO_OPT_DEST_TYPE, device->bdaddr_type,
				BT_IO_OPT_CID, ATT_CID,
				BT_IO_OPT_SEC_LEVEL, sec_level,
				BT_IO_OPT_INVALID);

	if (device->att_io == NULL) {
		device->browse = NULL;
		browse_request_free(req);
		g_free(attcb);
		return -EIO;
	}

done:

	if (msg) {
		const char *sender = dbus_message_get_sender(msg);

		req->msg = dbus_message_ref(msg);
		/* Track the request owner to cancel it
		 * automatically if the owner exits */
		req->listener_id = g_dbus_add_disconnect_watch(dbus_conn,
						sender,
						discover_services_req_exit,
						req, NULL);
	}

	return 0;
}

static int device_browse_sdp(struct btd_device *device, DBusMessage *msg,
							gboolean reverse)
{
	struct btd_adapter *adapter = device->adapter;
	struct browse_req *req;
	uuid_t uuid;
	int err;

	if (device->browse)
		return -EBUSY;

	req = g_new0(struct browse_req, 1);
	req->device = btd_device_ref(device);
	sdp_uuid16_create(&uuid, uuid_list[req->search_uuid++]);
	init_browse(req, reverse);

	err = bt_search_service(adapter_get_address(adapter), &device->bdaddr,
						&uuid, browse_cb, req, NULL);
	if (err < 0) {
		browse_request_free(req);
		return err;
	}

	device->browse = req;

	if (msg) {
		const char *sender = dbus_message_get_sender(msg);

		req->msg = dbus_message_ref(msg);
		/* Track the request owner to cancel it
		 * automatically if the owner exits */
		req->listener_id = g_dbus_add_disconnect_watch(dbus_conn,
						sender,
						discover_services_req_exit,
						req, NULL);
	}

	return err;
}

struct btd_adapter *device_get_adapter(struct btd_device *device)
{
	if (!device)
		return NULL;

	return device->adapter;
}

const bdaddr_t *device_get_address(struct btd_device *device)
{
	return &device->bdaddr;
}

const char *device_get_path(const struct btd_device *device)
{
	if (!device)
		return NULL;

	return device->path;
}

gboolean device_is_temporary(struct btd_device *device)
{
	return device->temporary;
}

void device_set_temporary(struct btd_device *device, gboolean temporary)
{
	if (!device)
		return;

	if (device->temporary == temporary)
		return;

	DBG("temporary %d", temporary);

	if (temporary)
		adapter_connect_list_remove(device->adapter, device);

	device->temporary = temporary;
}

void device_set_trusted(struct btd_device *device, gboolean trusted)
{
	if (!device)
		return;

	if (device->trusted == trusted)
		return;

	DBG("trusted %d", trusted);

	device->trusted = trusted;

	store_device_info(device);

	g_dbus_emit_property_changed(dbus_conn, device->path,
					DEVICE_INTERFACE, "Trusted");
}

void device_set_bonded(struct btd_device *device, gboolean bonded)
{
	if (!device)
		return;

	DBG("bonded %d", bonded);

	device->bonded = bonded;
}

void device_set_legacy(struct btd_device *device, bool legacy)
{
	if (!device)
		return;

	DBG("legacy %d", legacy);

	if (device->legacy == legacy)
		return;

	device->legacy = legacy;

	g_dbus_emit_property_changed(dbus_conn, device->path,
					DEVICE_INTERFACE, "LegacyPairing");
}

void device_set_rssi(struct btd_device *device, int8_t rssi)
{
	if (!device)
		return;

	if (rssi == 0 || device->rssi == 0) {
		if (device->rssi == rssi)
			return;

		DBG("rssi %d", rssi);

		device->rssi = rssi;
	} else {
		int delta;

		if (device->rssi > rssi)
			delta = device->rssi - rssi;
		else
			delta = rssi - device->rssi;

		/* only report changes of 8 dBm or more */
		if (delta < 8)
			return;

		DBG("rssi %d delta %d", rssi, delta);

		device->rssi = rssi;
	}

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "RSSI");
}

static void device_set_auto_connect(struct btd_device *device, gboolean enable)
{
	char addr[18];

	if (!device)
		return;

	ba2str(&device->bdaddr, addr);

	DBG("%s auto connect: %d", addr, enable);

	device->auto_connect = enable;

	/* Disabling auto connect */
	if (enable == FALSE) {
		adapter_connect_list_remove(device->adapter, device);
		return;
	}

	if (device->attrib) {
		DBG("Already connected");
		return;
	}

	/* Enabling auto connect */
	adapter_connect_list_add(device->adapter, device);
}

static gboolean start_discovery(gpointer user_data)
{
	struct btd_device *device = user_data;

	if (device_is_bredr(device))
		device_browse_sdp(device, NULL, TRUE);
	else
		device_browse_primary(device, NULL, FALSE);

	device->discov_timer = 0;

	return FALSE;
}

void device_set_paired(struct btd_device *device, gboolean value)
{
	if (device->paired == value)
		return;

	if (!value)
		btd_adapter_remove_bonding(device->adapter, &device->bdaddr,
							device->bdaddr_type);

	device->paired = value;

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Paired");
}

static void device_auth_req_free(struct btd_device *device)
{
	if (device->authr)
		g_free(device->authr->pincode);
	g_free(device->authr);
	device->authr = NULL;
}

void device_bonding_complete(struct btd_device *device, uint8_t status)
{
	struct bonding_req *bonding = device->bonding;
	struct authentication_req *auth = device->authr;

	DBG("bonding %p status 0x%02x", bonding, status);

	if (auth && auth->agent)
		agent_cancel(auth->agent);

	if (status) {
		device_cancel_authentication(device, TRUE);
		device_bonding_failed(device, status);
		return;
	}

	device_auth_req_free(device);

	/* If we're already paired nothing more is needed */
	if (device->paired)
		return;

	device_set_paired(device, TRUE);

	/* If we were initiators start service discovery immediately.
	 * However if the other end was the initator wait a few seconds
	 * before SDP. This is due to potential IOP issues if the other
	 * end starts doing SDP at the same time as us */
	if (bonding) {
		DBG("Proceeding with service discovery");
		/* If we are initiators remove any discovery timer and just
		 * start discovering services directly */
		if (device->discov_timer) {
			g_source_remove(device->discov_timer);
			device->discov_timer = 0;
		}

		if (device_is_bredr(device))
			device_browse_sdp(device, bonding->msg, FALSE);
		else
			device_browse_primary(device, bonding->msg, FALSE);

		bonding_request_free(bonding);
	} else if (!device->svc_resolved) {
		if (!device->browse && !device->discov_timer &&
				main_opts.reverse_sdp) {
			/* If we are not initiators and there is no currently
			 * active discovery or discovery timer, set discovery
			 * timer */
			DBG("setting timer for reverse service discovery");
			device->discov_timer = g_timeout_add_seconds(
							DISCOVERY_TIMER,
							start_discovery,
							device);
		}
	}
}

static gboolean svc_idle_cb(gpointer user_data)
{
	struct svc_callback *cb = user_data;
	struct btd_device *dev = cb->dev;

	dev->svc_callbacks = g_slist_remove(dev->svc_callbacks, cb);

	cb->func(cb->dev, 0, cb->user_data);

	g_free(cb);

	return FALSE;
}

unsigned int device_wait_for_svc_complete(struct btd_device *dev,
							device_svc_cb_t func,
							void *user_data)
{
	static unsigned int id = 0;
	struct svc_callback *cb;

	cb = g_new0(struct svc_callback, 1);
	cb->func = func;
	cb->user_data = user_data;
	cb->dev = dev;
	cb->id = ++id;

	dev->svc_callbacks = g_slist_prepend(dev->svc_callbacks, cb);

	if (dev->svc_resolved || !main_opts.reverse_sdp)
		cb->idle_id = g_idle_add(svc_idle_cb, cb);
	else if (dev->discov_timer > 0) {
		g_source_remove(dev->discov_timer);
		dev->discov_timer = g_idle_add(start_discovery, dev);
	}

	return cb->id;
}

bool device_remove_svc_complete_callback(struct btd_device *dev,
							unsigned int id)
{
	GSList *l;

	for (l = dev->svc_callbacks; l != NULL; l = g_slist_next(l)) {
		struct svc_callback *cb = l->data;

		if (cb->id != id)
			continue;

		if (cb->idle_id > 0)
			g_source_remove(cb->idle_id);

		dev->svc_callbacks = g_slist_remove(dev->svc_callbacks, cb);
		g_free(cb);

		return true;
	}

	return false;
}

gboolean device_is_bonding(struct btd_device *device, const char *sender)
{
	struct bonding_req *bonding = device->bonding;

	if (!device->bonding)
		return FALSE;

	if (!sender)
		return TRUE;

	return g_str_equal(sender, dbus_message_get_sender(bonding->msg));
}

void device_bonding_failed(struct btd_device *device, uint8_t status)
{
	struct bonding_req *bonding = device->bonding;
	DBusMessage *reply;

	DBG("status %u", status);

	if (!bonding)
		return;

	if (device->authr)
		device_cancel_authentication(device, FALSE);

	reply = new_authentication_return(bonding->msg, status);
	g_dbus_send_message(dbus_conn, reply);

	bonding_request_free(bonding);
}

static void pincode_cb(struct agent *agent, DBusError *err, const char *pin,
								void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;

	/* No need to reply anything if the authentication already failed */
	if (auth->agent == NULL)
		return;

	btd_adapter_pincode_reply(device->adapter, &device->bdaddr,
						pin, pin ? strlen(pin) : 0);

	agent_unref(device->authr->agent);
	device->authr->agent = NULL;
}

static void confirm_cb(struct agent *agent, DBusError *err, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;

	/* No need to reply anything if the authentication already failed */
	if (auth->agent == NULL)
		return;

	btd_adapter_confirm_reply(device->adapter, &device->bdaddr,
							device->bdaddr_type,
							err ? FALSE : TRUE);

	agent_unref(device->authr->agent);
	device->authr->agent = NULL;
}

static void passkey_cb(struct agent *agent, DBusError *err,
						uint32_t passkey, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;

	/* No need to reply anything if the authentication already failed */
	if (auth->agent == NULL)
		return;

	if (err)
		passkey = INVALID_PASSKEY;

	btd_adapter_passkey_reply(device->adapter, &device->bdaddr,
						device->bdaddr_type, passkey);

	agent_unref(device->authr->agent);
	device->authr->agent = NULL;
}

static void display_pincode_cb(struct agent *agent, DBusError *err, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;

	pincode_cb(agent, err, auth->pincode, auth);

	g_free(device->authr->pincode);
	device->authr->pincode = NULL;
}

static struct authentication_req *new_auth(struct btd_device *device,
					auth_type_t type, gboolean secure)
{
	struct authentication_req *auth;
	struct agent *agent;
	char addr[18];

	ba2str(&device->bdaddr, addr);
	DBG("Requesting agent authentication for %s", addr);

	if (device->authr) {
		error("Authentication already requested for %s", addr);
		return NULL;
	}

	if (device->bonding && device->bonding->agent)
		agent = agent_ref(device->bonding->agent);
	else
		agent = agent_get(NULL);

	if (!agent) {
		error("No agent available for request type %d", type);
		return NULL;
	}

	auth = g_new0(struct authentication_req, 1);
	auth->agent = agent;
	auth->device = device;
	auth->type = type;
	auth->secure = secure;
	device->authr = auth;

	return auth;
}

int device_request_pincode(struct btd_device *device, gboolean secure)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_PINCODE, secure);
	if (!auth)
		return -EPERM;

	err = agent_request_pincode(auth->agent, device, pincode_cb, secure,
								auth, NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_request_passkey(struct btd_device *device)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_PASSKEY, FALSE);
	if (!auth)
		return -EPERM;

	err = agent_request_passkey(auth->agent, device, passkey_cb, auth,
									NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_confirm_passkey(struct btd_device *device, uint32_t passkey,
							uint8_t confirm_hint)

{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_CONFIRM, FALSE);
	if (!auth)
		return -EPERM;

	auth->passkey = passkey;

	if (confirm_hint)
		err = agent_request_authorization(auth->agent, device,
						confirm_cb, auth, NULL);
	else
		err = agent_request_confirmation(auth->agent, device, passkey,
						confirm_cb, auth, NULL);

	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_notify_passkey(struct btd_device *device, uint32_t passkey,
							uint8_t entered)
{
	struct authentication_req *auth;
	int err;

	if (device->authr) {
		auth = device->authr;
		if (auth->type != AUTH_TYPE_NOTIFY_PASSKEY)
			return -EPERM;
	} else {
		auth = new_auth(device, AUTH_TYPE_NOTIFY_PASSKEY, FALSE);
		if (!auth)
			return -EPERM;
	}

	err = agent_display_passkey(auth->agent, device, passkey, entered);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_notify_pincode(struct btd_device *device, gboolean secure,
							const char *pincode)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_NOTIFY_PINCODE, secure);
	if (!auth)
		return -EPERM;

	auth->pincode = g_strdup(pincode);

	err = agent_display_pincode(auth->agent, device, pincode,
					display_pincode_cb, auth, NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

static void cancel_authentication(struct authentication_req *auth)
{
	struct agent *agent;
	DBusError err;

	if (!auth || !auth->agent)
		return;

	agent = auth->agent;
	auth->agent = NULL;

	dbus_error_init(&err);
	dbus_set_error_const(&err, ERROR_INTERFACE ".Canceled", NULL);

	switch (auth->type) {
	case AUTH_TYPE_PINCODE:
		pincode_cb(agent, &err, NULL, auth);
		break;
	case AUTH_TYPE_CONFIRM:
		confirm_cb(agent, &err, auth);
		break;
	case AUTH_TYPE_PASSKEY:
		passkey_cb(agent, &err, 0, auth);
		break;
	case AUTH_TYPE_NOTIFY_PASSKEY:
		/* User Notify doesn't require any reply */
		break;
	case AUTH_TYPE_NOTIFY_PINCODE:
		pincode_cb(agent, &err, NULL, auth);
		break;
	}

	dbus_error_free(&err);
}

void device_cancel_authentication(struct btd_device *device, gboolean aborted)
{
	struct authentication_req *auth = device->authr;
	char addr[18];

	if (!auth)
		return;

	ba2str(&device->bdaddr, addr);
	DBG("Canceling authentication request for %s", addr);

	if (auth->agent)
		agent_cancel(auth->agent);

	if (!aborted)
		cancel_authentication(auth);

	device_auth_req_free(device);
}

gboolean device_is_authenticating(struct btd_device *device)
{
	return (device->authr != NULL);
}

static int primary_uuid_cmp(gconstpointer a, gconstpointer b)
{
	const struct gatt_primary *prim = a;
	const char *uuid = b;

	return strcasecmp(prim->uuid, uuid);
}

struct gatt_primary *btd_device_get_primary(struct btd_device *device,
							const char *uuid)
{
	GSList *match;

	match = g_slist_find_custom(device->primaries, uuid, primary_uuid_cmp);
	if (match)
		return match->data;

	return NULL;
}

GSList *btd_device_get_primaries(struct btd_device *device)
{
	return device->primaries;
}

void btd_device_gatt_set_service_changed(struct btd_device *device,
						uint16_t start, uint16_t end)
{
	GSList *l;

	for (l = device->primaries; l; l = g_slist_next(l)) {
		struct gatt_primary *prim = l->data;

		if (start <= prim->range.end && end >= prim->range.start)
			prim->changed = TRUE;
	}

	device_browse_primary(device, NULL, FALSE);
}

void btd_device_add_uuid(struct btd_device *device, const char *uuid)
{
	GSList *uuid_list;
	char *new_uuid;

	if (g_slist_find_custom(device->uuids, uuid, bt_uuid_strcmp))
		return;

	new_uuid = g_strdup(uuid);
	uuid_list = g_slist_append(NULL, new_uuid);

	device_probe_profiles(device, uuid_list);

	g_free(new_uuid);
	g_slist_free(uuid_list);

	store_device_info(device);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "UUIDs");
}

static sdp_list_t *read_device_records(struct btd_device *device)
{
	char local[18], peer[18];
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char **keys, **handle;
	char *str;
	sdp_list_t *recs = NULL;
	sdp_record_t *rec;

	ba2str(adapter_get_address(device->adapter), local);
	ba2str(&device->bdaddr, peer);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/cache/%s", local, peer);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);
	keys = g_key_file_get_keys(key_file, "ServiceRecords", NULL, NULL);

	for (handle = keys; handle && *handle; handle++) {
		str = g_key_file_get_string(key_file, "ServiceRecords",
						*handle, NULL);
		if (!str)
			continue;

		rec = record_from_string(str);
		recs = sdp_list_append(recs, rec);
		g_free(str);
	}

	g_strfreev(keys);
	g_key_file_free(key_file);

	return recs;
}

const sdp_record_t *btd_device_get_record(struct btd_device *device,
							const char *uuid)
{
	if (device->tmp_records) {
		const sdp_record_t *record;

		record = find_record_in_list(device->tmp_records, uuid);
		if (record != NULL)
			return record;

		sdp_list_free(device->tmp_records,
					(sdp_free_func_t) sdp_record_free);
		device->tmp_records = NULL;
	}

	device->tmp_records = read_device_records(device);
	if (!device->tmp_records)
		return NULL;

	return find_record_in_list(device->tmp_records, uuid);
}

struct btd_device *btd_device_ref(struct btd_device *device)
{
	__sync_fetch_and_add(&device->ref_count, 1);

	return device;
}

void btd_device_unref(struct btd_device *device)
{
	if (__sync_sub_and_fetch(&device->ref_count, 1))
		return;

	if (!device->path) {
		error("freeing device without an object path");
		return;
	}

	DBG("Freeing device %s", device->path);

	g_dbus_unregister_interface(dbus_conn, device->path, DEVICE_INTERFACE);
}

int device_get_appearance(struct btd_device *device, uint16_t *value)
{
	if (device->appearance == 0)
		return -1;

	if (value)
		*value = device->appearance;

	return 0;
}

void device_set_appearance(struct btd_device *device, uint16_t value)
{
	const char *icon = gap_appearance_to_icon(value);

	g_dbus_emit_property_changed(dbus_conn, device->path,
					DEVICE_INTERFACE, "Appearance");

	if (icon)
		g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Icon");

	device->appearance = value;
	store_device_info(device);
}

static gboolean notify_attios(gpointer user_data)
{
	struct btd_device *device = user_data;

	if (device->attrib == NULL)
		return FALSE;

	g_slist_foreach(device->attios_offline, attio_connected, device->attrib);
	device->attios = g_slist_concat(device->attios, device->attios_offline);
	device->attios_offline = NULL;

	return FALSE;
}

guint btd_device_add_attio_callback(struct btd_device *device,
						attio_connect_cb cfunc,
						attio_disconnect_cb dcfunc,
						gpointer user_data)
{
	struct attio_data *attio;
	static guint attio_id = 0;

	DBG("%p registered ATT connection callback", device);

	attio = g_new0(struct attio_data, 1);
	attio->id = ++attio_id;
	attio->cfunc = cfunc;
	attio->dcfunc = dcfunc;
	attio->user_data = user_data;

	device_set_auto_connect(device, TRUE);

	if (device->attrib && cfunc) {
		device->attios_offline = g_slist_append(device->attios_offline,
									attio);
		g_idle_add(notify_attios, device);
		return attio->id;
	}

	device->attios = g_slist_append(device->attios, attio);

	return attio->id;
}

static int attio_id_cmp(gconstpointer a, gconstpointer b)
{
	const struct attio_data *attio = a;
	guint id = GPOINTER_TO_UINT(b);

	return attio->id - id;
}

gboolean btd_device_remove_attio_callback(struct btd_device *device, guint id)
{
	struct attio_data *attio;
	GSList *l;

	l = g_slist_find_custom(device->attios, GUINT_TO_POINTER(id),
								attio_id_cmp);
	if (l) {
		attio = l->data;
		device->attios = g_slist_remove(device->attios, attio);
	} else {
		l = g_slist_find_custom(device->attios_offline,
					GUINT_TO_POINTER(id), attio_id_cmp);
		if (!l)
			return FALSE;

		attio = l->data;
		device->attios_offline = g_slist_remove(device->attios_offline,
									attio);
	}

	g_free(attio);

	if (device->attios != NULL || device->attios_offline != NULL)
		return TRUE;

	attio_cleanup(device);

	return TRUE;
}

void btd_device_set_pnpid(struct btd_device *device, uint16_t source,
			uint16_t vendor, uint16_t product, uint16_t version)
{
	device->vendor_src = source;
	device->vendor = vendor;
	device->product = product;
	device->version = version;

	g_free(device->modalias);
	device->modalias = bt_modalias(source, vendor, product, version);

	g_dbus_emit_property_changed(dbus_conn, device->path,
						DEVICE_INTERFACE, "Modalias");

	store_device_info(device);
}

void btd_device_init(void)
{
	dbus_conn = btd_get_dbus_connection();
}

void btd_device_cleanup(void)
{
}
