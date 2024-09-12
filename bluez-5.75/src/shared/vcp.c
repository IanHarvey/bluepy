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

#include "src/shared/queue.h"
#include "src/shared/util.h"
#include "src/shared/timeout.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "src/shared/gatt-client.h"
#include "src/shared/vcp.h"

#define DBG(_vcp, fmt, arg...) \
	vcp_debug(_vcp, "%s:%s() " fmt, __FILE__, __func__, ## arg)

#define VCP_STEP_SIZE 1

#define VOCS_VOL_OFFSET_UPPER_LIMIT	 255
#define VOCS_VOL_OFFSET_LOWER_LIMIT	-255

/* Apllication Error Code */
#define BT_ATT_ERROR_INVALID_CHANGE_COUNTER	0x80
#define BT_ATT_ERROR_OPCODE_NOT_SUPPORTED	0x81
#define BT_ATT_ERROR_VALUE_OUT_OF_RANGE		0x82
#define BT_ATT_AICS_ERROR_VALUE_OUT_OF_RANGE	0x83
#define BT_ATT_AICS_ERROR_MUTE_DISABLED			0x82
#define BT_ATT_AICS_ERROR_GAIN_MODE_CHANGE_NOT_ALLOWED	0x84

#define BT_VCP_NA                   BIT(0)
#define BT_VCP_FRONT_LEFT           BIT(1)
#define BT_VCP_FRONT_RIGHT          BIT(2)
#define BT_VCP_FRONT_CENTER         BIT(3)
#define BT_VCP_LOW_FRQ_EFF_1        BIT(4)
#define BT_VCP_BACK_LEFT            BIT(5)
#define BT_VCP_BACK_RIGHT           BIT(6)
#define BT_VCP_FRONT_LEFT_CENTER    BIT(7)
#define BT_VCP_FRONT_RIGHT_CENTER   BIT(8)
#define BT_VCP_BACK_CENTER          BIT(9)
#define BT_VCP_LOW_FRQ_EFF_2        BIT(10)
#define BT_VCP_SIDE_LEFT            BIT(11)
#define BT_VCP_SIDE_RIGHT           BIT(12)
#define BT_VCP_TOP_FRONT_LEFT       BIT(13)
#define BT_VCP_TOP_FRONT_RIGHT      BIT(14)
#define BT_VCP_TOP_FRONT_CENTER     BIT(15)
#define BT_VCP_TOP_CENTER           BIT(16)
#define BT_VCP_TOP_BACK_LEFT        BIT(17)
#define BT_VCP_TOP_BACK_RIGHT       BIT(18)
#define BT_VCP_TOP_SIDE_LEFT        BIT(19)
#define BT_VCP_TOP_SIDE_RIGHT       BIT(20)
#define BT_VCP_TOP_BACK_CENTER      BIT(21)
#define BT_VCP_BOTTOM_FRONT_CENTER  BIT(22)
#define BT_VCP_BOTTOM_FRONT_LEFT    BIT(23)
#define BT_VCP_BOTTOM_FRONT_RIGHT   BIT(24)
#define BT_VCP_FRONT_LEFT_WIDE      BIT(25)
#define BT_VCP_FRONT_RIGHT_WIDE     BIT(26)
#define BT_VCP_LEFT_SURROUND        BIT(27)
#define BT_VCP_RIGHT_SURROUND       BIT(28)

#define VCS_TOTAL_NUM_HANDLES	11
#define AICS_TOTAL_NUM_HANDLES	16

/* AICS Audio Input Type Values */
#define AICS_AUD_IP_TYPE_UNSPECIFIED		0x00
#define AICS_AUD_IP_TYPE_BLUETOOTH		0x01
#define AICS_AUD_IP_TYPE_MICROPHONE		0x02
#define AICS_AUD_IP_TYPE_ANALOG		0x03
#define AICS_AUD_IP_TYPE_DIGITAL		0x04
#define AICS_AUD_IP_TYPE_RADIO			0x05
#define AICS_AUD_IP_TYPE_STREAMING		0x06
#define AICS_AUD_IP_TYPE_AMBIENT		0x07

/* AICS Audio Input Status Values */
#define AICS_AUD_IP_STATUS_INACTIVE	0x00
#define AICS_AUD_IP_STATUS_ACTIVE	0x01

/* AICS Audio Input Control Point Opcodes */
#define BT_AICS_SET_GAIN_SETTING		0x01
#define BT_AICS_UNMUTE				0x02
#define BT_AICS_MUTE				0x03
#define BT_AICS_SET_MANUAL_GAIN_MODE		0x04
#define BT_AICS_SET_AUTO_GAIN_MODE		0x05

/* AICS Gain Mode Field Value */
#define AICS_GAIN_MODE_MANUAL_ONLY		0x00
#define AICS_GAIN_MODE_AUTO_ONLY		0x01
#define AICS_GAIN_MODE_MANUAL			0x02
#define AICS_GAIN_MODE_AUTO			0x03

/* AICS Mute Field Values */
#define AICS_NOT_MUTED	0x00
#define AICS_MUTED	0x01
#define AICS_DISABLED	0x02

#define AICS_GAIN_SETTING_UNITS	1
#define AICS_GAIN_SETTING_MAX_VALUE	127
#define AICS_GAIN_SETTING_MIN_VALUE	-128

#define AICS_GAIN_SETTING_DEFAULT_VALUE	88

struct bt_vcp_db {
	struct gatt_db *db;
	struct bt_vcs *vcs;
	struct bt_vocs *vocs;
	struct bt_aics *aics;
};

typedef void (*vcp_func_t)(struct bt_vcp *vcp, bool success, uint8_t att_ecode,
					const uint8_t *value, uint16_t length,
					void *user_data);

struct bt_vcp_pending {
	unsigned int id;
	struct bt_vcp *vcp;
	vcp_func_t func;
	void *user_data;
};

struct bt_vcs_param {
	uint8_t	op;
	uint8_t	change_counter;
} __packed;

struct bt_vocs_param {
	uint8_t	op;
	uint8_t	change_counter;
} __packed;

struct bt_vcs_ab_vol {
	uint8_t	change_counter;
	uint8_t	vol_set;
} __packed;

struct bt_vocs_set_vol_off {
	uint8_t	change_counter;
	int16_t set_vol_offset;
} __packed;

struct bt_vcp_cb {
	unsigned int id;
	bt_vcp_func_t attached;
	bt_vcp_func_t detached;
	void *user_data;
};

typedef void (*vcp_notify_t)(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data);

struct bt_vcp_notify {
	unsigned int id;
	struct bt_vcp *vcp;
	vcp_notify_t func;
	void *user_data;
};

struct bt_vcp {
	int ref_count;
	struct bt_vcp_db *ldb;
	struct bt_vcp_db *rdb;
	struct bt_gatt_client *client;
	struct bt_att *att;
	unsigned int vstate_id;
	unsigned int vflag_id;

	unsigned int state_id;
	unsigned int audio_loc_id;
	unsigned int ao_dec_id;

	unsigned int aics_ip_state_id;
	unsigned int aics_ip_status_id;
	unsigned int aics_ip_descr_id;

	struct queue *notify;
	struct queue *pending;

	bt_vcp_debug_func_t debug_func;
	bt_vcp_destroy_func_t debug_destroy;
	void *debug_data;
	void *user_data;
};

#define RESET_VOLUME_SETTING 0x00
#define USERSET_VOLUME_SETTING 0x01

/* Contains local bt_vcp_db */
struct vol_state {
	uint8_t	vol_set;
	uint8_t	mute;
	uint8_t counter;
} __packed;

struct bt_vcs {
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t vol_flag;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *vs;
	struct gatt_db_attribute *vs_ccc;
	struct gatt_db_attribute *vol_cp;
	struct gatt_db_attribute *vf;
	struct gatt_db_attribute *vf_ccc;
};

/* Contains local bt_vcp_db */
struct vol_offset_state {
	int16_t vol_offset;
	uint8_t counter;
} __packed;

struct bt_vocs {
	struct bt_vcp_db *vdb;
	struct vol_offset_state *vostate;
	uint32_t vocs_audio_loc;
	char *vocs_ao_dec;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *vos;
	struct gatt_db_attribute *vos_ccc;
	struct gatt_db_attribute *voal;
	struct gatt_db_attribute *voal_ccc;
	struct gatt_db_attribute *vo_cp;
	struct gatt_db_attribute *voaodec;
	struct gatt_db_attribute *voaodec_ccc;
};

struct aud_ip_st {
	int8_t	gain_setting;
	uint8_t	mute;
	uint8_t	gain_mode;
	uint8_t	chg_counter;
} __packed;

struct gain_setting_prop {
	uint8_t	gain_setting_units;
	int8_t	gain_setting_min;
	int8_t	gain_setting_max;
} __packed;

struct bt_aics_set_gain_setting {
	uint8_t change_counter;
	int8_t gain_setting;
} __packed;

struct bt_aics {
	struct bt_vcp_db *vdb;
	struct aud_ip_st *aud_ipst;
	struct gain_setting_prop *gain_settingprop;
	uint8_t	aud_input_type;
	uint8_t	aud_input_status;
	char *aud_input_descr;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *aud_ip_state;
	struct gatt_db_attribute *aud_ip_state_ccc;
	struct gatt_db_attribute *gain_stting_prop;
	struct gatt_db_attribute *aud_ip_type;
	struct gatt_db_attribute *aud_ip_status;
	struct gatt_db_attribute *aud_ip_status_ccc;
	struct gatt_db_attribute *aud_ip_cp;
	struct gatt_db_attribute *aud_ip_dscrptn;
	struct gatt_db_attribute *aud_ip_dscrptn_ccc;
};

static struct queue *vcp_db;
static struct queue *vcp_cbs;
static struct queue *sessions;

static void *iov_pull_mem(struct iovec *iov, size_t len)
{
	void *data = iov->iov_base;

	if (iov->iov_len < len)
		return NULL;

	iov->iov_base += len;
	iov->iov_len -= len;

	return data;
}

static struct bt_vcp_db *vcp_get_vdb(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->ldb)
		return vcp->ldb;

	return NULL;
}

