// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
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
#include "lib/hci.h"

#include "src/shared/queue.h"
#include "src/shared/util.h"
#include "src/shared/timeout.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "src/shared/gatt-client.h"
#include "src/shared/mcp.h"
#include "src/shared/mcs.h"

#define DBG(_mcp, fmt, arg...) \
	mcp_debug(_mcp, "%s:%s() " fmt, __FILE__, __func__, ## arg)

struct bt_mcp_db {
	struct gatt_db *db;
	struct bt_mcs *mcs;
};

struct bt_mcp_pending {
	unsigned int id;
	struct bt_mcp *mcp;
	bt_gatt_client_read_callback_t func;
	void *user_data;
};

struct event_callback {
	const struct bt_mcp_event_callback *cbs;
	void *user_data;
};

struct bt_mcp_session_info {
	uint8_t content_control_id;
	uint32_t cp_op_supported;
};

struct bt_mcp {
	int ref_count;
	struct bt_gatt_client *client;
	struct bt_mcp_db *ldb;
	struct bt_mcp_db *rdb;
	unsigned int mp_name_id;
	unsigned int track_changed_id;
	unsigned int track_title_id;
	unsigned int track_duration_id;
	unsigned int track_position_id;
	unsigned int media_state_id;
	unsigned int media_cp_id;
	unsigned int media_cp_op_supported_id;

	struct bt_mcp_session_info session;
	struct event_callback *cb;

	struct queue *pending;

	bt_mcp_debug_func_t debug_func;
	bt_mcp_destroy_func_t debug_destroy;
	void *debug_data;
	void *user_data;
};

struct bt_mcs {
	struct bt_mcp_db *mdb;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *mp_name;
	struct gatt_db_attribute *track_changed;
	struct gatt_db_attribute *track_changed_ccc;
	struct gatt_db_attribute *track_title;
	struct gatt_db_attribute *track_duration;
	struct gatt_db_attribute *track_position;
	struct gatt_db_attribute *playback_speed;
	struct gatt_db_attribute *seeking_speed;
	struct gatt_db_attribute *play_order;
	struct gatt_db_attribute *play_order_supported;
	struct gatt_db_attribute *media_state;
	struct gatt_db_attribute *media_state_ccc;
	struct gatt_db_attribute *media_cp;
	struct gatt_db_attribute *media_cp_ccc;
	struct gatt_db_attribute *media_cp_op_supportd;
	struct gatt_db_attribute *content_control_id;
	struct gatt_db_attribute *content_control_id_ccc;
};

static struct queue *mcp_db;

static void mcp_debug(struct bt_mcp *mcp, const char *format, ...)
{
	va_list ap;

	if (!mcp || !format || !mcp->debug_func)
		return;

	va_start(ap, format);
	util_debug_va(mcp->debug_func, mcp->debug_data, format, ap);
	va_end(ap);
}

static bool mcp_db_match(const void *data, const void *match_data)
{
	const struct bt_mcp_db *mdb = data;
	const struct gatt_db *db = match_data;

	return (mdb->db == db);
}

static void mcp_db_free(void *data)
{
	struct bt_mcp_db *bdb = data;

	if (!bdb)
		return;

	gatt_db_unref(bdb->db);

	free(bdb->mcs);
	free(bdb);
}

static void mcp_free(void *data)
{
	struct bt_mcp *mcp = data;

	DBG(mcp, "");

	bt_mcp_detach(mcp);

	mcp_db_free(mcp->rdb);

	queue_destroy(mcp->pending, NULL);

	free(mcp);
}

struct bt_mcp *bt_mcp_ref(struct bt_mcp *mcp)
{
	if (!mcp)
		return NULL;

	__sync_fetch_and_add(&mcp->ref_count, 1);

	return mcp;
}

void bt_mcp_unref(struct bt_mcp *mcp)
{
	if (!mcp)
		return;

	if (__sync_sub_and_fetch(&mcp->ref_count, 1))
		return;

	mcp_free(mcp);
}

bool bt_mcp_set_user_data(struct bt_mcp *mcp, void *user_data)
{
	if (!mcp)
		return false;

	mcp->user_data = user_data;

	return true;
}

void *bt_mcp_get_user_data(struct bt_mcp *mcp)
{
	if (!mcp)
		return NULL;

	return mcp->user_data;
}

bool bt_mcp_set_debug(struct bt_mcp *mcp, bt_mcp_debug_func_t func,
			void *user_data, bt_mcp_destroy_func_t destroy)
{
	if (!mcp)
		return false;

	if (mcp->debug_destroy)
		mcp->debug_destroy(mcp->debug_data);

	mcp->debug_func = func;
	mcp->debug_destroy = destroy;
	mcp->debug_data = user_data;

	return true;
}

static void mcs_mp_name_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	char mp_name[] = "";
	struct iovec iov;

	iov.iov_base = mp_name;
	iov.iov_len = sizeof(mp_name);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_track_title_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	char track_title[] = "";
	struct iovec iov;

	iov.iov_base = track_title;
	iov.iov_len = 0;

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_track_duration_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	int32_t track_duration = 0xFFFFFFFF;
	struct iovec iov;

	iov.iov_base = &track_duration;
	iov.iov_len = sizeof(track_duration);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_track_position_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	int32_t track_position = 0xFFFFFFFF;
	struct iovec iov;

	iov.iov_base = &track_position;
	iov.iov_len = sizeof(track_position);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_track_position_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	gatt_db_attribute_write_result(attrib, id,
			BT_ATT_ERROR_INSUFFICIENT_RESOURCES);
}

