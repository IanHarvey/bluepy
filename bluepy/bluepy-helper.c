/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
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
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>


#include "lib/bluetooth.h"
#include "lib/bluetooth/sdp.h"
#include "lib/uuid.h"
#include "lib/mgmt.h"
#include "src/shared/mgmt.h"

#include <btio/btio.h>
#include "att.h"
#include "gattrib.h"
#include "gatt.h"
#include "gatttool.h"

#define IO_CAPABILITY_NOINPUTNOOUTPUT   0x03

#ifdef BLUEPY_DEBUG
#define DBG(fmt, ...) do {printf("# %s() :" fmt "\n", __FUNCTION__, ##__VA_ARGS__); fflush(stdout); \
	} while(0)
#else
#ifdef BLUEPY_DEBUG_FILE_LOG
static FILE * fp = NULL;

static void try_open(void) {
	if (!fp) {
		fp = fopen ("bluepy-helper.log", "w");
	}
}
#define DBG(fmt, ...) do {try_open();if (fp) {fprintf(fp, "%s() :" fmt "\n", __FUNCTION__, ##__VA_ARGS__); fflush(fp);} \
	} while(0)

#else
#define DBG(fmt, ...)
#endif
#endif

static GIOChannel *iochannel = NULL;
static GAttrib *attrib = NULL;
static GMainLoop *event_loop;

static gchar *opt_src = NULL;
static gchar *opt_dst = NULL;
static gchar *opt_dst_type = NULL;
static gchar *opt_sec_level = NULL;
static const int opt_psm = 0;
static int opt_mtu = 0;
static int start;
static int end;

static struct mgmt *mgmt_master = NULL;

struct characteristic_data {
	uint16_t orig_start;
	uint16_t start;
	uint16_t end;
	bt_uuid_t uuid;
};

static void cmd_help(int argcp, char **argvp);

static enum state {
	STATE_DISCONNECTED=0,
	STATE_CONNECTING=1,
	STATE_CONNECTED=2,
	STATE_SCANNING=3,
} conn_state;


static const char
	*tag_RESPONSE	= "rsp",
	*tag_ERRCODE	= "code",
	*tag_HANDLE		= "hnd",
	*tag_UUID		= "uuid",
	*tag_DATA		= "d",
	*tag_CONNSTATE	= "state",
	*tag_SEC_LEVEL	= "sec",
	*tag_MTU		= "mtu",
	*tag_DEVICE		= "dst",
	*tag_RANGE_START = "hstart",
	*tag_RANGE_END	= "hend",
	*tag_PROPERTIES	= "props",
	*tag_VALUE_HANDLE = "vhnd",
	*tag_ADDR		= "addr",
	*tag_TYPE		= "type",
	*tag_RSSI		= "rssi",
	*tag_FLAG		= "flag";

static const char
	*rsp_ERROR		= "err",
	*rsp_STATUS		= "stat",
	*rsp_NOTIFY		= "ntfy",
	*rsp_IND		= "ind",
	*rsp_DISCOVERY	= "find",
	*rsp_DESCRIPTORS = "desc",
	*rsp_READ		= "rd",
	*rsp_WRITE		= "wr",
	*rsp_MGMT		= "mgmt",
	*rsp_SCAN		= "scan";

static const char
	*err_CONN_FAIL	= "connfail",
	*err_COMM_ERR	= "comerr",
	*err_PROTO_ERR	= "protoerr",
	*err_NOT_FOUND	= "notfound",
	*err_BAD_CMD	= "badcmd",
	*err_BAD_PARAM	= "badparam",
	*err_BAD_STATE	= "badstate",
	*err_BUSY	= "busy",
	*err_SUCCESS	= "success";

static const char
	*st_DISCONNECTED = "disc",
	*st_CONNECTING	= "tryconn",
	*st_CONNECTED	= "conn",
	*st_SCANNING    = "scan";

static void resp_begin(const char *rsptype)
{
	printf("%s=$%s", tag_RESPONSE, rsptype);
}

static void send_sym(const char *tag, const char *val)
{
	printf(" %s=$%s", tag, val);
}

static void send_uint(const char *tag, unsigned int val)
{
	printf(" %s=h%X", tag, val);
}

static void send_str(const char *tag, const char *val)
{
	//!!FIXME
	printf(" %s='%s", tag, val);
}

static void send_data(const unsigned char *val, size_t len)
{
	printf(" %s=b", tag_DATA);
	while ( len-- > 0 )
		printf("%02X", *val++);
}

static void send_addr(const struct mgmt_addr_info *addr)
{
	const uint8_t *val = addr->bdaddr.b;
	printf(" %s=b", tag_ADDR);
	int len = 6;
	while ( len-- > 0 )
		printf("%02X", *val++);

	send_uint(tag_TYPE, addr->type);
}