static struct vol_state *vdb_get_vstate(struct bt_vcp_db *vdb)
{
	if (!vdb->vcs)
		return NULL;

	if (vdb->vcs->vstate)
		return vdb->vcs->vstate;

	return NULL;
}

static struct vol_offset_state *vdb_get_vostate(struct bt_vcp_db *vdb)
{
	if (!vdb->vocs)
		return NULL;

	if (vdb->vocs->vostate)
		return vdb->vocs->vostate;

	return NULL;
}

static struct bt_vcs *vcp_get_vcs(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->rdb->vcs)
		return vcp->rdb->vcs;

	vcp->rdb->vcs = new0(struct bt_vcs, 1);
	vcp->rdb->vcs->vdb = vcp->rdb;

	return vcp->rdb->vcs;
}

static struct bt_vocs *vcp_get_vocs(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->rdb->vocs)
		return vcp->rdb->vocs;

	vcp->rdb->vocs = new0(struct bt_vocs, 1);
	vcp->rdb->vocs->vdb = vcp->rdb;

	return vcp->rdb->vocs;
}

static struct bt_aics *vcp_get_aics(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->rdb->aics)
		return vcp->rdb->aics;

	vcp->rdb->aics = new0(struct bt_aics, 1);
	vcp->rdb->aics->vdb = vcp->rdb;

	return vcp->rdb->aics;
}

static void vcp_detached(void *data, void *user_data)
{
	struct bt_vcp_cb *cb = data;
	struct bt_vcp *vcp = user_data;

	cb->detached(vcp, cb->user_data);
}

void bt_vcp_detach(struct bt_vcp *vcp)
{
	if (!queue_remove(sessions, vcp))
		return;

	bt_gatt_client_unref(vcp->client);
	vcp->client = NULL;

	queue_foreach(vcp_cbs, vcp_detached, vcp);
}

static void vcp_db_free(void *data)
{
	struct bt_vcp_db *vdb = data;

	if (!vdb)
		return;

	gatt_db_unref(vdb->db);

	free(vdb->vcs);
	free(vdb->vocs);
	free(vdb->aics);
	free(vdb);
}

static void vcp_free(void *data)
{
	struct bt_vcp *vcp = data;

	bt_vcp_detach(vcp);

	vcp_db_free(vcp->rdb);

	queue_destroy(vcp->pending, NULL);

	free(vcp);
}
bool bt_vcp_set_user_data(struct bt_vcp *vcp, void *user_data)
{
	if (!vcp)
		return false;

	vcp->user_data = user_data;

	return true;
}

static bool vcp_db_match(const void *data, const void *match_data)
{
	const struct bt_vcp_db *vdb = data;
	const struct gatt_db *db = match_data;

	return (vdb->db == db);
}

struct bt_att *bt_vcp_get_att(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->att)
		return vcp->att;

	return bt_gatt_client_get_att(vcp->client);
}

struct bt_vcp *bt_vcp_ref(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	__sync_fetch_and_add(&vcp->ref_count, 1);

	return vcp;
}

void bt_vcp_unref(struct bt_vcp *vcp)
{
	if (!vcp)
		return;

	if (__sync_sub_and_fetch(&vcp->ref_count, 1))
		return;

	vcp_free(vcp);
}

static void vcp_debug(struct bt_vcp *vcp, const char *format, ...)
{
	va_list ap;

	if (!vcp || !format || !vcp->debug_func)
		return;

	va_start(ap, format);
	util_debug_va(vcp->debug_func, vcp->debug_data, format, ap);
	va_end(ap);
}

static void vcp_disconnected(int err, void *user_data)
{
	struct bt_vcp *vcp = user_data;

	DBG(vcp, "vcp %p disconnected err %d", vcp, err);

	bt_vcp_detach(vcp);
}

static struct bt_vcp *vcp_get_session(struct bt_att *att, struct gatt_db *db)
{
	const struct queue_entry *entry;
	struct bt_vcp *vcp;

	for (entry = queue_get_entries(sessions); entry; entry = entry->next) {
		struct bt_vcp *vcp = entry->data;

		if (att == bt_vcp_get_att(vcp))
			return vcp;
	}

	vcp = bt_vcp_new(db, NULL);
	vcp->att = att;

	bt_att_register_disconnect(att, vcp_disconnected, vcp, NULL);

	bt_vcp_attach(vcp, NULL);

	return vcp;

}