static void mcs_playback_speed_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	int8_t playback_speed = 0x00;
	struct iovec iov;

	iov.iov_base = &playback_speed;
	iov.iov_len = sizeof(playback_speed);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_playback_speed_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	gatt_db_attribute_write_result(attrib, id,
				BT_ATT_ERROR_INSUFFICIENT_RESOURCES);
}

static void mcs_seeking_speed_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	int8_t seeking_speed = 0x00;
	struct iovec iov;

	iov.iov_base = &seeking_speed;
	iov.iov_len = sizeof(seeking_speed);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_playing_order_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint8_t playing_order = 0x01;
	struct iovec iov;

	iov.iov_base = &playing_order;
	iov.iov_len = sizeof(playing_order);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_playing_order_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	gatt_db_attribute_write_result(attrib, id,
				BT_ATT_ERROR_INSUFFICIENT_RESOURCES);
}

static void mcs_playing_order_supported_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint16_t playing_order_supported = 0x01;
	struct iovec iov;

	iov.iov_base = &playing_order_supported;
	iov.iov_len = sizeof(playing_order_supported);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_media_state_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint8_t media_state = 0x00;
	struct iovec iov;

	iov.iov_base = &media_state;
	iov.iov_len = sizeof(media_state);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_media_cp_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	gatt_db_attribute_write_result(attrib, id,
				BT_ATT_ERROR_INSUFFICIENT_RESOURCES);
}

