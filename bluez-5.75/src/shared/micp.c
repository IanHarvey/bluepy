// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  NXP Semiconductors. All rights reserved.
 *
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include "lib/bluetooth.h"
#include "lib/uuid.h"

#include "src/shared/queue.h"
#include "src/shared/util.h"
#include "src/shared/timeout.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "src/shared/gatt-helpers.h"
#include "src/shared/micp.h"

#define DBG(_micp, fmt, arg...) \
	micp_debug(_micp, "%s:%s() " fmt, __FILE__, __func__, ##arg)

/* Application error codes */
#define MICP_ERROR_MUTE_DISABLED            0x80
#define MICP_ERROR_VALUE_NOT_ALLOWED        0x13
#define BT_ATT_ERROR_OPCODE_NOT_SUPPORTED   0x81

/* Mute char values */
#define MICS_NOT_MUTED  0x00
#define MICS_MUTED      0x01
#define MICS_DISABLED   0x02

static struct queue *micp_db;
static struct queue *micp_cbs;
static struct queue *sessions;

struct bt_micp_cb {
	unsigned int id;
	bt_micp_func_t attached;
	bt_micp_func_t detached;
	void *user_data;
};

typedef void (*micp_func_t)(struct bt_micp *micp, bool success,
				uint8_t att_ecode, const uint8_t *value,
				uint16_t length, void *user_data);

struct bt_micp_pending {
	unsigned int id;
	struct bt_micp *micp;
	micp_func_t func;
	void *userdata;
};

struct bt_micp_ready {
	unsigned int id;
	bt_micp_ready_func_t func;
	bt_micp_destroy_func_t destroy;
	void *data;
};

typedef void (*micp_notify_t)(struct bt_micp *micp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data);

struct bt_micp_notify {
	unsigned int id;
	struct bt_micp *micp;
	micp_notify_t func;
	void *user_data;
};

static void *iov_pull_mem(struct iovec *iov, size_t len)
{
	void *data = iov->iov_base;

	if (iov->iov_len < len)
		return NULL;

	iov->iov_base += len;
	iov->iov_len -= len;

	return data;
}

static struct bt_micp_db *micp_get_mdb(struct bt_micp *micp)
{
	if (!micp)
		return NULL;

	if (micp->ldb)
		return micp->ldb;

	return NULL;
}

static uint8_t *mdb_get_mute_state(struct bt_micp_db *vdb)
{
	if (!vdb->mics)
		return NULL;

	return &(vdb->mics->mute_stat);
}

struct bt_mics *micp_get_mics(struct bt_micp *micp)
{
	if (!micp)
		return NULL;

	if (micp->rdb->mics)
		return micp->rdb->mics;

	micp->rdb->mics = new0(struct bt_mics, 1);
	micp->rdb->mics->mdb = micp->rdb;

	return micp->rdb->mics;
}

static void micp_detached(void *data, void *user_data)
{
	struct bt_micp_cb *cb = data;
	struct bt_micp *micp = user_data;

	cb->detached(micp, cb->user_data);
}

void bt_micp_detach(struct bt_micp *micp)
{
	if (!queue_remove(sessions, micp))
		return;

	bt_gatt_client_idle_unregister(micp->client, micp->idle_id);
	bt_gatt_client_unref(micp->client);
	micp->client = NULL;

	queue_foreach(micp_cbs, micp_detached, micp);
}

static void micp_db_free(void *data)
{
	struct bt_micp_db *mdb = data;

	if (!mdb)
		return;

	gatt_db_unref(mdb->db);

	free(mdb->mics);
	free(mdb);
}

static void micp_ready_free(void *data)
{
	struct bt_micp_ready *ready = data;

	if (ready->destroy)
		ready->destroy(ready->data);

	free(ready);
}

static void micp_free(void *data)
{
	struct bt_micp *micp = data;

	bt_micp_detach(micp);

	micp_db_free(micp->rdb);

	queue_destroy(micp->notify, free);
	queue_destroy(micp->pending, NULL);
	queue_destroy(micp->ready_cbs, micp_ready_free);

	free(micp);
}

bool bt_micp_set_user_data(struct bt_micp *micp, void *user_data)
{

	if (!micp)
		return false;

	micp->user_data = user_data;

	return true;
}

static bool micp_db_match(const void *data, const void *match_data)
{
	const struct bt_micp_db *mdb = data;
	const struct gatt_db *db = match_data;

	return (mdb->db == db);
}

struct bt_att *bt_micp_get_att(struct bt_micp *micp)
{
	if (!micp)
		return NULL;

	if (micp->att)
		return micp->att;

	return bt_gatt_client_get_att(micp->client);
}

struct bt_micp *bt_micp_ref(struct bt_micp *micp)
{
	if (!micp)
		return NULL;

	__sync_fetch_and_add(&micp->ref_count, 1);

	return micp;
}

void bt_micp_unref(struct bt_micp *micp)
{
	if (!micp)
		return;

	if (__sync_sub_and_fetch(&micp->ref_count, 1))
		return;

	micp_free(micp);
}

static void micp_debug(struct bt_micp *micp, const char *format, ...)
{
	va_list ap;

	if (!micp || !format || !micp->debug_func)
		return;

	va_start(ap, format);
	util_debug_va(micp->debug_func, micp->debug_data, format, ap);
	va_end(ap);
}

static void micp_disconnected(int err, void *user_data)
{
	struct bt_micp *micp = user_data;

	DBG(micp, "micp %p disconnected err %d", micp, err);

	bt_micp_detach(micp);
}

static struct bt_micp *micp_get_session(struct bt_att *att, struct gatt_db *db)
{
	const struct queue_entry *entry;
	struct bt_micp *micp;

	for (entry = queue_get_entries(sessions); entry; entry = entry->next) {
		struct bt_micp *micp = entry->data;

		if (att == bt_micp_get_att(micp))
			return micp;
	}

	micp = bt_micp_new(db, NULL);
	micp->att = att;

	bt_att_register_disconnect(att, micp_disconnected, micp, NULL);

	bt_micp_attach(micp, NULL);

	return micp;
}

static void mics_mute_read(struct gatt_db_attribute *attrib,
			unsigned int id, uint16_t offset,
			uint8_t opcode, struct bt_att *att,
			void *user_data)
{
	struct bt_mics *mics = user_data;
	struct iovec iov;

	iov.iov_base = &mics->mute_stat;
	iov.iov_len = sizeof(mics->mute_stat);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
								  iov.iov_len);
}