static uint8_t vcs_rel_vol_down(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Volume Down");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_set = MAX((vstate->vol_set - VCP_STEP_SIZE), 0);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_rel_vol_up(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Volume Up");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VCP database not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_set = MIN((vstate->vol_set + VCP_STEP_SIZE), 255);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_unmute_rel_vol_down(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Un Mute and Volume Down");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VCP database not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x00;
	vstate->vol_set = MAX((vstate->vol_set - VCP_STEP_SIZE), 0);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_unmute_rel_vol_up(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "UN Mute and Volume Up");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x00;
	vstate->vol_set = MIN((vstate->vol_set + VCP_STEP_SIZE), 255);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_set_absolute_vol(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	struct bt_vcs_ab_vol *req;

	DBG(vcp, "Set Absolute Volume");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	req = iov_pull_mem(iov, sizeof(*req));
	if (!req)
		return 0;

	if (req->change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_set = req->vol_set;
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_unmute(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Un Mute");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x00;
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_mute(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "MUTE");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x01;
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	return 0;
}

static uint8_t vocs_set_vol_offset(struct bt_vocs *vocs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_offset_state *vstate, state;
	struct bt_vocs_set_vol_off *req;

	DBG(vcp, "Set Volume Offset");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vostate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	req = iov_pull_mem(iov, sizeof(*req));
	if (!req)
		return 0;

	if (req->change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_offset = le16_to_cpu(req->set_vol_offset);

	if (vstate->vol_offset > VOCS_VOL_OFFSET_UPPER_LIMIT ||
		vstate->vol_offset < VOCS_VOL_OFFSET_LOWER_LIMIT) {
		DBG(vcp, "error: Value Out of Range");
		return BT_ATT_ERROR_VALUE_OUT_OF_RANGE;
	}

	/* Increment Change Counter */
	vstate->counter = -~vstate->counter;

	/* Notify change */
	state.vol_offset = req->set_vol_offset;
	state.counter = vstate->counter;

	gatt_db_attribute_notify(vdb->vocs->vos, (void *)&state, sizeof(state),
				 bt_vcp_get_att(vcp));

	return 0;
}

#define	BT_VCS_REL_VOL_DOWN		0x00
#define	BT_VCS_REL_VOL_UP		0x01
#define	BT_VCS_UNMUTE_REL_VOL_DOWN	0x02
#define	BT_VCS_UNMUTE_REL_VOL_UP	0x03
#define	BT_VCS_SET_ABSOLUTE_VOL		0x04
#define	BT_VCS_UNMUTE			0x05
#define	BT_VCS_MUTE			0x06

#define BT_VOCS_SET_VOL_OFFSET	0x01

#define VCS_OP(_str, _op, _size, _func) \
	{ \
		.str = _str, \
		.op = _op, \
		.size = _size, \
		.func = _func, \
	}

struct vcs_op_handler {
	const char *str;
	uint8_t	op;
	size_t	size;
	uint8_t	(*func)(struct bt_vcs *vcs, struct bt_vcp *vcp,
			struct iovec *iov);
} vcp_handlers[] = {
	VCS_OP("Relative Volume Down", BT_VCS_REL_VOL_DOWN,
		sizeof(uint8_t), vcs_rel_vol_down),
	VCS_OP("Relative Volume Up", BT_VCS_REL_VOL_UP,
		sizeof(uint8_t), vcs_rel_vol_up),
	VCS_OP("Unmute - Relative Volume Down", BT_VCS_UNMUTE_REL_VOL_DOWN,
		sizeof(uint8_t), vcs_unmute_rel_vol_down),
	VCS_OP("Unmute - Relative Volume Up", BT_VCS_UNMUTE_REL_VOL_UP,
		sizeof(uint8_t), vcs_unmute_rel_vol_up),
	VCS_OP("Set Absolute Volume", BT_VCS_SET_ABSOLUTE_VOL,
		sizeof(struct bt_vcs_ab_vol), vcs_set_absolute_vol),
	VCS_OP("UnMute", BT_VCS_UNMUTE,
		sizeof(uint8_t), vcs_unmute),
	VCS_OP("Mute", BT_VCS_MUTE,
		sizeof(uint8_t), vcs_mute),
	{}
};

#define VOCS_OP(_str, _op, _size, _func) \
	{ \
		.str = _str, \
		.op = _op, \
		.size = _size, \
		.func = _func, \
	}

struct vocs_op_handler {
	const char *str;
	uint8_t	op;
	size_t	size;
	uint8_t	(*func)(struct bt_vocs *vocs, struct bt_vcp *vcp,
			struct iovec *iov);
} vocp_handlers[] = {
	VOCS_OP("Set Volume Offset", BT_VOCS_SET_VOL_OFFSET,
		sizeof(uint8_t), vocs_set_vol_offset),
	{}
};

static void vcs_cp_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vcs *vcs = user_data;
	struct bt_vcp *vcp = vcp_get_session(att, vcs->vdb->db);
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = len,
	};
	uint8_t	*vcp_op;
	struct vcs_op_handler *handler;
	uint8_t ret = BT_ATT_ERROR_REQUEST_NOT_SUPPORTED;

	DBG(vcp, "VCP Control Point Write");

	if (offset) {
		DBG(vcp, "invalid offset %d", offset);
		ret = BT_ATT_ERROR_INVALID_OFFSET;
		goto respond;
	}

	if (len < sizeof(*vcp_op)) {
		DBG(vcp, "invalid len %ld < %ld sizeof(*param)", len,
							sizeof(*vcp_op));
		ret = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto respond;
	}

	vcp_op = iov_pull_mem(&iov, sizeof(*vcp_op));

	for (handler = vcp_handlers; handler && handler->str; handler++) {
		if (handler->op != *vcp_op)
			continue;

		if (iov.iov_len < handler->size) {
			DBG(vcp, "invalid len %ld < %ld handler->size", len,
			    handler->size);
			ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
			goto respond;
		}

		break;
	}

	if (handler && handler->str) {
		DBG(vcp, "%s", handler->str);

		ret = handler->func(vcs, vcp, &iov);
	} else {
		DBG(vcp, "Unknown opcode 0x%02x", *vcp_op);
		ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
	}

respond:
	gatt_db_attribute_write_result(attrib, id, ret);
}

static void vocs_cp_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	struct bt_vcp *vcp = vcp_get_session(att, vocs->vdb->db);
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = len,
	};
	uint8_t	*vcp_op;
	struct vocs_op_handler *handler;
	uint8_t ret = BT_ATT_ERROR_REQUEST_NOT_SUPPORTED;

	DBG(vcp, "VOCP Control Point Write");

	if (offset) {
		DBG(vcp, "invalid offset %d", offset);
		ret = BT_ATT_ERROR_INVALID_OFFSET;
		goto respond;
	}

	if (len < sizeof(*vcp_op)) {
		DBG(vcp, "invalid len %ld < %ld sizeof(*param)", len,
							sizeof(*vcp_op));
		ret = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto respond;
	}

	vcp_op = iov_pull_mem(&iov, sizeof(*vcp_op));

	for (handler = vocp_handlers; handler && handler->str; handler++) {
		if (handler->op != *vcp_op)
			continue;

		if (iov.iov_len < handler->size) {
			DBG(vcp, "invalid len %ld < %ld handler->size", len,
			    handler->size);
			ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
			goto respond;
		}

		break;
	}

	if (handler && handler->str) {
		DBG(vcp, "%s", handler->str);

		ret = handler->func(vocs, vcp, &iov);
	} else {
		DBG(vcp, "Unknown opcode 0x%02x", *vcp_op);
		ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
	}

respond:
	gatt_db_attribute_write_result(attrib, id, ret);
}

static void vcs_state_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vcs *vcs = user_data;
	struct iovec iov;

	iov.iov_base = vcs->vstate;
	iov.iov_len = sizeof(*vcs->vstate);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void vocs_state_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	struct vol_offset_state state;

	state.vol_offset = cpu_to_le16(vocs->vostate->vol_offset);
	state.counter = vocs->vostate->counter;

	gatt_db_attribute_read_result(attrib, id, 0, (void *)&state,
					sizeof(state));
}

static void vcs_flag_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vcs *vcs = user_data;
	struct iovec iov;

	iov.iov_base = &vcs->vol_flag;
	iov.iov_len = sizeof(vcs->vol_flag);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void vocs_voal_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	uint32_t loc;

	loc = cpu_to_le32(vocs->vocs_audio_loc);

	gatt_db_attribute_read_result(attrib, id, 0, (void *)&loc,
							sizeof(loc));
}

static void vocs_voaodec_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	struct iovec iov;

	iov.iov_base = vocs->vocs_ao_dec;
	iov.iov_len = strlen(vocs->vocs_ao_dec);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void aics_input_state_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_aics *aics = user_data;
	struct iovec iov;

	iov.iov_base = aics->aud_ipst;
	iov.iov_len = sizeof(*aics->aud_ipst);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void aics_gain_setting_prop_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_aics *aics = user_data;
	struct iovec iov;

	iov.iov_base = aics->gain_settingprop;
	iov.iov_len = sizeof(*aics->gain_settingprop);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void aics_audio_input_type_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_aics *aics = user_data;
	struct iovec iov;

	iov.iov_base = &aics->aud_input_type;
	iov.iov_len = sizeof(aics->aud_input_type);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void aics_input_status_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_aics *aics = user_data;
	struct iovec iov;

	iov.iov_base = &aics->aud_input_status;
	iov.iov_len = sizeof(aics->aud_input_status);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static struct aud_ip_st *vdb_get_audipst(struct bt_vcp_db *vdb)
{
	if (!vdb->aics)
		return NULL;

	if (vdb->aics->aud_ipst)
		return vdb->aics->aud_ipst;

	return NULL;
}

static struct gain_setting_prop *vdb_get_gainsettingprop(
					struct bt_vcp_db *vdb)
{
	if (!vdb->aics)
		return NULL;

	if (vdb->aics->gain_settingprop)
		return vdb->aics->gain_settingprop;

	return NULL;
}