static void mcs_media_cp_op_supported_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint32_t cp_op_supported = 0x00000000;
	struct iovec iov;

	iov.iov_base = &cp_op_supported;
	iov.iov_len = sizeof(cp_op_supported);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void mcs_media_content_control_id_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint8_t content_control_id = 0x00;
	struct iovec iov;

	iov.iov_base = &content_control_id;
	iov.iov_len = sizeof(content_control_id);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static struct bt_mcs *mcs_new(struct gatt_db *db)
{
	struct bt_mcs *mcs;
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	mcs = new0(struct bt_mcs, 1);

	/* Populate DB with MCS attributes */
	bt_uuid16_create(&uuid, GMCS_UUID);
	mcs->service = gatt_db_add_service(db, &uuid, true, 31);

	bt_uuid16_create(&uuid, MEDIA_PLAYER_NAME_CHRC_UUID);
	mcs->mp_name = gatt_db_service_add_characteristic(mcs->service, &uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					mcs_mp_name_read, NULL,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_TRACK_CHNGD_CHRC_UUID);
	mcs->track_changed = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_NONE,
					BT_GATT_CHRC_PROP_NOTIFY,
					NULL, NULL,
					mcs);

	mcs->track_changed_ccc = gatt_db_service_add_ccc(mcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, MEDIA_TRACK_TITLE_CHRC_UUID);
	mcs->track_title = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					mcs_track_title_read, NULL,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_TRACK_DURATION_CHRC_UUID);
	mcs->track_duration = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					mcs_track_duration_read, NULL,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_TRACK_POSTION_CHRC_UUID);
	mcs->track_position = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_WRITE |
					BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
					mcs_track_position_read,
					mcs_track_position_write,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_PLAYBACK_SPEED_CHRC_UUID);
	mcs->playback_speed = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_WRITE |
					BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
					mcs_playback_speed_read,
					mcs_playback_speed_write,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_SEEKING_SPEED_CHRC_UUID);
	mcs->seeking_speed = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					mcs_seeking_speed_read, NULL,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_PLAYING_ORDER_CHRC_UUID);
	mcs->play_order = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_WRITE |
					BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
					mcs_playing_order_read,
					mcs_playing_order_write,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_PLAY_ORDER_SUPPRTD_CHRC_UUID);
	mcs->play_order_supported = gatt_db_service_add_characteristic(
					mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					mcs_playing_order_supported_read, NULL,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_STATE_CHRC_UUID);
	mcs->media_state = gatt_db_service_add_characteristic(mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					mcs_media_state_read, NULL,
					mcs);

	mcs->media_state_ccc = gatt_db_service_add_ccc(mcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, MEDIA_CP_CHRC_UUID);
	mcs->media_cp = gatt_db_service_add_characteristic(mcs->service, &uuid,
					BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_WRITE |
					BT_GATT_CHRC_PROP_NOTIFY |
					BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
					NULL, mcs_media_cp_write,
					mcs);

	mcs->media_cp_ccc = gatt_db_service_add_ccc(mcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, MEDIA_CP_OP_SUPPORTED_CHRC_UUID);
	mcs->media_cp_op_supportd = gatt_db_service_add_characteristic(
					mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					mcs_media_cp_op_supported_read, NULL,
					mcs);

	bt_uuid16_create(&uuid, MEDIA_CONTENT_CONTROL_ID_CHRC_UUID);
	mcs->content_control_id = gatt_db_service_add_characteristic(
					mcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					mcs_media_content_control_id_read,
					NULL,
					mcs);

	mcs->content_control_id_ccc = gatt_db_service_add_ccc(mcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	gatt_db_service_set_active(mcs->service, false);

	return mcs;
}

static struct bt_mcs *mcp_get_mcs(struct bt_mcp *mcp)
{
	if (!mcp)
		return NULL;

	if (mcp->rdb->mcs)
		return mcp->rdb->mcs;

	mcp->rdb->mcs = new0(struct bt_mcs, 1);
	mcp->rdb->mcs->mdb = mcp->rdb;

	return mcp->rdb->mcs;
}

static unsigned int mcp_send(struct bt_mcp *mcp, uint8_t operation)
{
	struct bt_mcs *mcs = mcp_get_mcs(mcp);
	int ret;
	uint16_t handle;

	DBG(mcp, "mcs %p", mcs);

	if (!mcp->client)
		return -1;

	if (!gatt_db_attribute_get_char_data(mcs->media_cp, NULL, &handle,
					NULL, NULL, NULL))
		return -1;

	ret = bt_gatt_client_write_without_response(mcp->client, handle, false,
					&operation, sizeof(uint8_t));
	if (!ret)
		return -1;

	return 0;
}

unsigned int bt_mcp_play(struct bt_mcp *mcp)
{
	if (!(mcp->session.cp_op_supported & BT_MCS_CMD_PLAY_SUPPORTED))
		return -ENOTSUP;

	DBG(mcp, "mcp %p", mcp);

	return mcp_send(mcp, BT_MCS_CMD_PLAY);
}

unsigned int bt_mcp_pause(struct bt_mcp *mcp)
{
	if (!(mcp->session.cp_op_supported & BT_MCS_CMD_PAUSE_SUPPORTED))
		return -ENOTSUP;

	DBG(mcp, "mcp %p", mcp);

	return mcp_send(mcp, BT_MCS_CMD_PAUSE);
}

unsigned int bt_mcp_stop(struct bt_mcp *mcp)
{
	if (!(mcp->session.cp_op_supported & BT_MCS_CMD_STOP_SUPPORTED))
		return -ENOTSUP;

	DBG(mcp, "mcp %p", mcp);

	return mcp_send(mcp, BT_MCS_CMD_STOP);
}

static void mcp_mp_set_player_name(struct bt_mcp *mcp, const uint8_t *value,
					uint16_t length)
{
	struct event_callback *cb = mcp->cb;

	if (cb && cb->cbs && cb->cbs->player_name)
		cb->cbs->player_name(mcp, value, length);
}