static void resp_end()
{
	printf("\n");
	fflush(stdout);
}

static void resp_error(const char *errcode)
{
	resp_begin(rsp_ERROR);
	send_sym(tag_ERRCODE, errcode);
	resp_end();
}

static void resp_mgmt(const char *errcode)
{
	resp_begin(rsp_MGMT);
	send_sym(tag_ERRCODE, errcode);
	resp_end();
}

static void cmd_status(int argcp, char **argvp)
{
	resp_begin(rsp_STATUS);
	switch(conn_state)
	{
	case STATE_CONNECTING:
		send_sym(tag_CONNSTATE, st_CONNECTING);
		send_str(tag_DEVICE, opt_dst);
		break;

	case STATE_CONNECTED:
		send_sym(tag_CONNSTATE, st_CONNECTED);
		send_str(tag_DEVICE, opt_dst);
		break;

	case STATE_SCANNING:
		send_sym(tag_CONNSTATE, st_SCANNING);
		send_str(tag_DEVICE, opt_dst);
		break;

	default:
		send_sym(tag_CONNSTATE, st_DISCONNECTED);
		break;
	}

	send_uint(tag_MTU, opt_mtu);
	send_str(tag_SEC_LEVEL, opt_sec_level);
	resp_end();
}

static void set_state(enum state st)
{
	conn_state = st;
	cmd_status(0, NULL);
}

static void events_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t evt;
	uint16_t handle, olen;
	size_t plen;

	evt = pdu[0];

	if ( evt != ATT_OP_HANDLE_NOTIFY && evt != ATT_OP_HANDLE_IND )
	{
		printf("#Invalid opcode %02X in event handler??\n", evt);
		return;
	}

	assert( len >= 3 );
	handle = bt_get_le16(&pdu[1]);

	resp_begin( evt==ATT_OP_HANDLE_NOTIFY ? rsp_NOTIFY : rsp_IND );
	send_uint( tag_HANDLE, handle );
	send_data( pdu+3, len-3 );
	resp_end();

	if (evt == ATT_OP_HANDLE_NOTIFY)
		return;

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_confirmation(opdu, plen);

	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_find_info_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	starting_handle = bt_get_le16(&pdu[1]);
	/* ending_handle = bt_get_le16(&pdu[3]); */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_find_by_type_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len >= 7 );
	opcode = pdu[0];
	starting_handle = bt_get_le16(&pdu[1]);
	/* ending_handle = bt_get_le16(&pdu[3]); */
	/* att_type = bt_get_le16(&pdu[5]); */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_by_type_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len == 7 || len == 21 );
	opcode = pdu[0];
	starting_handle = bt_get_le16(&pdu[1]);
	/* ending_handle = bt_get_le16(&pdu[3]); */
	if (len == 7) {
		/* att_type = bt_get_le16(&pdu[5]); */
	}

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len == 3 );
	opcode = pdu[0];
	handle = bt_get_le16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_blob_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	handle = bt_get_le16(&pdu[1]);
	/* offset = bt_get_le16(&pdu[3]); */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_multi_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle1, olen;
	size_t plen;

	assert( len >= 5 );
	opcode = pdu[0];
	handle1 = bt_get_le16(&pdu[1]);
	/* handle2 = bt_get_le16(&pdu[3]); */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle1, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_by_group_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len >= 7 );
	opcode = pdu[0];
	starting_handle = bt_get_le16(&pdu[1]);
	/* ending_handle = bt_get_le16(&pdu[3]); */
	/* att_group_type = bt_get_le16(&pdu[5]); */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len >= 3 );
	opcode = pdu[0];
	handle = bt_get_le16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_write_cmd(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	assert( len >= 3 );
	/* opcode = pdu[0]; */
	/* handle = bt_get_le16(&pdu[1]); */
}

static void gatts_signed_write_cmd(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	assert( len >= 15 );
	/* opcode = pdu[0]; */
	/* handle = bt_get_le16(&pdu[1]); */
}