static uint8_t aics_set_gain_setting(struct bt_aics *aics,
				struct bt_vcp *vcp, struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct aud_ip_st *audipst;
	struct bt_aics_set_gain_setting *req;
	struct gain_setting_prop *gainsettngprop;
	uint8_t	ret = 1;

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		ret = 0;
		goto respond;
	}

	audipst = vdb_get_audipst(vdb);
	if (!audipst) {
		DBG(vcp, "error: Audio Input State value is not available");
		ret = 0;
		goto respond;

	}

	req = iov_pull_mem(iov, sizeof(*req));
	if (!req) {
		ret = 0;
		goto respond;

	}

	if (req->change_counter != audipst->chg_counter) {
		DBG(vcp, "Change Counter Mismatch Audio Input State!");
		ret = BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
		goto respond;
	}

	if (audipst->gain_mode != AICS_GAIN_MODE_MANUAL_ONLY &&
		audipst->gain_mode != AICS_GAIN_MODE_MANUAL) {
		DBG(vcp, "Gain Mode is not Manual only or Manual");
		ret = BT_ATT_AICS_ERROR_GAIN_MODE_CHANGE_NOT_ALLOWED;
		goto respond;
	}

	gainsettngprop = vdb_get_gainsettingprop(vdb);
	if (req->gain_setting > gainsettngprop->gain_setting_max ||
		req->gain_setting < gainsettngprop->gain_setting_min) {
		DBG(vcp, "error: Value Out of Range");
		ret = BT_ATT_AICS_ERROR_VALUE_OUT_OF_RANGE;
		goto respond;
	}

	audipst->gain_setting = req->gain_setting;
	/*Increment Change Counter*/
	audipst->chg_counter = -~audipst->chg_counter;
	gatt_db_attribute_notify(vdb->aics->aud_ip_state, (void *)audipst,
				sizeof(struct aud_ip_st),
				bt_vcp_get_att(vcp));
	ret = 0;

respond:
	return ret;
}

static uint8_t aics_unmute(struct bt_aics *aics, struct bt_vcp *vcp,
							struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct aud_ip_st *audipst;
	uint8_t *change_counter;
	uint8_t	ret = 1;

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		ret = 0;
		goto respond;

	}

	audipst = vdb_get_audipst(vdb);
	if (!audipst) {
		DBG(vcp, "error: Audio Input State value is not available");
		ret = 0;
		goto respond;

	}
	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter) {
		ret = 0;
		goto respond;

	}

	if (*change_counter != audipst->chg_counter) {
		DBG(vcp, "Change Counter Mismatch Audio Input State!");
		ret = BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
		goto respond;
	}

	if (audipst->mute == AICS_DISABLED) {
		DBG(vcp, "Mute state is Disabled!");
		ret = BT_ATT_AICS_ERROR_MUTE_DISABLED;
		goto respond;
	}

	audipst->mute = AICS_NOT_MUTED;
	/*Increment Change Counter*/
	audipst->chg_counter = -~audipst->chg_counter;
	gatt_db_attribute_notify(vdb->aics->aud_ip_state, (void *)audipst,
				sizeof(struct aud_ip_st),
				bt_vcp_get_att(vcp));
	ret = 0;

respond:
	return ret;
}

static uint8_t aics_mute(struct bt_aics *aics, struct bt_vcp *vcp,
						struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct aud_ip_st *audipst;
	uint8_t *change_counter;
	uint8_t	ret = 1;

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		ret = 0;
		goto respond;
	}

	audipst = vdb_get_audipst(vdb);
	if (!audipst) {
		DBG(vcp, "error: Audio Input State value is not available");
		ret = 0;
		goto respond;
	}
	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter) {
		ret = 0;
		goto respond;
	}

	if (*change_counter != audipst->chg_counter) {
		DBG(vcp, "Change Counter Mismatch Audio Input State!");
		ret = BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
		goto respond;
	}

	if (audipst->mute == AICS_DISABLED) {
		DBG(vcp, "Mute state is Disabled!");
		ret = BT_ATT_AICS_ERROR_MUTE_DISABLED;
		goto respond;
	}

	audipst->mute = AICS_MUTED;
	/*Increment Change Counter*/
	audipst->chg_counter = -~audipst->chg_counter;
	gatt_db_attribute_notify(vdb->aics->aud_ip_state, (void *)audipst,
				sizeof(struct aud_ip_st),
				bt_vcp_get_att(vcp));
	ret = 0;

respond:
	return ret;
}

static uint8_t aics_set_manual_gain_mode(struct bt_aics *aics,
				struct bt_vcp *vcp, struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct aud_ip_st *audipst;
	uint8_t *change_counter;
	uint8_t	ret = 1;

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		ret = 0;
		goto respond;
	}

	audipst = vdb_get_audipst(vdb);
	if (!audipst) {
		DBG(vcp, "error: Audio Input State value is not available");
		ret = 0;
		goto respond;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter) {
		ret = 0;
		goto respond;
	}

	if (*change_counter != audipst->chg_counter) {
		DBG(vcp, "Change Counter Mismatch Audio Input State!");
		ret = BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
		goto respond;
	}

	if (audipst->gain_mode == AICS_GAIN_MODE_AUTO_ONLY ||
		audipst->gain_mode == AICS_GAIN_MODE_MANUAL_ONLY) {
		DBG(vcp, "error!! gain mode is Automatic only or Manual only");
		ret = BT_ATT_AICS_ERROR_GAIN_MODE_CHANGE_NOT_ALLOWED;
		goto respond;
	}

	if (audipst->gain_mode == AICS_GAIN_MODE_AUTO) {
		audipst->gain_mode = AICS_GAIN_MODE_MANUAL;
		/*Increment Change Counter*/
		audipst->chg_counter = -~audipst->chg_counter;
		gatt_db_attribute_notify(vdb->aics->aud_ip_state,
					(void *)audipst,
					sizeof(struct aud_ip_st),
					bt_vcp_get_att(vcp));
		ret = 0;
	} else {
		DBG(vcp,
		"error!! Gain mode field value not Automatic");
		ret = BT_ATT_AICS_ERROR_GAIN_MODE_CHANGE_NOT_ALLOWED;
	}

respond:
	return ret;
}

static uint8_t aics_set_auto_gain_mode(struct bt_aics *aics, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct aud_ip_st *audipst;
	uint8_t *change_counter;
	uint8_t	ret = 1;

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		ret = 0;
		goto respond;
	}

	audipst = vdb_get_audipst(vdb);
	if (!audipst) {
		DBG(vcp, "error: Audio Input State value is not available");
		ret = 0;
		goto respond;
	}
	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter) {
		ret = 0;
		goto respond;
	}

	if (*change_counter != audipst->chg_counter) {
		DBG(vcp, "Change Counter Mismatch Audio Input State!");
		ret = BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
		goto respond;
	}

	if (audipst->gain_mode == AICS_GAIN_MODE_AUTO_ONLY ||
		audipst->gain_mode == AICS_GAIN_MODE_MANUAL_ONLY) {
		DBG(vcp, "error!! gain mode is Automatic only or Manual only");
		ret = BT_ATT_AICS_ERROR_GAIN_MODE_CHANGE_NOT_ALLOWED;
		goto respond;
	}

	if (audipst->gain_mode == AICS_GAIN_MODE_MANUAL) {
		audipst->gain_mode = AICS_GAIN_MODE_AUTO;
		/*Increment Change Counter*/
		audipst->chg_counter = -~audipst->chg_counter;
		gatt_db_attribute_notify(vdb->aics->aud_ip_state,
				(void *)audipst,
				sizeof(struct aud_ip_st), bt_vcp_get_att(vcp));
		ret = 0;
	} else {
		DBG(vcp, "error!! Gain mode field value is not Manual");
		ret = BT_ATT_AICS_ERROR_GAIN_MODE_CHANGE_NOT_ALLOWED;
	}

respond:
	return ret;
}

#define AICS_OP(_str, _op, _size, _func) \
	{ \
			.str = _str, \
			.op = _op, \
			.size = _size, \
			.func = _func, \
	}

struct aics_op_handler {
		const char *str;
		uint8_t op;
		size_t  size;
		uint8_t (*func)(struct bt_aics *aics, struct bt_vcp *vcp,
				struct iovec *iov);
} aics_handlers[] = {
		AICS_OP("Set Gain Setting", BT_AICS_SET_GAIN_SETTING,
				sizeof(struct bt_aics_set_gain_setting),
				aics_set_gain_setting),
		AICS_OP("Unmute", BT_AICS_UNMUTE,
				sizeof(uint8_t), aics_unmute),
		AICS_OP("Mute", BT_AICS_MUTE,
				sizeof(uint8_t), aics_mute),
		AICS_OP("Set Manual Gain Mode", BT_AICS_SET_MANUAL_GAIN_MODE,
				sizeof(uint8_t), aics_set_manual_gain_mode),
		AICS_OP("Set Automatic Gain Mode", BT_AICS_SET_AUTO_GAIN_MODE,
				sizeof(uint8_t), aics_set_auto_gain_mode),
	{}
};