static uint8_t mics_not_muted(struct bt_mics *mics, struct bt_micp *micp,
							  struct iovec *iov)
{
	struct bt_micp_db *mdb;
	uint8_t *mute_state;

	DBG(micp, "Mute state OP: Not Muted");

	mdb = micp_get_mdb(micp);
	if (!mdb) {
		DBG(micp, "error: MDB not available");
		return 0;
	}

	mute_state = mdb_get_mute_state(mdb);
	if (!mute_state) {
		DBG(micp, "Error : Mute State not available");
		return 0;
	}

	*mute_state = MICS_NOT_MUTED;

	gatt_db_attribute_notify(mdb->mics->ms, (void *)mute_state,
				sizeof(uint8_t), bt_micp_get_att(micp));

	return 0;
}

static uint8_t mics_muted(struct bt_mics *mics, struct bt_micp *micp,
						  struct iovec *iov)
{
	struct bt_micp_db *mdb;
	uint8_t *mute_state;

	DBG(micp, "Mute state OP: Muted");

	mdb = micp_get_mdb(micp);
	if (!mdb) {
		DBG(micp, "error: MDB not available");
		return 0;
	}

	mute_state = mdb_get_mute_state(mdb);

	*mute_state = MICS_MUTED;

	gatt_db_attribute_notify(mdb->mics->ms, (void *)mute_state,
				sizeof(uint8_t), bt_micp_get_att(micp));

	return 0;
}

#define MICS_OP(_str, _op, _size, _func) \
	{                                    \
		.str = _str,                     \
		.op = _op,                       \
		.size = _size,                   \
		.func = _func,                   \
	}

struct mics_op_handler {
	const char *str;
	uint8_t op;
	size_t size;
	uint8_t (*func)(struct bt_mics *mics, struct bt_micp *micp,
					struct iovec *iov);
} micp_handlers[] = {
	MICS_OP("Not Muted", MICS_NOT_MUTED,
			sizeof(uint8_t), mics_not_muted),
	MICS_OP("Muted", MICS_MUTED,
			sizeof(uint8_t), mics_muted),
	{}};