static void gatts_prep_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode, handle;
	uint16_t olen;
	size_t plen;

	assert( len >= 5 );
	opcode = pdu[0];
	handle = bt_get_le16(&pdu[1]);
	/* offset = bt_get_le16(&pdu[3]); */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_exec_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	/* flags = pdu[1]; */

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, 0, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
	uint16_t mtu;
	uint16_t cid;
	GError *gerr = NULL;

	DBG("io = %p, err = %p", io, err);
	if (err) {
		DBG("err = %s", err->message);
		set_state(STATE_DISCONNECTED);
		resp_error(err_CONN_FAIL);
		printf("# Connect error: %s\n", err->message);
		return;
	}

	bt_io_get(io, &gerr, BT_IO_OPT_IMTU, &mtu,
				BT_IO_OPT_CID, &cid, BT_IO_OPT_INVALID);

	if (gerr) {
		printf("# Can't detect MTU, using default");
		g_error_free(gerr);
		mtu = ATT_DEFAULT_LE_MTU;
	}
	else if (cid == ATT_CID)
		mtu = ATT_DEFAULT_LE_MTU;

	attrib = g_attrib_new(iochannel, mtu);

	g_attrib_register(attrib, ATT_OP_HANDLE_NOTIFY, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_HANDLE_IND, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_FIND_INFO_REQ, GATTRIB_ALL_HANDLES,
						gatts_find_info_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_FIND_BY_TYPE_REQ, GATTRIB_ALL_HANDLES,
						gatts_find_by_type_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BY_TYPE_REQ, GATTRIB_ALL_HANDLES,
						gatts_read_by_type_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_REQ, GATTRIB_ALL_HANDLES,
						gatts_read_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BLOB_REQ, GATTRIB_ALL_HANDLES,
						gatts_read_blob_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_MULTI_REQ, GATTRIB_ALL_HANDLES,
						gatts_read_multi_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BY_GROUP_REQ, GATTRIB_ALL_HANDLES,
						gatts_read_by_group_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_WRITE_REQ, GATTRIB_ALL_HANDLES,
						gatts_write_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_WRITE_CMD, GATTRIB_ALL_HANDLES,
						gatts_write_cmd, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_SIGNED_WRITE_CMD, GATTRIB_ALL_HANDLES,
						gatts_signed_write_cmd, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_PREP_WRITE_REQ, GATTRIB_ALL_HANDLES,
						gatts_prep_write_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_EXEC_WRITE_REQ, GATTRIB_ALL_HANDLES,
						gatts_exec_write_req, attrib, NULL);

	set_state(STATE_CONNECTED);
}

static void disconnect_io()
{
	if (conn_state == STATE_DISCONNECTED)
		return;

	g_attrib_unref(attrib);
	attrib = NULL;
	opt_mtu = 0;

	g_io_channel_shutdown(iochannel, FALSE, NULL);
	g_io_channel_unref(iochannel);
	iochannel = NULL;

	set_state(STATE_DISCONNECTED);
}

static void primary_all_cb(uint8_t status, GSList *services, void *user_data)
{
	GSList *l;

	if (status) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	resp_begin(rsp_DISCOVERY);
	for (l = services; l; l = l->next) {
		struct gatt_primary *prim = l->data;
		send_uint(tag_RANGE_START, prim->range.start);
		send_uint(tag_RANGE_END, prim->range.end);
		send_str(tag_UUID, prim->uuid);
	}
	resp_end();

}

static void primary_by_uuid_cb(uint8_t status, GSList *ranges, void *user_data)
{
	GSList *l;

	if (status) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	resp_begin(rsp_DISCOVERY);
	for (l = ranges; l; l = l->next) {
		struct att_range *range = l->data;
		send_uint(tag_RANGE_START, range->start);
		send_uint(tag_RANGE_END, range->end);
	}
	resp_end();
}

static void included_cb(uint8_t status, GSList *includes, void *user_data)
{
	GSList *l;

	if (status) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	resp_begin(rsp_DISCOVERY);
	for (l = includes; l; l = l->next) {
		struct gatt_included *incl = l->data;
		send_uint(tag_HANDLE, incl->handle);
		send_uint(tag_RANGE_START, incl->range.start);
		send_uint(tag_RANGE_END,   incl->range.end);
		send_str(tag_UUID, incl->uuid);
	}
	resp_end();
}

static void char_cb(uint8_t status, GSList *characteristics, void *user_data)
{
	GSList *l;

	if (status) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	resp_begin(rsp_DISCOVERY);
	for (l = characteristics; l; l = l->next) {
		struct gatt_char *chars = l->data;
		send_uint(tag_HANDLE, chars->handle);
		send_uint(tag_PROPERTIES, chars->properties);
		send_uint(tag_VALUE_HANDLE, chars->value_handle);
		send_str(tag_UUID, chars->uuid);
	}
	resp_end();
}

static void char_desc_cb(uint8_t status, GSList *descriptors, void *user_data)
{
	GSList *l;

	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	resp_begin(rsp_DESCRIPTORS);
	for (l = descriptors; l != NULL; l = l->next) {
		struct gatt_desc *desc = (struct gatt_desc *)l->data;
		send_uint(tag_HANDLE, desc->handle);
		send_str (tag_UUID, desc->uuid);
	}
	resp_end();
}

static void char_read_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	uint8_t value[plen];
	ssize_t vlen;

	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	vlen = dec_read_resp(pdu, plen, value, sizeof(value));
	if (vlen < 0) {
		resp_error(err_COMM_ERR);
		return;
	}

	resp_begin(rsp_READ);
	send_data(value, vlen);
	resp_end();
}