static void aics_ip_cp_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_aics *aics = user_data;
	struct bt_vcp *vcp = vcp_get_session(att, aics->vdb->db);
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = len,
	};
	uint8_t	*aics_op;
	struct aics_op_handler *handler;
	uint8_t ret = BT_ATT_ERROR_REQUEST_NOT_SUPPORTED;

	DBG(vcp, "AICS Control Point Write");

	if (offset) {
		DBG(vcp, "invalid offset %d", offset);
		ret = BT_ATT_ERROR_INVALID_OFFSET;
		goto respond;
	}

	if (len < sizeof(*aics_op)) {
		DBG(vcp, "invalid len %ld < %ld sizeof(*param)", len,
							sizeof(*aics_op));
		ret = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto respond;
	}

	aics_op = iov_pull_mem(&iov, sizeof(*aics_op));

	for (handler = aics_handlers; handler && handler->str; handler++) {
		if (handler->op != *aics_op)
			continue;

		if (iov.iov_len < handler->size) {
			DBG(vcp, "invalid len %ld < %ld handler->size", len,
				handler->size);
			ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
			goto respond;
		}

		break;
	}

	if (handler && handler->str) {
		DBG(vcp, "%s", handler->str);

		ret = handler->func(aics, vcp, &iov);
	} else {
		DBG(vcp, "Unknown opcode 0x%02x", *aics_op);
		ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
	}

respond:
	gatt_db_attribute_write_result(attrib, id, ret);
}

static void aics_input_descr_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_aics *aics = user_data;
	struct iovec iov;

	iov.iov_base = aics->aud_input_descr;
	iov.iov_len = strlen(aics->aud_input_descr);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void aics_input_descr_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	/* TODO : AICS optional feature */
}

static struct bt_vcs *vcs_new(struct gatt_db *db, struct bt_vcp_db *vdb)
{
	struct bt_vcs *vcs;
	struct vol_state *vstate;
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	vcs = new0(struct bt_vcs, 1);

	vstate = new0(struct vol_state, 1);

	vcs->vstate = vstate;
	vcs->vol_flag = USERSET_VOLUME_SETTING;

	/* Populate DB with VCS attributes */
	bt_uuid16_create(&uuid, VCS_UUID);
	vcs->service = gatt_db_add_service(db, &uuid, true,
						VCS_TOTAL_NUM_HANDLES);
	gatt_db_service_add_included(vcs->service, vdb->vocs->service);
	gatt_db_service_set_active(vdb->vocs->service, true);
	gatt_db_service_add_included(vcs->service, vdb->aics->service);
	gatt_db_service_set_active(vdb->aics->service, true);

	bt_uuid16_create(&uuid, VOL_STATE_CHRC_UUID);
	vcs->vs = gatt_db_service_add_characteristic(vcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vcs_state_read, NULL,
					vcs);

	vcs->vs_ccc = gatt_db_service_add_ccc(vcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, VOL_CP_CHRC_UUID);
	vcs->vol_cp = gatt_db_service_add_characteristic(vcs->service,
					&uuid,
					BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_WRITE,
					NULL, vcs_cp_write,
					vcs);

	bt_uuid16_create(&uuid, VOL_FLAG_CHRC_UUID);
	vcs->vf = gatt_db_service_add_characteristic(vcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vcs_flag_read, NULL,
					vcs);

	vcs->vf_ccc = gatt_db_service_add_ccc(vcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);


	gatt_db_service_set_active(vcs->service, true);

	return vcs;
}

static struct bt_vocs *vocs_new(struct gatt_db *db)
{
	struct bt_vocs *vocs;
	struct vol_offset_state *vostate;
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	vocs = new0(struct bt_vocs, 1);

	vostate = new0(struct vol_offset_state, 1);

	vocs->vostate = vostate;
	vocs->vocs_audio_loc = BT_VCP_FRONT_LEFT;
	vocs->vocs_ao_dec = "Left Speaker";

	/* Populate DB with VOCS attributes */
	bt_uuid16_create(&uuid, VOL_OFFSET_CS_UUID);

	vocs->service = gatt_db_add_service(db, &uuid, false, 12);

	bt_uuid16_create(&uuid, VOCS_STATE_CHAR_UUID);
	vocs->vos = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vocs_state_read, NULL,
					vocs);

	vocs->vos_ccc = gatt_db_service_add_ccc(vocs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, VOCS_AUDIO_LOC_CHRC_UUID);
	vocs->voal = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vocs_voal_read, NULL,
					vocs);

	vocs->voal_ccc = gatt_db_service_add_ccc(vocs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, VOCS_CP_CHRC_UUID);
	vocs->vo_cp = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_WRITE,
					NULL, vocs_cp_write,
					vocs);

	bt_uuid16_create(&uuid, VOCS_AUDIO_OP_DESC_CHAR_UUID);
	vocs->voaodec = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vocs_voaodec_read, NULL,
					vocs);

	vocs->voaodec_ccc = gatt_db_service_add_ccc(vocs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	return vocs;
}

static struct bt_aics *aics_new(struct gatt_db *db)
{
	struct bt_aics *aics;
	struct aud_ip_st *aics_aud_ip_st;
	struct gain_setting_prop *aics_gain_settng_prop;
	char *ip_descr;
	char ip_descr_str[] = "Blueooth";
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	aics = new0(struct bt_aics, 1);

	aics_aud_ip_st = new0(struct aud_ip_st, 1);
	aics_gain_settng_prop = new0(struct gain_setting_prop, 1);
	ip_descr = malloc(256);
	memset(ip_descr, 0, 256);

	aics_aud_ip_st->mute = AICS_NOT_MUTED;
	aics_aud_ip_st->gain_mode = AICS_GAIN_MODE_MANUAL;
	aics_aud_ip_st->gain_setting = AICS_GAIN_SETTING_DEFAULT_VALUE;
	aics->aud_ipst = aics_aud_ip_st;
	aics_gain_settng_prop->gain_setting_units = AICS_GAIN_SETTING_UNITS;
	aics_gain_settng_prop->gain_setting_max = AICS_GAIN_SETTING_MAX_VALUE;
	aics_gain_settng_prop->gain_setting_min = AICS_GAIN_SETTING_MIN_VALUE;
	aics->gain_settingprop = aics_gain_settng_prop;
	aics->aud_input_type =	AICS_AUD_IP_TYPE_BLUETOOTH;
	aics->aud_input_status = AICS_AUD_IP_STATUS_ACTIVE;
	memcpy(ip_descr, ip_descr_str, strlen(ip_descr_str));
	aics->aud_input_descr = ip_descr;

	/* Populate DB with AICS attributes */
	bt_uuid16_create(&uuid, AUDIO_INPUT_CS_UUID);
	aics->service = gatt_db_add_service(db, &uuid, false,
					AICS_TOTAL_NUM_HANDLES);

	bt_uuid16_create(&uuid, AICS_INPUT_STATE_CHAR_UUID);
	aics->aud_ip_state = gatt_db_service_add_characteristic(aics->service,
				&uuid,
				BT_ATT_PERM_READ,
				BT_GATT_CHRC_PROP_READ |
				BT_GATT_CHRC_PROP_NOTIFY,
				aics_input_state_read,
				NULL,
				aics);
	aics->aud_ip_state_ccc = gatt_db_service_add_ccc(aics->service,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, AICS_GAIN_SETTING_PROP_CHAR_UUID);
	aics->gain_stting_prop = gatt_db_service_add_characteristic(
				aics->service,
				&uuid,
				BT_ATT_PERM_READ,
				BT_GATT_CHRC_PROP_READ,
				aics_gain_setting_prop_read, NULL,
				aics);

	bt_uuid16_create(&uuid, AICS_AUDIO_INPUT_TYPE_CHAR_UUID);
	aics->aud_ip_type = gatt_db_service_add_characteristic(aics->service,
				&uuid,
				BT_ATT_PERM_READ,
				BT_GATT_CHRC_PROP_READ,
				aics_audio_input_type_read, NULL,
				aics);

	bt_uuid16_create(&uuid, AICS_INPUT_STATUS_CHAR_UUID);
	aics->aud_ip_status = gatt_db_service_add_characteristic(aics->service,
				&uuid,
				BT_ATT_PERM_READ,
				BT_GATT_CHRC_PROP_READ |
				BT_GATT_CHRC_PROP_NOTIFY,
				aics_input_status_read, NULL,
				aics);
	aics->aud_ip_status_ccc = gatt_db_service_add_ccc(aics->service,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, AICS_AUDIO_INPUT_CP_CHRC_UUID);
	aics->aud_ip_cp = gatt_db_service_add_characteristic(aics->service,
				&uuid,
				BT_ATT_PERM_WRITE,
				BT_GATT_CHRC_PROP_WRITE,
				NULL, aics_ip_cp_write,
				aics);