static void mics_mute_write(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct bt_mics *mics = user_data;
	struct bt_micp *micp = micp_get_session(att, mics->mdb->db);
	struct iovec iov = {
		.iov_base = (void *)value,
		.iov_len = len,
	};
	uint8_t *micp_op, *mute_state;
	struct mics_op_handler *handler;
	uint8_t ret = BT_ATT_ERROR_REQUEST_NOT_SUPPORTED;
	struct bt_micp_db *mdb;

	DBG(micp, "MICS Mute Char write: len: %ld: %ld", len, iov.iov_len);

	if (offset) {
		DBG(micp, "invalid offset: %d", offset);
		ret = BT_ATT_ERROR_INVALID_OFFSET;
		goto respond;
	}

	if (len < sizeof(*micp_op)) {
		DBG(micp, "invalid length: %ld < %ld sizeof(param)", len,
			sizeof(*micp_op));
		ret = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto respond;
	}

	micp_op = iov_pull_mem(&iov, sizeof(*micp_op));

	if ((*micp_op == MICS_DISABLED) || (*micp_op != MICS_NOT_MUTED
		&& *micp_op != MICS_MUTED)) {
		DBG(micp, "Invalid operation - MICS DISABLED/RFU mics op:%d",
					micp_op);
		ret = MICP_ERROR_VALUE_NOT_ALLOWED;
		goto respond;
	}

	mdb = micp_get_mdb(micp);
	if (!mdb) {
		DBG(micp, "error: MDB not available");
		goto respond;
	}

	mute_state = mdb_get_mute_state(mdb);
	if (*mute_state == MICS_DISABLED) {
		DBG(micp, "state: MICS DISABLED , can not write value: %d",
				*micp_op);
		ret = MICP_ERROR_MUTE_DISABLED;
		goto respond;
	}

	for (handler = micp_handlers; handler && handler->str; handler++) {
		DBG(micp, "handler->op: %d micp_op: %d iov.iov_len: %ld",
					handler->op, *micp_op, iov.iov_len);
		if (handler->op != *micp_op)
			continue;

		if (len < handler->size) {
			DBG(micp, "invalid len %ld : %ld < %ld handler->size",
			len, iov.iov_len, handler->size);
			ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
			goto respond;
		}

		break;
	}

	if (handler && handler->str) {
		DBG(micp, "%s", handler->str);

		ret = handler->func(mics, micp, &iov);
	} else {
		DBG(micp, "unknown opcode 0x%02x", *micp_op);
		ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
	}

respond:
	gatt_db_attribute_write_result(attrib, id, ret);
}

static struct bt_mics *mics_new(struct gatt_db *db)
{
	struct bt_mics *mics;
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	mics = new0(struct bt_mics, 1);

	mics->mute_stat = MICS_MUTED;

	/* Populate DB with MICS attributes */
	bt_uuid16_create(&uuid, MICS_UUID);
	mics->service = gatt_db_add_service(db, &uuid, true, 4);

	bt_uuid16_create(&uuid, MUTE_CHRC_UUID);
	mics->ms = gatt_db_service_add_characteristic(mics->service,
				&uuid,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_WRITE
				| BT_GATT_CHRC_PROP_NOTIFY,
				mics_mute_read, mics_mute_write,
				mics);

	mics->ms_ccc = gatt_db_service_add_ccc(mics->service,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	gatt_db_service_set_active(mics->service, true);

	return mics;
}

static struct bt_micp_db *micp_db_new(struct gatt_db *db)
{
	struct bt_micp_db *mdb;

	if (!db)
		return NULL;

	mdb = new0(struct bt_micp_db, 1);
	mdb->db = gatt_db_ref(db);

	if (!micp_db)
		micp_db = queue_new();

	mdb->mics = mics_new(db);
	mdb->mics->mdb = mdb;

	queue_push_tail(micp_db, mdb);

	return mdb;
}

static struct bt_micp_db *micp_get_db(struct gatt_db *db)
{
	struct bt_micp_db *mdb;

	mdb = queue_find(micp_db, micp_db_match, db);
	if (mdb)
		return mdb;