static void char_read_by_uuid_cb(guint8 status, const guint8 *pdu,
					guint16 plen, gpointer user_data)
{
	struct characteristic_data *char_data = user_data;
	struct att_data_list *list;
	int i;

	if (status == ATT_ECODE_ATTR_NOT_FOUND &&
				char_data->start != char_data->orig_start)
	{
		printf("# TODO case in char_read_by_uuid_cb\n");
		goto done;
	}

	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		goto done;
	}

	list = dec_read_by_type_resp(pdu, plen);

	resp_begin(rsp_READ);
	if (list == NULL)
		goto nolist;

	for (i = 0; i < list->num; i++) {
		uint8_t *value = list->data[i];

		char_data->start = bt_get_le16(value) + 1;

		send_uint(tag_HANDLE, bt_get_le16(value));
		send_data(value+2, list->len-2); // All the same length??
	}

	att_data_list_free(list);
nolist:
	resp_end();

done:
	g_free(char_data);
}

static void cmd_exit(int argcp, char **argvp)
{
	g_main_loop_quit(event_loop);
}

static gboolean channel_watcher(GIOChannel *chan, GIOCondition cond,
				gpointer user_data)
{
	DBG("chan = %p", chan);

	// in case of quick disconnection/reconnection, do not mix them
	if (chan == iochannel)
		disconnect_io();

	return FALSE;
}

static void cmd_connect(int argcp, char **argvp)
{
	GError *gerr = NULL;
	if (conn_state != STATE_DISCONNECTED)
		return;

	if (argcp > 1) {
		g_free(opt_dst);
		opt_dst = g_strdup(argvp[1]);

		g_free(opt_dst_type);
		if (argcp > 2)
			opt_dst_type = g_strdup(argvp[2]);
		else
			opt_dst_type = g_strdup("public");
	}

	if (opt_dst == NULL) {
		resp_error(err_BAD_PARAM);
		return;
	}

	set_state(STATE_CONNECTING);
	iochannel = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
						opt_psm, opt_mtu, connect_cb, &gerr);

	DBG("gatt_connect returned %p", iochannel);
	if (iochannel == NULL)
	{
		set_state(STATE_DISCONNECTED);
		g_error_free(gerr);
	}
	else
		g_io_add_watch(iochannel, G_IO_HUP, channel_watcher, NULL);
}

static void cmd_disconnect(int argcp, char **argvp)
{
	disconnect_io();
}

static void cmd_primary(int argcp, char **argvp)
{
	bt_uuid_t uuid;

	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp == 1) {
		gatt_discover_primary(attrib, NULL, primary_all_cb, NULL);
		return;
	}

	if (bt_string_to_uuid(&uuid, argvp[1]) < 0) {
		resp_error(err_BAD_PARAM);
		return;
	}

	gatt_discover_primary(attrib, &uuid, primary_by_uuid_cb, NULL);
}

static int strtohandle(const char *src)
{
	char *e;
	int dst;

	errno = 0;
	dst = strtoll(src, &e, 16);
	if (errno != 0 || *e != '\0')
		return -EINVAL;

	return dst;
}