	bt_uuid16_create(&uuid, AICS_INPUT_DESCR_CHAR_UUID);
	aics->aud_ip_dscrptn = gatt_db_service_add_characteristic(aics->service,
				&uuid,
				BT_ATT_PERM_READ |
				BT_ATT_PERM_WRITE,
				BT_GATT_CHRC_PROP_READ |
				BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP |
				BT_GATT_CHRC_PROP_NOTIFY,
				aics_input_descr_read,
				aics_input_descr_write,
				aics);
	aics->aud_ip_dscrptn_ccc = gatt_db_service_add_ccc(aics->service,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	return aics;
}

static struct bt_vcp_db *vcp_db_new(struct gatt_db *db)
{
	struct bt_vcp_db *vdb;

	if (!db)
		return NULL;

	vdb = new0(struct bt_vcp_db, 1);
	vdb->db = gatt_db_ref(db);

	if (!vcp_db)
		vcp_db = queue_new();

	vdb->vocs = vocs_new(db);
	vdb->vocs->vdb = vdb;

	vdb->aics = aics_new(db);
	vdb->aics->vdb = vdb;

	vdb->vcs = vcs_new(db, vdb);
	vdb->vcs->vdb = vdb;

	queue_push_tail(vcp_db, vdb);

	return vdb;
}

static struct bt_vcp_db *vcp_get_db(struct gatt_db *db)
{
	struct bt_vcp_db *vdb;

	vdb = queue_find(vcp_db, vcp_db_match, db);
	if (vdb)
		return vdb;

	return vcp_db_new(db);
}

void bt_vcp_add_db(struct gatt_db *db)
{
	vcp_db_new(db);
}

bool bt_vcp_set_debug(struct bt_vcp *vcp, bt_vcp_debug_func_t func,
			void *user_data, bt_vcp_destroy_func_t destroy)
{
	if (!vcp)
		return false;

	if (vcp->debug_destroy)
		vcp->debug_destroy(vcp->debug_data);

	vcp->debug_func = func;
	vcp->debug_destroy = destroy;
	vcp->debug_data = user_data;

	return true;
}

unsigned int bt_vcp_register(bt_vcp_func_t attached, bt_vcp_func_t detached,
							void *user_data)
{
	struct bt_vcp_cb *cb;
	static unsigned int id;

	if (!attached && !detached)
		return 0;

	if (!vcp_cbs)
		vcp_cbs = queue_new();

	cb = new0(struct bt_vcp_cb, 1);
	cb->id = ++id ? id : ++id;
	cb->attached = attached;
	cb->detached = detached;
	cb->user_data = user_data;

	queue_push_tail(vcp_cbs, cb);

	return cb->id;
}

static bool match_id(const void *data, const void *match_data)
{
	const struct bt_vcp_cb *cb = data;
	unsigned int id = PTR_TO_UINT(match_data);

	return (cb->id == id);
}

bool bt_vcp_unregister(unsigned int id)
{
	struct bt_vcp_cb *cb;

	cb = queue_remove_if(vcp_cbs, match_id, UINT_TO_PTR(id));
	if (!cb)
		return false;

	free(cb);

	return true;
}

struct bt_vcp *bt_vcp_new(struct gatt_db *ldb, struct gatt_db *rdb)
{
	struct bt_vcp *vcp;
	struct bt_vcp_db *vdb;

	if (!ldb)
		return NULL;

	vdb = vcp_get_db(ldb);
	if (!vdb)
		return NULL;

	vcp = new0(struct bt_vcp, 1);
	vcp->ldb = vdb;
	vcp->pending = queue_new();

	if (!rdb)
		goto done;

	vdb = new0(struct bt_vcp_db, 1);
	vdb->db = gatt_db_ref(rdb);

	vcp->rdb = vdb;

done:
	bt_vcp_ref(vcp);

	return vcp;
}

static void vcp_vstate_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct vol_state vstate;

	memcpy(&vstate, value, sizeof(struct vol_state));

	DBG(vcp, "Vol Settings 0x%x", vstate.vol_set);
	DBG(vcp, "Mute Status 0x%x", vstate.mute);
	DBG(vcp, "Vol Counter 0x%x", vstate.counter);
}

static void vcp_voffset_state_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct vol_offset_state vostate;

	memcpy(&vostate, value, sizeof(struct vol_offset_state));

	DBG(vcp, "Vol Offset 0x%x", vostate.vol_offset);
	DBG(vcp, "Vol Offset Counter 0x%x", vostate.counter);
}

static void vcp_audio_loc_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	uint32_t *vocs_audio_loc_n = malloc(sizeof(uint32_t));
	*vocs_audio_loc_n = 0;

	if (value != NULL)
		memcpy(vocs_audio_loc_n, value, sizeof(uint32_t));

	DBG(vcp, "VOCS Audio Location 0x%x", *vocs_audio_loc_n);

	free(vocs_audio_loc_n);
}


static void vcp_audio_descriptor_notify(struct bt_vcp *vcp,
					uint16_t value_handle,
					const uint8_t *value,
					uint16_t length,
					void *user_data)
{
	char vocs_audio_dec_n[256] = {'\0'};

	memcpy(vocs_audio_dec_n, value, length);

	DBG(vcp, "VOCS Audio Descriptor 0x%s", *vocs_audio_dec_n);
}

static void vcp_vflag_notify(struct bt_vcp *vcp, uint16_t value_handle,
			     const uint8_t *value, uint16_t length,
			     void *user_data)
{
	uint8_t vflag;

	memcpy(&vflag, value, sizeof(vflag));

	DBG(vcp, "Vol Flag 0x%x", vflag);
}

static void read_vol_flag(struct bt_vcp *vcp, bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	uint8_t *vol_flag;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Vol Flag: error 0x%02x", att_ecode);
		return;
	}

	vol_flag = iov_pull_mem(&iov, sizeof(*vol_flag));
	if (!vol_flag) {
		DBG(vcp, "Unable to get Vol Flag");
		return;
	}

	DBG(vcp, "Vol Flag:%x", *vol_flag);
}

static void read_vol_state(struct bt_vcp *vcp, bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct vol_state *vs;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Vol State: error 0x%02x", att_ecode);
		return;
	}

	vs = iov_pull_mem(&iov, sizeof(*vs));
	if (!vs) {
		DBG(vcp, "Unable to get Vol State");
		return;
	}

	DBG(vcp, "Vol Set:%x", vs->vol_set);
	DBG(vcp, "Vol Mute:%x", vs->mute);
	DBG(vcp, "Vol Counter:%x", vs->counter);
}

static void read_vol_offset_state(struct bt_vcp *vcp, bool success,
				  uint8_t att_ecode,
				  const uint8_t *value, uint16_t length,
				  void *user_data)
{
	struct vol_offset_state *vos;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Vol Offset State: error 0x%02x",
		    att_ecode);
		return;
	}

	vos = iov_pull_mem(&iov, sizeof(*vos));
	if (!vos) {
		DBG(vcp, "Unable to get Vol Offset State");
		return;
	}

	DBG(vcp, "Vol Offset: 0x%04x", le16_to_cpu(vos->vol_offset));
	DBG(vcp, "Vol Counter: 0x%02x", vos->counter);
}

static void read_vocs_audio_location(struct bt_vcp *vcp, bool success,
				     uint8_t att_ecode,
				     const uint8_t *value, uint16_t length,
				     void *user_data)
{
	uint32_t vocs_audio_loc;
	struct iovec iov;

	if (!value) {
		DBG(vcp, "Unable to get VOCS Audio Location");
		return;
	}

	if (!success) {
		DBG(vcp, "Unable to read VOCS Audio Location: error 0x%02x",
		    att_ecode);
		return;
	}

	iov.iov_base = (void *)value;
	iov.iov_len = length;

	if (!util_iov_pull_le32(&iov, &vocs_audio_loc)) {
		DBG(vcp, "Invalid size for VOCS Audio Location");
		return;
	}

	DBG(vcp, "VOCS Audio Loc: 0x%8x", vocs_audio_loc);
}