static void mcp_mp_set_track_title(struct bt_mcp *mcp, const uint8_t *value,
					uint16_t length)
{
	struct event_callback *cb = mcp->cb;

	if (cb && cb->cbs && cb->cbs->track_title)
		cb->cbs->track_title(mcp, value, length);
}

static void mcp_mp_set_title_duration(struct bt_mcp *mcp, int32_t duration)
{
	struct event_callback *cb = mcp->cb;

	DBG(mcp, "Track Duration 0x%08x", duration);

	if (cb && cb->cbs && cb->cbs->track_duration)
		cb->cbs->track_duration(mcp, duration);
}

static void mcp_mp_set_title_position(struct bt_mcp *mcp, int32_t position)
{
	struct event_callback *cb = mcp->cb;

	DBG(mcp, "Track Position 0x%08x", position);

	if (cb && cb->cbs && cb->cbs->track_position)
		cb->cbs->track_position(mcp, position);
}

static void mcp_mp_set_media_state(struct bt_mcp *mcp, uint8_t state)
{
	struct event_callback *cb = mcp->cb;

	DBG(mcp, "Media State 0x%02x", state);

	if (cb && cb->cbs && cb->cbs->media_state)
		cb->cbs->media_state(mcp, state);
}

static void read_media_player_name(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (!success) {
		DBG(mcp, "Unable to read media player name: error 0x%02x",
				att_ecode);
		return;
	}

	if (!length)
		return;

	mcp_mp_set_player_name(mcp, value, length);
}

static void read_track_title(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (!success) {
		DBG(mcp, "Unable to read track title: error 0x%02x",
					att_ecode);
		return;
	}

	if (!length)
		return;

	mcp_mp_set_track_title(mcp, value, length);
}

static void read_track_duration(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;
	int32_t duration;

	if (!success) {
		DBG(mcp, "Unable to read track duration: error 0x%02x",
					att_ecode);
		return;
	}

	if (length != sizeof(duration))
		DBG(mcp, "Wrong length received Length : %u", length);

	memcpy(&duration, value, length);
	mcp_mp_set_title_duration(mcp, duration);
}

static void read_track_position(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;
	int32_t position;

	if (!success) {
		DBG(mcp, "Unable to read track position: error 0x%02x",
					att_ecode);
		return;
	}

	if (length != sizeof(position))
		DBG(mcp, "Wrong length received Length : %u", length);

	memcpy(&position, value, length);
	mcp_mp_set_title_position(mcp, position);
}

static void read_media_state(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (!success) {
		DBG(mcp, "Unable to read media state: error 0x%02x",
					att_ecode);
		return;
	}

	if (length != sizeof(uint8_t))
		DBG(mcp, "Wrong length received Length : %u", length);

	mcp_mp_set_media_state(mcp, *value);
}

static void read_media_cp_op_supported(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (!success) {
		DBG(mcp, "Unable to read media CP OP supported: error 0x%02x",
					att_ecode);
		return;
	}

	if (length != sizeof(uint32_t))
		DBG(mcp, "Wrong length received Length : %u", length);

	memcpy(&mcp->session.cp_op_supported, value, sizeof(uint32_t));
	DBG(mcp, "Media Control Point Opcodes Supported 0x%08x",
			mcp->session.cp_op_supported);
}

static void read_content_control_id(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (!success) {
		DBG(mcp, "Unable to read content control id: error 0x%02x",
					att_ecode);
		return;
	}

	if (length != sizeof(uint8_t))
		DBG(mcp, "Wrong length received Length : %u", length);

	DBG(mcp, "Content Control ID 0x%02x", *value);
}

static void mcp_pending_destroy(void *data)
{
	struct bt_mcp_pending *pending = data;
	struct bt_mcp *mcp = pending->mcp;

	queue_remove_if(mcp->pending, NULL, pending);
}

static void mcp_pending_complete(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_mcp_pending *pending = user_data;

	if (pending->func)
		pending->func(success, att_ecode, value, length,
						pending->user_data);
}

static void mcp_read_value(struct bt_mcp *mcp, uint16_t value_handle,
				bt_gatt_client_read_callback_t func,
				void *user_data)
{
	struct bt_mcp_pending *pending;

	pending = new0(struct bt_mcp_pending, 1);
	pending->mcp = mcp;
	pending->func = func;
	pending->user_data = user_data;

	pending->id = bt_gatt_client_read_value(mcp->client, value_handle,
						mcp_pending_complete, pending,
						mcp_pending_destroy);
	if (!pending->id) {
		DBG(mcp, "Unable to send Read request");
		free(pending);
		return;
	}

	queue_push_tail(mcp->pending, pending);
}