static void cmd_included(int argcp, char **argvp)
{
	int start = 0x0001;
	int end = 0xffff;

	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp > 1) {
		start = strtohandle(argvp[1]);
		if (start < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
		end = start;
	}

	if (argcp > 2) {
		end = strtohandle(argvp[2]);
		if (end < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	}

	gatt_find_included(attrib, start, end, included_cb, NULL);
}

static void cmd_char(int argcp, char **argvp)
{
	int start = 0x0001;
	int end = 0xffff;

	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp > 1) {
		start = strtohandle(argvp[1]);
		if (start < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	}

	if (argcp > 2) {
		end = strtohandle(argvp[2]);
		if (end < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	}

	if (argcp > 3) {
		bt_uuid_t uuid;

		if (bt_string_to_uuid(&uuid, argvp[3]) < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}

		gatt_discover_char(attrib, start, end, &uuid, char_cb, NULL);
		return;
	}

	gatt_discover_char(attrib, start, end, NULL, char_cb, NULL);
}

static void cmd_char_desc(int argcp, char **argvp)
{
	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp > 1) {
		start = strtohandle(argvp[1]);
		if (start < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	} else
		start = 0x0001;

	if (argcp > 2) {
		end = strtohandle(argvp[2]);
		if (end < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	} else
		end = 0xffff;

	gatt_discover_desc(attrib, start, end, NULL, char_desc_cb, NULL);
}

static void cmd_read_hnd(int argcp, char **argvp)
{
	int handle;

	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp < 2) {
		resp_error(err_BAD_PARAM);
		return;
	}

	handle = strtohandle(argvp[1]);
	if (handle < 0) {
		resp_error(err_BAD_PARAM);
		return;
	}

	gatt_read_char(attrib, handle, char_read_cb, attrib);
}

static void cmd_read_uuid(int argcp, char **argvp)
{
	struct characteristic_data *char_data;
	int start = 0x0001;
	int end = 0xffff;
	bt_uuid_t uuid;

	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp < 2 ||
		bt_string_to_uuid(&uuid, argvp[1]) < 0) {
		resp_error(err_BAD_PARAM);
		return;
	}

	if (argcp > 2) {
		start = strtohandle(argvp[2]);
		if (start < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	}

	if (argcp > 3) {
		end = strtohandle(argvp[3]);
		if (end < 0) {
			resp_error(err_BAD_PARAM);
			return;
		}
	}

	char_data = g_new(struct characteristic_data, 1);
	char_data->orig_start = start;
	char_data->start = start;
	char_data->end = end;
	char_data->uuid = uuid;

	gatt_read_char_by_uuid(attrib, start, end, &char_data->uuid,
					char_read_by_uuid_cb, char_data);
}

static void char_write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	if (!dec_write_resp(pdu, plen) && !dec_exec_write_resp(pdu, plen)) {
		resp_error(err_PROTO_ERR);
		return;
	}

	resp_begin(rsp_WRITE);
	resp_end();
}


static void cmd_char_write_common(int argcp, char **argvp, int with_response)
{
	uint8_t *value;
	size_t plen;
	int handle;

	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	if (argcp < 3) {
		resp_error(err_BAD_PARAM);
		return;
	}

	handle = strtohandle(argvp[1]);
	if (handle <= 0) {
		resp_error(err_BAD_PARAM);
		return;
	}

	plen = gatt_attr_data_from_string(argvp[2], &value);
	if (plen == 0) {
		resp_error(err_BAD_PARAM);
		return;
	}

	if (with_response)
		gatt_write_char(attrib, handle, value, plen,
					char_write_req_cb, NULL);
	else
	{
		gatt_write_cmd(attrib, handle, value, plen, NULL, NULL);
		resp_begin(rsp_WRITE);
		resp_end();
	}

	g_free(value);
}

static void cmd_char_write(int argcp, char **argvp)
{
	cmd_char_write_common(argcp, argvp, 0);
}

static void cmd_char_write_rsp(int argcp, char **argvp)
{
	cmd_char_write_common(argcp, argvp, 1);
}

static void cmd_sec_level(int argcp, char **argvp)
{
	GError *gerr = NULL;
	BtIOSecLevel sec_level;

	if (argcp < 2) {
		resp_error(err_BAD_PARAM);
		return;
	}

	if (strcasecmp(argvp[1], "medium") == 0)
		sec_level = BT_IO_SEC_MEDIUM;
	else if (strcasecmp(argvp[1], "high") == 0)
		sec_level = BT_IO_SEC_HIGH;
	else if (strcasecmp(argvp[1], "low") == 0)
		sec_level = BT_IO_SEC_LOW;
	else {
		resp_error(err_BAD_PARAM);
		return;
	}

	g_free(opt_sec_level);
	opt_sec_level = g_strdup(argvp[1]);

	if (conn_state != STATE_CONNECTED)
		return;

	assert(!opt_psm);

	bt_io_set(iochannel, &gerr,
			BT_IO_OPT_SEC_LEVEL, sec_level,
			BT_IO_OPT_INVALID);
	if (gerr) {
		printf("# Error: %s\n", gerr->message);
		resp_error(err_COMM_ERR);
		g_error_free(gerr);
	}
	else {
		/* Tell bluepy the security level
		 * has been changed successfuly */
		cmd_status(0, NULL);
	}
}

static void exchange_mtu_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	uint16_t mtu;

	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	if (!dec_mtu_resp(pdu, plen, &mtu)) {
		resp_error(err_PROTO_ERR);
		return;
	}

	mtu = MIN(mtu, opt_mtu);
	/* Set new value for MTU in client */
	if (g_attrib_set_mtu(attrib, mtu))
	{
		opt_mtu = mtu;
		cmd_status(0, NULL);
	}
	else
	{
		printf("# Error exchanging MTU\n");
		resp_error(err_COMM_ERR);
	}
}

static void cmd_mtu(int argcp, char **argvp)
{
	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	assert(!opt_psm);

	if (argcp < 2) {
		resp_error(err_BAD_PARAM);
		return;
	}

	if (opt_mtu) {
		resp_error(err_BAD_STATE);
		/* Can only set once per connection */
		return;
	}

	errno = 0;
	opt_mtu = strtoll(argvp[1], NULL, 16);
	if (errno != 0 || opt_mtu < ATT_DEFAULT_LE_MTU) {
		resp_error(err_BAD_PARAM);
		return;
	}

	gatt_exchange_mtu(attrib, opt_mtu, exchange_mtu_cb, NULL);
}