	return micp_db_new(db);
}

void bt_micp_add_db(struct gatt_db *db)
{
	micp_db_new(db);
}

bool bt_micp_set_debug(struct bt_micp *micp, bt_micp_debug_func_t func,
			void *user_data, bt_micp_destroy_func_t destroy)
{
	if (!micp)
		return false;

	if (micp->debug_destroy)
		micp->debug_destroy(micp->debug_data);

	micp->debug_func = func;
	micp->debug_destroy = destroy;
	micp->debug_data = user_data;

	return true;
}

unsigned int bt_micp_register(bt_micp_func_t attached, bt_micp_func_t detached,
							  void *user_data)
{
	struct bt_micp_cb *cb;
	static unsigned int id;

	if (!attached && !detached)
		return 0;

	if (!micp_cbs)
		micp_cbs = queue_new();

	cb = new0(struct bt_micp_cb, 1);
	cb->id = ++id ? id : ++id;
	cb->attached = attached;
	cb->detached = detached;
	cb->user_data = user_data;

	queue_push_tail(micp_cbs, cb);

	return cb->id;
}

static bool match_id(const void *data, const void *match_data)
{
	const struct bt_micp_cb *cb = data;
	unsigned int id = PTR_TO_UINT(match_data);

	return (cb->id == id);
}

bool bt_micp_unregister(unsigned int id)
{
	struct bt_micp_cb *cb;

	cb = queue_remove_if(micp_cbs, match_id, UINT_TO_PTR(id));
	if (!cb)
		return false;

	free(cb);

	return true;
}

struct bt_micp *bt_micp_new(struct gatt_db *ldb, struct gatt_db *rdb)
{
	struct bt_micp *micp;
	struct bt_micp_db *mdb;

	if (!ldb)
		return NULL;

	mdb = micp_get_db(ldb);
	if (!mdb)
		return NULL;

	micp = new0(struct bt_micp, 1);
	micp->ldb = mdb;
	micp->pending = queue_new();
	micp->ready_cbs = queue_new();
	micp->notify = queue_new();

	if (!rdb)
		goto done;

	mdb = new0(struct bt_micp_db, 1);
	mdb->db = gatt_db_ref(rdb);

	micp->rdb = mdb;

done:
	bt_micp_ref(micp);

	return micp;
}

static void micp_pending_destroy(void *data)
{
	struct bt_micp_pending *pending = data;
	struct bt_micp *micp = pending->micp;

	if (queue_remove_if(micp->pending, NULL, pending))
		free(pending);
}

static void micp_pending_complete(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_micp_pending *pending = user_data;

	if (pending->func)
		pending->func(pending->micp, success, att_ecode, value, length,
					  pending->userdata);
}

static void micp_read_value(struct bt_micp *micp, uint16_t value_handle,
				micp_func_t func, void *user_data)
{
	struct bt_micp_pending *pending;

	pending = new0(struct bt_micp_pending, 1);
	pending->micp = micp;
	pending->func = func;
	pending->userdata = user_data;

	pending->id = bt_gatt_client_read_value(micp->client, value_handle,
					micp_pending_complete, pending,
					micp_pending_destroy);

	if (!pending->id) {
		DBG(micp, "unable to send read request");
		free(pending);
		return;
	}

	queue_push_tail(micp->pending, pending);
}

static void micp_register(uint16_t att_ecode, void *user_data)
{
	struct bt_micp_notify *notify = user_data;

	if (att_ecode)
		DBG(notify->micp, "MICP register failed 0x%04x", att_ecode);
}

static void micp_notify(uint16_t value_handle, const uint8_t *value,
				uint16_t length, void *user_data)
{
	struct bt_micp_notify *notify = user_data;

	if (notify->func)
		notify->func(notify->micp, value_handle, value, length,
					 notify->user_data);
}

static void micp_notify_destroy(void *data)
{
	struct bt_micp_notify *notify = data;
	struct bt_micp *micp = notify->micp;

	if (queue_remove_if(micp->notify, NULL, notify))
		free(notify);
}

static unsigned int micp_register_notify(struct bt_micp *micp,
					uint16_t value_handle,
					micp_notify_t func,
					void *user_data)
{
	struct bt_micp_notify *notify;

	notify = new0(struct bt_micp_notify, 1);
	notify->micp = micp;
	notify->func = func;
	notify->user_data = user_data;

	notify->id = bt_gatt_client_register_notify(micp->client,
					value_handle, micp_register,
					micp_notify, notify,
					micp_notify_destroy);
	if (!notify->id) {
		DBG(micp, "Unable to register for notifications");
		free(notify);
		return 0;
	}

	queue_push_tail(micp->notify, notify);

	return notify->id;
}

static void micp_mute_state_notify(struct bt_micp *micp, uint16_t value_handle,
					const uint8_t *value, uint16_t length,
					void *user_data)
{
	uint8_t mute_state;

	memcpy(&mute_state, value, sizeof(mute_state));

	DBG(micp, "Mute state: 0x%x", mute_state);
}

static void read_mute_state(struct bt_micp *micp, bool success,
					uint8_t att_ecode, const uint8_t *value,
					uint16_t length, void *user_data)
{
	uint8_t *mute_state;
	struct iovec iov = {
		.iov_base = (void *)value,
		.iov_len = length,
	};

	if (!success) {
		DBG(micp, "Unable to read Mute state: error 0x%02x", att_ecode);
		return;
	}

	mute_state = iov_pull_mem(&iov, sizeof(uint8_t));
	if (mute_state == NULL) {
		DBG(micp, "Unable to get Mute state");
		return;
	}

	DBG(micp, "Mute state: %x", *mute_state);
}

static void foreach_mics_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_micp *micp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_mute;
	struct bt_mics *mics;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
			NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_mute, MUTE_CHRC_UUID);
	if (!bt_uuid_cmp(&uuid, &uuid_mute)) {
		DBG(micp, "MICS Mute characteristic found: handle 0x%04x",
				value_handle);

		mics = micp_get_mics(micp);
		if (!mics || mics->ms)
			return;

		mics->ms = attr;

		micp_read_value(micp, value_handle, read_mute_state, micp);

		micp->mute_id = micp_register_notify(micp, value_handle,
						micp_mute_state_notify, NULL);
	}
}