static void mcp_mp_name_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Player Name notification failed: 0x%04x",
					att_ecode);
}

static void mcp_mp_name_notify(uint16_t value_handle, const uint8_t *value,
					uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (!length)
		return;

	mcp_mp_set_player_name(mcp, value, length);
}

static void mcp_track_changed_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Track Changed notification failed: 0x%04x",
					att_ecode);
}

static void mcp_track_changed_notify(uint16_t value_handle,
			const uint8_t *value, uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;
	struct event_callback *cb = mcp->cb;

	DBG(mcp, "Track Changed");

	if (cb && cb->cbs && cb->cbs->track_changed)
		cb->cbs->track_changed(mcp);
}

static void mcp_track_title_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Track Title notification failed: 0x%04x",
					att_ecode);
}

static void mcp_track_title_notify(uint16_t value_handle,
			const uint8_t *value, uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	mcp_mp_set_track_title(mcp, value, length);
}

static void mcp_track_duration_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Track Duration notification failed: 0x%04x",
					att_ecode);
}

static void mcp_track_duration_notify(uint16_t value_handle,
			const uint8_t *value, uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;
	int32_t duration;

	memcpy(&duration, value, sizeof(int32_t));
	mcp_mp_set_title_duration(mcp, duration);
}

static void mcp_track_position_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Track Position notification failed: 0x%04x",
					att_ecode);
}

static void mcp_track_position_notify(uint16_t value_handle,
		const uint8_t *value, uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;
	int32_t position;

	memcpy(&position, value, sizeof(int32_t));
	mcp_mp_set_title_position(mcp, position);
}

static void mcp_media_state_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Media State notification failed: 0x%04x",
					att_ecode);
}

static void mcp_media_state_notify(uint16_t value_handle,
			const uint8_t *value, uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	mcp_mp_set_media_state(mcp, *value);
}

static void mcp_media_cp_register(uint16_t att_ecode, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Media CP notification failed: 0x%04x",
					att_ecode);
}

static void mcp_media_cp_notify(uint16_t value_handle, const uint8_t *value,
					uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	DBG(mcp, "Media CP Notification");
}

static void mcp_media_cp_op_supported_register(uint16_t att_ecode,
					void *user_data)
{
	struct bt_mcp *mcp = user_data;

	if (att_ecode)
		DBG(mcp, "Media Media CP OP Supported notify failed: 0x%04x",
					att_ecode);
}

static void mcp_media_cp_op_supported_notify(uint16_t value_handle,
			const uint8_t *value, uint16_t length, void *user_data)
{
	struct bt_mcp *mcp = user_data;

	memcpy(&mcp->session.cp_op_supported, value, sizeof(uint32_t));
	DBG(mcp, "Media CP Opcodes Supported Notification 0x%08x",
			mcp->session.cp_op_supported);
}

static void bt_mcp_mp_name_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->mp_name, NULL, &value_handle,
						NULL, NULL, NULL))
		return;

	DBG(mcp, "Media Player handle 0x%04x", value_handle);

	mcp_read_value(mcp, value_handle, read_media_player_name, mcp);

	mcp->mp_name_id = bt_gatt_client_register_notify(mcp->client,
				value_handle, mcp_mp_name_register,
				mcp_mp_name_notify, mcp, NULL);
}

static void bt_mcp_track_changed_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->track_changed, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Track Changed handle 0x%04x", value_handle);

	mcp->track_changed_id = bt_gatt_client_register_notify(mcp->client,
				value_handle, mcp_track_changed_register,
				mcp_track_changed_notify, mcp, NULL);
}

static void bt_mcp_track_title_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->track_title, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Track Title handle 0x%04x", value_handle);

	mcp_read_value(mcp, value_handle, read_track_title, mcp);

	mcp->track_title_id = bt_gatt_client_register_notify(mcp->client,
				value_handle, mcp_track_title_register,
				mcp_track_title_notify, mcp, NULL);
}

static void bt_mcp_track_duration_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->track_duration, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Track Duration handle 0x%04x", value_handle);

	mcp_read_value(mcp, value_handle, read_track_duration, mcp);

	mcp->track_duration_id = bt_gatt_client_register_notify(mcp->client,
				value_handle, mcp_track_duration_register,
				mcp_track_duration_notify, mcp, NULL);
}