static void set_mode_complete(uint8_t status, uint16_t length,
								const void *param, void *user_data)
{
	if (status != MGMT_STATUS_SUCCESS) {
		DBG("status returned error : %s (0x%02x)",
			mgmt_errstr(status), status);
		resp_mgmt(err_PROTO_ERR);
		return;
	}

	resp_mgmt(err_SUCCESS);
}

static bool set_mode(uint16_t opcode, char *p_mode)
{
	struct mgmt_mode cp;
	uint8_t val;

	if (!memcmp(p_mode, "on", 2))
		val = 1;
	else if (!memcmp(p_mode, "off", 3))
		val = 0;
	else
		return false;

	memset(&cp, 0, sizeof(cp));
	cp.val = val;

	// at this time only index 0 is supported
	if (mgmt_send(mgmt_master, opcode,
			0, sizeof(cp), &cp,
			set_mode_complete, NULL, NULL) == 0) {
		resp_mgmt(err_SUCCESS);
	}
	return true;
}


static void cmd_le(int argcp, char **argvp)
{
	if (argcp < 2) {
		resp_mgmt(err_BAD_PARAM);
		return;
	}

	if (!set_mode(MGMT_OP_SET_LE, argvp[1])) {
		resp_mgmt(err_BAD_PARAM);
	}
}

static void cmd_pairable(int argcp, char **argvp)
{
	if (argcp < 2) {
		resp_mgmt(err_BAD_PARAM);
		return;
	}

	if (!set_mode(MGMT_OP_SET_BONDABLE, argvp[1])) {
		resp_mgmt(err_BAD_PARAM);
	}
}

static void pair_device_complete(uint8_t status, uint16_t length,
									const void *param, void *user_data)
{
	if (status != MGMT_STATUS_SUCCESS) {
		DBG("status returned error : %s (0x%02x)",
			mgmt_errstr(status), status);
		resp_mgmt(err_PROTO_ERR);
		return;
	}

	resp_mgmt(err_SUCCESS);
}

static void cmd_pair(int argcp, char **argvp)
{
	struct mgmt_cp_pair_device cp;
	bdaddr_t bdaddr;
	uint8_t io_cap = IO_CAPABILITY_NOINPUTNOOUTPUT;
	uint8_t addr_type = BDADDR_LE_RANDOM;

	if (conn_state != STATE_CONNECTED) {
		resp_mgmt(err_BAD_STATE);
		return;
	}

	if (str2ba(opt_dst, &bdaddr)) {
		resp_mgmt(err_NOT_FOUND);
		return;
	}

	if (!memcmp(opt_dst_type, "public", 6)) {
		addr_type = BDADDR_LE_PUBLIC;
	}

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, &bdaddr);
	cp.addr.type = addr_type;
	cp.io_cap = io_cap;

	if (mgmt_send(mgmt_master, MGMT_OP_PAIR_DEVICE,
				MGMT_INDEX_NONE, sizeof(cp), &cp,
				pair_device_complete, NULL,
				NULL) == 0) {
		DBG("mgmt_send(MGMT_OP_PAIR_DEVICE) failed for %s for hci%u", opt_dst, MGMT_INDEX_NONE);
		resp_mgmt(err_PROTO_ERR);
		return;
	}
}

static void unpair_device_complete(uint8_t status, uint16_t length,
									const void *param, void *user_data)
{
	if (status != MGMT_STATUS_SUCCESS) {
		DBG("status returned error : %s (0x%02x)",
			mgmt_errstr(status), status);
		resp_mgmt(err_PROTO_ERR);
		return;
	}

	resp_mgmt(err_SUCCESS);
}

static void cmd_unpair(int argcp, char **argvp)
{
	struct mgmt_cp_unpair_device cp;
	bdaddr_t bdaddr;
	uint8_t addr_type = BDADDR_LE_RANDOM;

	if (str2ba(opt_dst, &bdaddr)) {
		DBG("str2ba failed");
		resp_mgmt(err_NOT_FOUND);
		return;
	}

	if (!memcmp(opt_dst_type, "public", 6)) {
		addr_type = BDADDR_LE_PUBLIC;
	}

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, &bdaddr);
	cp.addr.type = addr_type;
	cp.disconnect = 1;

	if (mgmt_send(mgmt_master, MGMT_OP_UNPAIR_DEVICE,
				0, sizeof(cp), &cp,
				unpair_device_complete, NULL,
				NULL) == 0) {
		DBG("mgmt_send(MGMT_OP_UNPAIR_DEVICE) failed for %s for hci%u", opt_dst, MGMT_INDEX_NONE);
		resp_mgmt(err_PROTO_ERR);
		return;
	}
}