static void read_vocs_audio_descriptor(struct bt_vcp *vcp, bool success,
				       uint8_t att_ecode,
				       const uint8_t *value, uint16_t length,
				       void *user_data)
{
	char *vocs_ao_dec_r;

	if (!value) {
		DBG(vcp, "Unable to get VOCS Audio Location");
		return;
	}

	if (!success) {
		DBG(vcp, "Unable to read VOCS Audio Descriptor: error 0x%02x",
			att_ecode);
		return;
	}

	vocs_ao_dec_r = malloc(length+1);
	memset(vocs_ao_dec_r, 0, length+1);
	memcpy(vocs_ao_dec_r, value, length);

	if (!vocs_ao_dec_r) {
		DBG(vcp, "Unable to get VOCS Audio Descriptor");
		return;
	}

	DBG(vcp, "VOCS Audio Descriptor: %s", vocs_ao_dec_r);
	free(vocs_ao_dec_r);
	vocs_ao_dec_r = NULL;
}

static void vcp_pending_destroy(void *data)
{
	struct bt_vcp_pending *pending = data;
	struct bt_vcp *vcp = pending->vcp;

	if (queue_remove_if(vcp->pending, NULL, pending))
		free(pending);
}

static void vcp_pending_complete(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_vcp_pending *pending = user_data;

	if (pending->func)
		pending->func(pending->vcp, success, att_ecode, value, length,
						pending->user_data);
}

static void vcp_read_value(struct bt_vcp *vcp, uint16_t value_handle,
				vcp_func_t func, void *user_data)
{
	struct bt_vcp_pending *pending;

	pending = new0(struct bt_vcp_pending, 1);
	pending->vcp = vcp;
	pending->func = func;
	pending->user_data = user_data;

	pending->id = bt_gatt_client_read_value(vcp->client, value_handle,
						vcp_pending_complete, pending,
						vcp_pending_destroy);
	if (!pending->id) {
		DBG(vcp, "Unable to send Read request");
		free(pending);
		return;
	}

	queue_push_tail(vcp->pending, pending);
}

static void vcp_register(uint16_t att_ecode, void *user_data)
{
	struct bt_vcp_notify *notify = user_data;

	if (att_ecode)
		DBG(notify->vcp, "VCP register failed: 0x%04x", att_ecode);
}

static void vcp_notify(uint16_t value_handle, const uint8_t *value,
				uint16_t length, void *user_data)
{
	struct bt_vcp_notify *notify = user_data;

	if (notify->func)
		notify->func(notify->vcp, value_handle, value, length,
						notify->user_data);
}

static void vcp_notify_destroy(void *data)
{
	struct bt_vcp_notify *notify = data;
	struct bt_vcp *vcp = notify->vcp;

	if (queue_remove_if(vcp->notify, NULL, notify))
		free(notify);
}

static unsigned int vcp_register_notify(struct bt_vcp *vcp,
					uint16_t value_handle,
					vcp_notify_t func,
					void *user_data)
{
	struct bt_vcp_notify *notify;

	notify = new0(struct bt_vcp_notify, 1);
	notify->vcp = vcp;
	notify->func = func;
	notify->user_data = user_data;

	notify->id = bt_gatt_client_register_notify(vcp->client,
						value_handle, vcp_register,
						vcp_notify, notify,
						vcp_notify_destroy);
	if (!notify->id) {
		DBG(vcp, "Unable to register for notifications");
		free(notify);
		return 0;
	}

	queue_push_tail(vcp->notify, notify);

	return notify->id;
}

static void foreach_vcs_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_vcp *vcp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_vstate, uuid_cp, uuid_vflag;
	struct bt_vcs *vcs;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_vstate, VOL_STATE_CHRC_UUID);
	bt_uuid16_create(&uuid_cp, VOL_CP_CHRC_UUID);
	bt_uuid16_create(&uuid_vflag, VOL_FLAG_CHRC_UUID);

	if (!bt_uuid_cmp(&uuid, &uuid_vstate)) {
		DBG(vcp, "VCS Vol state found: handle 0x%04x", value_handle);

		vcs = vcp_get_vcs(vcp);
		if (!vcs || vcs->vs)
			return;

		vcs->vs = attr;

		vcp_read_value(vcp, value_handle, read_vol_state, vcp);

		vcp->vstate_id = vcp_register_notify(vcp, value_handle,
						     vcp_vstate_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_cp)) {
		DBG(vcp, "VCS Volume CP found: handle 0x%04x", value_handle);

		vcs = vcp_get_vcs(vcp);
		if (!vcs || vcs->vol_cp)
			return;

		vcs->vol_cp = attr;

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_vflag)) {
		DBG(vcp, "VCS Vol Flag found: handle 0x%04x", value_handle);

		vcs = vcp_get_vcs(vcp);
		if (!vcs || vcs->vf)
			return;

		vcs->vf = attr;

		vcp_read_value(vcp, value_handle, read_vol_flag, vcp);
		vcp->vflag_id = vcp_register_notify(vcp, value_handle,
						    vcp_vflag_notify, NULL);

	}
}

static void foreach_vocs_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_vcp *vcp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_vostate, uuid_audio_loc, uuid_vo_cp,
			uuid_audio_op_decs;
	struct bt_vocs *vocs;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_vostate, VOCS_STATE_CHAR_UUID);
	bt_uuid16_create(&uuid_audio_loc, VOCS_AUDIO_LOC_CHRC_UUID);
	bt_uuid16_create(&uuid_vo_cp, VOCS_CP_CHRC_UUID);
	bt_uuid16_create(&uuid_audio_op_decs, VOCS_AUDIO_OP_DESC_CHAR_UUID);

	if (!bt_uuid_cmp(&uuid, &uuid_vostate)) {
		DBG(vcp, "VOCS Vol state found: handle 0x%04x", value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->vos)
			return;

		vocs->vos = attr;

		vcp_read_value(vcp, value_handle, read_vol_offset_state, vcp);

		vcp->state_id = vcp_register_notify(vcp, value_handle,
					vcp_voffset_state_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_audio_loc)) {
		DBG(vcp, "VOCS Volume Audio Location found: handle 0x%04x",
			value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->voal)
			return;

		vocs->voal = attr;

		vcp_read_value(vcp, value_handle, read_vocs_audio_location,
				       vcp);

		vcp->audio_loc_id = vcp_register_notify(vcp, value_handle,
						vcp_audio_loc_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_vo_cp)) {
		DBG(vcp, "VOCS Volume CP found: handle 0x%04x", value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->vo_cp)
			return;

		vocs->vo_cp = attr;

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_audio_op_decs)) {
		DBG(vcp, "VOCS Vol Audio Descriptor found: handle 0x%04x",
			value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->voaodec)
			return;

		vocs->voaodec = attr;

		vcp_read_value(vcp, value_handle, read_vocs_audio_descriptor,
			       vcp);
		vcp->ao_dec_id = vcp_register_notify(vcp, value_handle,
					vcp_audio_descriptor_notify, NULL);

	}

}

static void read_aics_audio_ip_state(struct bt_vcp *vcp, bool success,
				  uint8_t att_ecode,
				  const uint8_t *value, uint16_t length,
				  void *user_data)
{
	struct aud_ip_st *ip_st;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Audio Input State: error 0x%02x",
			att_ecode);
		return;
	}

	ip_st = iov_pull_mem(&iov, sizeof(*ip_st));
	if (!ip_st) {
		DBG(vcp, "Unable to get Audio Input State");
		return;
	}

	DBG(vcp, "Audio Input State, Gain Setting:%d", ip_st->gain_setting);
	DBG(vcp, "Audio Input State, Mute:%x", ip_st->mute);
	DBG(vcp, "Audio Input State, Gain Mode:%x", ip_st->gain_mode);
	DBG(vcp, "Audio Input State, Change Counter:%x", ip_st->chg_counter);
}

static void aics_ip_state_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct aud_ip_st ip_st;

	memcpy(&ip_st, value, sizeof(struct aud_ip_st));

	DBG(vcp, "Audio Input State, Gain Setting:%d", ip_st.gain_setting);
	DBG(vcp, "Audio Input State, Mute:%x", ip_st.mute);
	DBG(vcp, "Audio Input State, Gain Mode:%x", ip_st.gain_mode);
	DBG(vcp, "Audio Input State, Change Counter:%x", ip_st.chg_counter);
}