static void bt_mcp_track_position_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->track_position, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Track Position handle 0x%04x", value_handle);

	mcp_read_value(mcp, value_handle, read_track_position, mcp);

	mcp->track_position_id = bt_gatt_client_register_notify(mcp->client,
				value_handle, mcp_track_position_register,
				mcp_track_position_notify, mcp, NULL);
}

static void bt_mcp_media_state_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->media_state, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Media State handle 0x%04x", value_handle);

	mcp_read_value(mcp, value_handle, read_media_state, mcp);

	mcp->media_state_id = bt_gatt_client_register_notify(mcp->client,
					value_handle, mcp_media_state_register,
					mcp_media_state_notify, mcp, NULL);
}

static void bt_mcp_media_cp_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->media_cp, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Media Control Point handle 0x%04x", value_handle);

	mcp->media_cp_id = bt_gatt_client_register_notify(mcp->client,
					value_handle, mcp_media_cp_register,
					mcp_media_cp_notify, mcp, NULL);
}

static void bt_mcp_media_cp_op_supported_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->media_cp_op_supportd, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Media Control Point Opcodes Supported handle 0x%04x",
			value_handle);

	mcp_read_value(mcp, value_handle, read_media_cp_op_supported, mcp);

	mcp->media_cp_op_supported_id = bt_gatt_client_register_notify(
		mcp->client, value_handle, mcp_media_cp_op_supported_register,
		mcp_media_cp_op_supported_notify, mcp, NULL);
}

static void bt_mcp_content_control_id_supported_attach(struct bt_mcp *mcp)
{
	uint16_t value_handle;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	if (!gatt_db_attribute_get_char_data(mcs->content_control_id, NULL,
				&value_handle, NULL, NULL, NULL))
		return;

	DBG(mcp, "Media Content Control id Supported handle 0x%04x",
				value_handle);
	mcp_read_value(mcp, value_handle, read_content_control_id, mcp);
}

static void foreach_mcs_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_mcp *mcp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_mp_name, uuid_track_changed, uuid_track_title,
		uuid_track_duration, uuid_track_position, uuid_media_state,
		uuid_media_cp, uuid_media_cp_op_supported,
		uuid_content_control_id;
	struct bt_mcs *mcs;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_mp_name, MEDIA_PLAYER_NAME_CHRC_UUID);
	bt_uuid16_create(&uuid_track_changed, MEDIA_TRACK_CHNGD_CHRC_UUID);
	bt_uuid16_create(&uuid_track_title, MEDIA_TRACK_TITLE_CHRC_UUID);
	bt_uuid16_create(&uuid_track_duration, MEDIA_TRACK_DURATION_CHRC_UUID);
	bt_uuid16_create(&uuid_track_position, MEDIA_TRACK_POSTION_CHRC_UUID);
	bt_uuid16_create(&uuid_media_state, MEDIA_STATE_CHRC_UUID);
	bt_uuid16_create(&uuid_media_cp, MEDIA_CP_CHRC_UUID);
	bt_uuid16_create(&uuid_media_cp_op_supported,
					MEDIA_CP_OP_SUPPORTED_CHRC_UUID);
	bt_uuid16_create(&uuid_content_control_id,
					MEDIA_CONTENT_CONTROL_ID_CHRC_UUID);

	if (!bt_uuid_cmp(&uuid, &uuid_mp_name)) {
		DBG(mcp, "Media Player Name found: handle 0x%04x",
					value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->mp_name)
			return;

		mcs->mp_name = attr;
		bt_mcp_mp_name_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_track_changed)) {
		DBG(mcp, "Track Changed found: handle 0x%04x", value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->track_changed)
			return;

		mcs->track_changed = attr;
		bt_mcp_track_changed_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_track_title)) {
		DBG(mcp, "Track Title found: handle 0x%04x", value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->track_title)
			return;

		mcs->track_title = attr;
		bt_mcp_track_title_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_track_duration)) {
		DBG(mcp, "Track Duration found: handle 0x%04x", value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->track_duration)
			return;

		mcs->track_duration = attr;
		bt_mcp_track_duration_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_track_position)) {
		DBG(mcp, "Track Position found: handle 0x%04x", value_handle);


		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->track_position)
			return;

		mcs->track_position = attr;
		bt_mcp_track_position_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_media_state)) {
		DBG(mcp, "Media State found: handle 0x%04x", value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->media_state)
			return;

		mcs->media_state = attr;
		bt_mcp_media_state_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_media_cp)) {
		DBG(mcp, "Media Control Point found: handle 0x%04x",
					value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->media_cp)
			return;

		mcs->media_cp = attr;
		bt_mcp_media_cp_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_media_cp_op_supported)) {
		DBG(mcp, "Media CP Opcodes Supported found: handle 0x%04x",
					value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->media_cp_op_supportd)
			return;

		mcs->media_cp_op_supportd = attr;
		bt_mcp_media_cp_op_supported_attach(mcp);
	}

	if (!bt_uuid_cmp(&uuid, &uuid_content_control_id)) {
		DBG(mcp, "Content Control ID found: handle 0x%04x",
					value_handle);

		mcs = mcp_get_mcs(mcp);
		if (!mcs || mcs->content_control_id)
			return;

		mcs->content_control_id = attr;
		bt_mcp_content_control_id_supported_attach(mcp);
	}
}