static void scan_cb(uint8_t status, uint16_t length, const void *param, void *user_data)
{
	if (status != MGMT_STATUS_SUCCESS) {
		DBG("Scan error: %s (0x%02x)", mgmt_errstr(status), status);
		resp_mgmt(status == MGMT_STATUS_BUSY? err_BUSY : err_PROTO_ERR);
		return;
	}

	resp_mgmt(err_SUCCESS);
}

// Unlike Bluez, we follow BT 4.0 spec which renammed Device Discovery by Scan
static void scan(bool start)
{
	// mgmt_cp_start_discovery and mgmt_cp_stop_discovery are the same
	struct mgmt_cp_start_discovery cp = { (1 << BDADDR_LE_PUBLIC) | (1 << BDADDR_LE_RANDOM) };
	uint16_t opcode = start? MGMT_OP_START_DISCOVERY : MGMT_OP_STOP_DISCOVERY;

	DBG("Scan %s", start? "start" : "stop");

	if (mgmt_send(mgmt_master, opcode, 0, sizeof(cp),
		&cp, scan_cb, NULL, NULL) == 0)
	{
		DBG("mgmt_send(MGMT_OP_%s_DISCOVERY) failed", start? "START" : "STOP");
		resp_mgmt(err_PROTO_ERR);
		return;
	}
}

static void cmd_scanend(int argcp, char **argvp)
{
	if (1 < argcp) {
		resp_mgmt(err_BAD_PARAM);
	} else {
		scan(FALSE);
	}
}

static void cmd_scan(int argcp, char **argvp)
{
	if (1 < argcp) {
		resp_mgmt(err_BAD_PARAM);
	} else {
		scan(TRUE);
	}
}

static struct {
	const char *cmd;
	void (*func)(int argcp, char **argvp);
	const char *params;
	const char *desc;
} commands[] = {
	{ "help",		cmd_help,	"",
		"Show this help"},
	{ "stat",		cmd_status,	"",
		"Show current status" },
	{ "quit",		cmd_exit,	"",
		"Exit interactive mode" },
	{ "conn",		cmd_connect,	"[address [address type]]",
		"Connect to a remote device" },
	{ "disc",		cmd_disconnect,	"",
		"Disconnect from a remote device" },
	{ "svcs",		cmd_primary,	"[UUID]",
		"Primary Service Discovery" },
	{ "incl",		cmd_included,	"[start hnd [end hnd]]",
		"Find Included Services" },
	{ "char",		cmd_char,	"[start hnd [end hnd [UUID]]]",
		"Characteristics Discovery" },
	{ "desc",		cmd_char_desc,	"[start hnd] [end hnd]",
		"Characteristics Descriptor Discovery" },
	{ "rd",			cmd_read_hnd,	"<handle>",
		"Characteristics Value/Descriptor Read by handle" },
	{ "rdu",		cmd_read_uuid,	"<UUID> [start hnd] [end hnd]",
		"Characteristics Value/Descriptor Read by UUID" },
	{ "wrr",		cmd_char_write_rsp, "<handle> <new value>",
		"Characteristic Value Write (Write Request)" },
	{ "wr",			cmd_char_write,	"<handle> <new value>",
		"Characteristic Value Write (No response)" },
	{ "secu",		cmd_sec_level,	"[low | medium | high]",
		"Set security level. Default: low" },
	{ "mtu",		cmd_mtu,	"<value>",
		"Exchange MTU for GATT/ATT" },
	{ "le",			cmd_le,		"[on | off]",
		"Control LE feature on the controller" },
	{ "pairable",		cmd_pairable,	"[on | off]",
		"Control PAIRABLE feature on the controller" },
	{ "pair",		cmd_pair,	"",
		"Start pairing with the device" },
	{ "unpair",		cmd_unpair,	"",
		"Start unpairing with the device" },
	{ "scan",		cmd_scan, 	"",
		"Start scan" },
	{ "scanend",	cmd_scanend, 	"",
		"Force scan end" },
	{ NULL, NULL, NULL}
};

static void cmd_help(int argcp, char **argvp)
{
	int i;

	for (i = 0; commands[i].cmd; i++)
		printf("#%-15s %-30s %s\n", commands[i].cmd,
				commands[i].params, commands[i].desc);
	cmd_status(0, NULL);
}

static void parse_line(char *line_read)
{
	gchar **argvp;
	int argcp;
	int i;

	line_read = g_strstrip(line_read);

	if (*line_read == '\0')
		goto done;

	g_shell_parse_argv(line_read, &argcp, &argvp, NULL);

	for (i = 0; commands[i].cmd; i++)
		if (strcasecmp(commands[i].cmd, argvp[0]) == 0)
			break;

	if (commands[i].cmd)
		commands[i].func(argcp, argvp);
	else
		resp_error(err_BAD_CMD);

	g_strfreev(argvp);

done:
	free(line_read);
}