static void read_aics_gain_setting_prop(struct bt_vcp *vcp, bool success,
					 uint8_t att_ecode,
					 const uint8_t *value, uint16_t length,
					 void *user_data)
{
	struct gain_setting_prop *aics_gain_setting_prop;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!value) {
		DBG(vcp, "Unable to get Gain Setting Properties Char");
		return;
	}

	if (!success) {
		DBG(vcp,
		"Unable to read Gain Setting Properties Char: 0x%02x",
		att_ecode);
		return;
	}

	aics_gain_setting_prop = iov_pull_mem(&iov,
				sizeof(*aics_gain_setting_prop));
	if (!aics_gain_setting_prop) {
		DBG(vcp, "Unable to get Gain Setting Properties Char");
		return;
	}

	DBG(vcp, "Gain Setting Properties, Units: %x",
				aics_gain_setting_prop->gain_setting_units);
	DBG(vcp, "Gain Setting Properties,  Min Value: %d",
				aics_gain_setting_prop->gain_setting_min);
	DBG(vcp, "Gain Setting Properties,  Max Value: %d",
				aics_gain_setting_prop->gain_setting_max);
}

static void read_aics_aud_ip_type(struct bt_vcp *vcp, bool success,
					 uint8_t att_ecode,
					 const uint8_t *value, uint16_t length,
					 void *user_data)
{
	uint8_t ip_type;

	if (!success) {
		DBG(vcp,
		"Unable to read Audio Input Type Char: error 0x%02x",
		att_ecode);
		return;
	}

	memcpy(&ip_type, value, length);

	DBG(vcp, "Audio Input Type : %x", ip_type);
}

static void read_aics_audio_ip_status(struct bt_vcp *vcp, bool success,
					 uint8_t att_ecode,
					 const uint8_t *value, uint16_t length,
					 void *user_data)
{
	uint8_t ip_status;

	if (!success) {
		DBG(vcp,
		"Unable to read Audio Input Status Char: 0x%02x", att_ecode);
		return;
	}

	memcpy(&ip_status, value, length);

	DBG(vcp, "Audio Input Status : %x", ip_status);
}

static void aics_ip_status_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value,
				uint16_t length,
				void *user_data)
{
	uint8_t	ip_status;

	memcpy(&ip_status, value, length);

	DBG(vcp, "Audio Input Status, %x", ip_status);
}

static void read_aics_audio_ip_description(struct bt_vcp *vcp, bool success,
					   uint8_t att_ecode,
					   const uint8_t *value,
					   uint16_t length,
					   void *user_data)
{
	char *ip_descrptn;

	if (!value) {
		DBG(vcp, "Unable to get Audio Input Description");
		return;
	}

	if (!success) {
		DBG(vcp,
			"Unable to read Audio Input Description Char: error 0x%02x",
			att_ecode);
		return;
	}

	ip_descrptn = malloc(length+1);
	memset(ip_descrptn, 0, length+1);
	memcpy(ip_descrptn, value, length);

	if (!ip_descrptn) {
		DBG(vcp, "Unable to get Audio Input Description");
		return;
	}

	DBG(vcp, "Audio Input Description: %s", ip_descrptn);
	free(ip_descrptn);
	ip_descrptn = NULL;
}

static void aics_audio_ip_desr_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	char *aud_ip_desr;

	aud_ip_desr = malloc(length+1);
	memset(aud_ip_desr, 0, length+1);
	memcpy(aud_ip_desr, value, length);

	DBG(vcp, "Audio Input Description Notify, %s", aud_ip_desr);
	free(aud_ip_desr);
	aud_ip_desr = NULL;
}

static void foreach_aics_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_vcp *vcp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_ipstate, uuid_gain_setting_prop, uuid_ip_type,
			uuid_ip_status, uuid_ip_cp, uuid_ip_decs;
	struct bt_aics *aics;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_ipstate, AICS_INPUT_STATE_CHAR_UUID);
	bt_uuid16_create(&uuid_gain_setting_prop,
					AICS_GAIN_SETTING_PROP_CHAR_UUID);
	bt_uuid16_create(&uuid_ip_type, AICS_AUDIO_INPUT_TYPE_CHAR_UUID);
	bt_uuid16_create(&uuid_ip_status, AICS_INPUT_STATUS_CHAR_UUID);
	bt_uuid16_create(&uuid_ip_cp, AICS_AUDIO_INPUT_CP_CHRC_UUID);
	bt_uuid16_create(&uuid_ip_decs, AICS_INPUT_DESCR_CHAR_UUID);


	if (!bt_uuid_cmp(&uuid, &uuid_ipstate)) {
		DBG(vcp,
			"AICS Audio Input State Char found: handle 0x%04x",
			value_handle);

		aics = vcp_get_aics(vcp);
		if (!aics || aics->aud_ip_state)
			return;

		aics->aud_ip_state = attr;

		vcp_read_value(vcp, value_handle,
					read_aics_audio_ip_state, vcp);

		vcp->aics_ip_state_id = vcp_register_notify(vcp, value_handle,
					aics_ip_state_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_gain_setting_prop)) {
		DBG(vcp,
			"AICS Gain Setting Properties Char found: handle 0x%04x",
			value_handle);

		aics = vcp_get_aics(vcp);
		if (!aics || aics->gain_stting_prop)
			return;

		aics->gain_stting_prop = attr;

		vcp_read_value(vcp, value_handle, read_aics_gain_setting_prop,
					   vcp);
		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_ip_type)) {
		DBG(vcp, "AICS Audio Input Type Char found: handle 0x%04x",
			value_handle);

		aics = vcp_get_aics(vcp);
		if (!aics || aics->gain_stting_prop)
			return;

		aics->aud_ip_type = attr;

		vcp_read_value(vcp, value_handle, read_aics_aud_ip_type,
					   vcp);
		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_ip_status)) {
		DBG(vcp,
			"AICS Audio Input Status Char found: handle 0x%04x",
			value_handle);

		aics = vcp_get_aics(vcp);
		if (!aics || aics->aud_ip_status)
			return;

		aics->aud_ip_status = attr;

		vcp_read_value(vcp, value_handle,
				read_aics_audio_ip_status, vcp);

		vcp->aics_ip_status_id = vcp_register_notify(vcp, value_handle,
					aics_ip_status_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_ip_cp)) {
		DBG(vcp, "AICS Input CP found: handle 0x%04x", value_handle);

		aics = vcp_get_aics(vcp);
		if (!aics || aics->aud_ip_cp)
			return;

		aics->aud_ip_cp = attr;

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_ip_decs)) {
		DBG(vcp,
			"AICS Audio Input Description Char found: handle 0x%04x",
			value_handle);

		aics = vcp_get_aics(vcp);
		if (!aics || aics->aud_ip_dscrptn)
			return;

		aics->aud_ip_dscrptn = attr;

		vcp_read_value(vcp, value_handle,
				read_aics_audio_ip_description, vcp);
		vcp->aics_ip_descr_id = vcp_register_notify(vcp, value_handle,
					aics_audio_ip_desr_notify, NULL);
	}
}

static void foreach_vcs_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_vcp *vcp = user_data;
	struct bt_vcs *vcs = vcp_get_vcs(vcp);

	vcs->service = attr;

	gatt_db_service_set_claimed(attr, true);

	gatt_db_service_foreach_char(attr, foreach_vcs_char, vcp);
}

static void foreach_vocs_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_vcp *vcp = user_data;
	struct bt_vocs *vocs = vcp_get_vocs(vcp);

	vocs->service = attr;

	gatt_db_service_set_claimed(attr, true);

	gatt_db_service_foreach_char(attr, foreach_vocs_char, vcp);
}

static void foreach_aics_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_vcp *vcp = user_data;
	struct bt_aics *aics = vcp_get_aics(vcp);

	aics->service = attr;

	gatt_db_service_set_claimed(attr, true);

	gatt_db_service_foreach_char(attr, foreach_aics_char, vcp);
}

bool bt_vcp_attach(struct bt_vcp *vcp, struct bt_gatt_client *client)
{
	bt_uuid_t uuid;

	if (!sessions)
		sessions = queue_new();

	queue_push_tail(sessions, vcp);

	if (!client)
		return true;

	if (vcp->client)
		return false;

	vcp->client = bt_gatt_client_clone(client);
	if (!vcp->client)
		return false;

	bt_uuid16_create(&uuid, VCS_UUID);
	gatt_db_foreach_service(vcp->rdb->db, &uuid, foreach_vcs_service, vcp);

	bt_uuid16_create(&uuid, VOL_OFFSET_CS_UUID);
	gatt_db_foreach_service(vcp->rdb->db, &uuid, foreach_vocs_service, vcp);

	bt_uuid16_create(&uuid, AUDIO_INPUT_CS_UUID);
	gatt_db_foreach_service(vcp->rdb->db, &uuid, foreach_aics_service, vcp);

	return true;
}