void bt_mcp_set_event_callbacks(struct bt_mcp *mcp,
				const struct bt_mcp_event_callback *cbs,
				void *user_data)
{
	struct event_callback *cb;

	if (mcp->cb)
		free(mcp->cb);

	cb = new0(struct event_callback, 1);
	cb->cbs = cbs;
	cb->user_data = user_data;

	mcp->cb = cb;
}

static void foreach_mcs_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_mcp *mcp = user_data;
	struct bt_mcs *mcs = mcp_get_mcs(mcp);

	DBG(mcp, "");

	mcs->service = attr;

	gatt_db_service_foreach_char(attr, foreach_mcs_char, mcp);
}

static struct bt_mcp_db *mcp_db_new(struct gatt_db *db)
{
	struct bt_mcp_db *mdb;

	if (!db)
		return NULL;

	mdb = new0(struct bt_mcp_db, 1);
	mdb->db = gatt_db_ref(db);

	if (!mcp_db)
		mcp_db = queue_new();

	queue_push_tail(mcp_db, mdb);

	mdb->mcs = mcs_new(db);
	return mdb;
}

static struct bt_mcp_db *mcp_get_db(struct gatt_db *db)
{
	struct bt_mcp_db *mdb;

	mdb = queue_find(mcp_db, mcp_db_match, db);
	if (mdb)
		return mdb;

	return mcp_db_new(db);
}

struct bt_mcp *bt_mcp_new(struct gatt_db *ldb, struct gatt_db *rdb)
{
	struct bt_mcp *mcp;
	struct bt_mcp_db *mdb;

	if (!ldb)
		return NULL;

	mdb = mcp_get_db(ldb);
	if (!mdb)
		return NULL;

	mcp = new0(struct bt_mcp, 1);
	mcp->ldb = mdb;
	mcp->pending = queue_new();

	if (!rdb)
		goto done;

	mdb = new0(struct bt_mcp_db, 1);
	mdb->db = gatt_db_ref(rdb);

	mcp->rdb = mdb;

done:
	bt_mcp_ref(mcp);

	return mcp;
}

void bt_mcp_register(struct gatt_db *db)
{
	mcp_db_new(db);
}

bool bt_mcp_attach(struct bt_mcp *mcp, struct bt_gatt_client *client)
{
	bt_uuid_t uuid;

	DBG(mcp, "mcp %p", mcp);

	mcp->client = bt_gatt_client_clone(client);
	if (!mcp->client)
		return false;

	if (mcp->rdb->mcs) {
		bt_mcp_mp_name_attach(mcp);
		bt_mcp_track_changed_attach(mcp);
		bt_mcp_track_title_attach(mcp);
		bt_mcp_track_duration_attach(mcp);
		bt_mcp_track_position_attach(mcp);
		bt_mcp_media_state_attach(mcp);
		bt_mcp_media_cp_attach(mcp);
		bt_mcp_media_cp_op_supported_attach(mcp);
		bt_mcp_content_control_id_supported_attach(mcp);

		return true;
	}

	bt_uuid16_create(&uuid, GMCS_UUID);
	gatt_db_foreach_service(mcp->rdb->db, &uuid, foreach_mcs_service, mcp);

	return true;
}

void bt_mcp_detach(struct bt_mcp *mcp)
{
	DBG(mcp, "%p", mcp);

	bt_gatt_client_unref(mcp->client);
	mcp->client = NULL;
}