static void foreach_mics_service(struct gatt_db_attribute *attr,
					void *user_data)
{
	struct bt_micp *micp = user_data;
	struct bt_mics *mics = micp_get_mics(micp);

	mics->service = attr;

	gatt_db_service_set_claimed(attr, true);
	gatt_db_service_foreach_char(attr, foreach_mics_char, micp);
}

unsigned int bt_micp_ready_register(struct bt_micp *micp,
				bt_micp_ready_func_t func, void *user_data,
				bt_micp_destroy_func_t destroy)
{
	struct bt_micp_ready *ready;
	static unsigned int id;

	DBG(micp, "bt_micp_ready_register_Entry\n");
	if (!micp)
		return 0;

	ready = new0(struct bt_micp_ready, 1);
	ready->id = ++id ? id : ++id;
	ready->func = func;
	ready->destroy = destroy;
	ready->data = user_data;

	queue_push_tail(micp->ready_cbs, ready);

	return ready->id;
}

static bool match_ready_id(const void *data, const void *match_data)
{
	const struct bt_micp_ready *ready = data;
	unsigned int id = PTR_TO_UINT(match_data);

	return (ready->id == id);
}

bool bt_micp_ready_unregister(struct bt_micp *micp, unsigned int id)
{
	struct bt_micp_ready *ready;

	ready = queue_remove_if(micp->ready_cbs, match_ready_id,
						UINT_TO_PTR(id));
	if (!ready)
		return false;

	micp_ready_free(ready);

	return true;
}

static struct bt_micp *bt_micp_ref_safe(struct bt_micp *micp)
{
	if (!micp || !micp->ref_count)
		return NULL;

	return bt_micp_ref(micp);
}

static void micp_notify_ready(struct bt_micp *micp)
{
	const struct queue_entry *entry;

	if (!bt_micp_ref_safe(micp))
		return;

	for (entry = queue_get_entries(micp->ready_cbs); entry;
							entry = entry->next) {
		struct bt_micp_ready *ready = entry->data;

		ready->func(micp, ready->data);
	}

	bt_micp_unref(micp);
}

static void micp_idle(void *data)
{
	struct bt_micp *micp = data;

	micp->idle_id = 0;
	micp_notify_ready(micp);
}

bool bt_micp_attach(struct bt_micp *micp, struct bt_gatt_client *client)
{
	bt_uuid_t uuid;

	if (!sessions)
		sessions = queue_new();

	queue_push_tail(sessions, micp);

	if (!client)
		return true;

	if (micp->client)
		return false;

	micp->client = bt_gatt_client_clone(client);
	if (!micp->client)
		return false;

	bt_gatt_client_idle_register(micp->client, micp_idle, micp, NULL);

	bt_uuid16_create(&uuid, MICS_UUID);
	gatt_db_foreach_service(micp->ldb->db, &uuid, foreach_mics_service,
						micp);
	return true;
}