static gboolean prompt_read(GIOChannel *chan, GIOCondition cond,
							gpointer user_data)
{
	gchar *myline;

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		DBG("Quitting IO channel error");
		g_main_loop_quit(event_loop);
		return FALSE;
	}

	if ( G_IO_STATUS_NORMAL != g_io_channel_read_line(chan, &myline, NULL, NULL, NULL)
		|| myline == NULL
	)
	{
		DBG("Quitting on input read fail");
		g_main_loop_quit(event_loop);
		return FALSE;
	}

	parse_line(myline);
	return TRUE;
}


static void read_version_complete(uint8_t status, uint16_t length,
								const void *param, void *user_data)
{
	const struct mgmt_rp_read_version *rp = param;

	if (status != MGMT_STATUS_SUCCESS) {
		DBG("Failed to read version information: %s (0x%02x)",
			mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		DBG("Wrong size of read version response");
		return;
	}

	DBG("Bluetooth management interface %u.%u initialized",
		rp->version, btohs(rp->revision));
}

static void mgmt_device_connected(uint16_t index, uint16_t length,
								const void *param, void *user_data)
{
	DBG("New device connected");
}

static void mgmt_scanning(uint16_t index, uint16_t length,
			const void *param, void *user_data)
{
	const struct mgmt_ev_discovering *ev = param;
	assert(length == sizeof(*ev));

	DBG("Scanning (0x%x): %s", ev->type, ev->discovering? "started" : "ended");

	set_state(ev->discovering? STATE_SCANNING : STATE_DISCONNECTED);
}

static void mgmt_device_found(uint16_t index, uint16_t length,
							const void *param, void *user_data)
{
	const struct mgmt_ev_device_found *ev = param;
	assert(length == sizeof(*ev) + ev->eir_len);

	// Result sometimes sent too early
	if (conn_state != STATE_SCANNING)
		return;

	resp_begin(rsp_SCAN);
	send_addr(&ev->addr);
	send_uint(tag_RSSI, -ev->rssi);
	send_uint(tag_FLAG, -ev->flags);
	if (ev->eir_len)
		send_data(ev->eir, ev->eir_len);
	resp_end();
}

static void mgmt_debug(const char *str, void *user_data)
{
	//const char *prefix = user_data;

	DBG("%s%s", (const char *)user_data, str);
}

int main(int argc, char *argv[])
{
	GIOChannel *pchan;
	gint events;

	opt_sec_level = g_strdup("low");

	opt_src = NULL;
	opt_dst = NULL;
	opt_dst_type = g_strdup("public");

	DBG(__FILE__ " built at " __TIME__ " on " __DATE__);

	mgmt_master = mgmt_new_default();
	if (!mgmt_master) {
		DBG("Could not connect to the BT management interface, try with su rights");
	}
	mgmt_set_debug(mgmt_master, mgmt_debug, "mgmt: ", NULL);

	if (mgmt_send(mgmt_master, MGMT_OP_READ_VERSION,
			MGMT_INDEX_NONE, 0, NULL,
			read_version_complete, NULL, NULL) == 0) {
		DBG("mgmt_send(MGMT_OP_READ_VERSION) failed");
	}

	if (mgmt_register(mgmt_master, MGMT_EV_DEVICE_CONNECTED, 0, mgmt_device_connected, NULL, NULL)==0) {
		DBG("mgmt_register(MGMT_EV_DEVICE_CONNECTED) failed");
	}

	if (mgmt_register(mgmt_master, MGMT_EV_DISCOVERING, 0, mgmt_scanning, NULL, NULL)) {
		DBG("mgmt_register(MGMT_EV_DISCOVERING) failed");
	}

	if (mgmt_register(mgmt_master, MGMT_EV_DEVICE_FOUND, 0, mgmt_device_found, NULL, NULL)) {
		DBG("mgmt_register(MGMT_EV_DEVICE_FOUND) failed");
	}

	event_loop = g_main_loop_new(NULL, FALSE);

	pchan = g_io_channel_unix_new(fileno(stdin));
	g_io_channel_set_close_on_unref(pchan, TRUE);
	events = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	g_io_add_watch(pchan, events, prompt_read, NULL);

	DBG("Starting loop");
	g_main_loop_run(event_loop);

	DBG("Exiting loop");
	cmd_disconnect(0, NULL);
	fflush(stdout);
	g_io_channel_unref(pchan);
	g_main_loop_unref(event_loop);

	g_free(opt_src);
	g_free(opt_dst);
	g_free(opt_sec_level);

	mgmt_unregister_index(mgmt_master, MGMT_INDEX_NONE);
	mgmt_cancel_index(mgmt_master, MGMT_INDEX_NONE);
	mgmt_unref(mgmt_master);
	mgmt_master = NULL;

	return EXIT_SUCCESS;
}

