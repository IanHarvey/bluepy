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
#include "lib/sdp.h"
#include "lib/uuid.h"
#include "lib/mgmt.h"
#include "src/shared/mgmt.h"

#include <btio/btio.h>
#include "att.h"
#include "gattrib.h"
#include "gatt.h"
#include "gatttool.h"
#include "version.h"

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

static uint16_t mgmt_ind = MGMT_INDEX_NONE;
static struct mgmt *mgmt_master = NULL;

static int hci_dd = -1;
static GIOChannel *hci_io = NULL;

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
  *tag_RESPONSE  = "rsp",
  *tag_ERRCODE   = "code",
  *tag_ERRSTAT   = "estat",
  *tag_ERRMSG    = "emsg",
  *tag_HANDLE    = "hnd",
  *tag_UUID      = "uuid",
  *tag_DATA      = "d",
  *tag_CONNSTATE = "state",
  *tag_SEC_LEVEL = "sec",
  *tag_MTU       = "mtu",
  *tag_DEVICE    = "dst",
  *tag_RANGE_START = "hstart",
  *tag_RANGE_END = "hend",
  *tag_PROPERTIES= "props",
  *tag_VALUE_HANDLE = "vhnd",
  *tag_ADDR       = "addr",
  *tag_TYPE       = "type",
  *tag_RSSI       = "rssi",
  *tag_FLAG       = "flag";

static const char
  *rsp_ERROR     = "err",
  *rsp_STATUS    = "stat",
  *rsp_NOTIFY    = "ntfy",
  *rsp_IND       = "ind",
  *rsp_DISCOVERY = "find",
  *rsp_DESCRIPTORS = "desc",
  *rsp_READ      = "rd",
  *rsp_WRITE     = "wr",
  *rsp_MGMT      = "mgmt",
  *rsp_SCAN      = "scan",
  *rsp_OOB       = "oob";

static const char
  *err_CONN_FAIL = "connfail",
  *err_ATT_ERR   = "atterr",   /* Use for ATT error codes */
  *err_MGMT_ERR  = "mgmterr",  /* Use for Mgmt socket error codes */
  *err_DECODING  = "decodeerr",
  *err_SEND_FAIL = "sendfail",
  *err_CALL_FAIL = "callfail",
  *err_NOT_FOUND = "notfound",
  *err_BAD_CMD   = "badcmd",
  *err_BAD_PARAM = "badparam",
  *err_BAD_STATE = "badstate",
  *err_BUSY      = "busy",
  *err_NO_MGMT   = "nomgmt",
  *err_SUCCESS   = "success";

static const char
  *st_DISCONNECTED = "disc",
  *st_CONNECTING   = "tryconn",
  *st_CONNECTED    = "conn",
  *st_SCANNING    = "scan";

// delimits fields in response message
#define RESP_DELIM "\x1e"

static void resp_begin(const char *rsptype)
{
  printf("%s=$%s", tag_RESPONSE, rsptype);
}

static void send_sym(const char *tag, const char *val)
{
  printf(RESP_DELIM "%s=$%s", tag, val);
}

static void send_uint(const char *tag, unsigned int val)
{
  printf(RESP_DELIM "%s=h%X", tag, val);
}

static void send_str(const char *tag, const char *val)
{
  printf(RESP_DELIM "%s='%s", tag, val);
}

static void send_data(const unsigned char *val, size_t len)
{
  printf(RESP_DELIM "%s=b", tag_DATA);
  while ( len-- > 0 )
    printf("%02X", *val++);
}

static void send_addr(const struct mgmt_addr_info *addr)
{
    const uint8_t *val = addr->bdaddr.b;
    printf(RESP_DELIM "%s=b", tag_ADDR);
    int len = 6;
    /* Human-readable byte order is reverse of bdaddr.b */
    while ( len-- > 0 )
        printf("%02X", val[len]);

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

static void resp_str_error(const char *errcode, const char *msg)
{
  resp_begin(rsp_ERROR);
  send_sym(tag_ERRCODE, errcode);
  send_str(tag_ERRMSG, msg);
  resp_end();
}

static void resp_att_error(uint8_t status)
{
  resp_begin(rsp_ERROR);
  send_sym(tag_ERRCODE, err_ATT_ERR);
  send_uint(tag_ERRSTAT, status);
  send_str(tag_ERRMSG, att_ecode2str(status));
  resp_end();
}

static void resp_mgmt(const char *errcode)
{
  resp_begin(rsp_MGMT);
  send_sym(tag_ERRCODE, errcode);
  resp_end();
}

static void resp_mgmt_err(uint8_t status)
{
  resp_begin(rsp_MGMT);
  send_sym(tag_ERRCODE, err_MGMT_ERR);
  send_uint(tag_ERRSTAT, status);
  send_str(tag_ERRMSG, mgmt_errstr(status));
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

static void gatts_mtu_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
    uint8_t *opdu;
    uint8_t opcode;
    uint16_t mtu, olen;
    size_t plen;

    assert( len >= 3 );
    opcode = pdu[0];

    if (!dec_mtu_req(pdu, len, &mtu)) {
        resp_error(err_DECODING);
        return;
    }

    opdu = g_attrib_get_buffer(attrib, &plen);

    // According to the Bluetooth specification, we're supposed to send the response
    // before applying the new MTU value:
    //   This ATT_MTU value shall be applied in the server after this response has
    //   been sent and before any other Attribute protocol PDU is sent.
    // But if we do it in that order, what happens if setting the MTU fails?

    // set new value for MTU
    if (g_attrib_set_mtu(attrib, mtu))
    {
        opt_mtu = mtu;
        olen = enc_mtu_resp(mtu, opdu, plen);
        cmd_status(0, NULL);
    }
    else {
        // send NOT SUPPORTED
        olen = enc_error_resp(opcode, mtu, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
    }
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
        set_state(STATE_DISCONNECTED);
        resp_str_error(err_CONN_FAIL, err->message);
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

    attrib = g_attrib_new(iochannel, mtu, false);

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
    g_attrib_register(attrib, ATT_OP_MTU_REQ, GATTRIB_ALL_HANDLES,
                      gatts_mtu_req, attrib, NULL);

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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
        return;
    }

    vlen = dec_read_resp(pdu, plen, value, sizeof(value));
    if (vlen < 0) {
        resp_error(err_DECODING); /* TODO: -vlen is an error code */
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
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
        g_free(opt_src);
        if (argcp > 3) {
            opt_src = g_strdup(argvp[3]);
        } else {
            opt_src = NULL;
        }
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
        g_io_add_watch(iochannel, G_IO_HUP | G_IO_NVAL, channel_watcher, NULL);
}

static void cmd_disconnect(int argcp, char **argvp)
{
    DBG("");
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
        return;
    }

    if (!dec_write_resp(pdu, plen) && !dec_exec_write_resp(pdu, plen)) {
        resp_error(err_DECODING);
        return;
    }

    resp_begin(rsp_WRITE);
    resp_end();
}


static void cmd_char_write_common(int argcp, char **argvp, int with_response)
{
    uint8_t *value = NULL;
    size_t plen;
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
    if (handle <= 0) {
        resp_error(err_BAD_PARAM);
        return;
    }

    if (argcp >= 3) {
      plen = gatt_attr_data_from_string(argvp[2], &value);
      if (plen == 0) {
          resp_error(err_BAD_PARAM);
          return;
      }
    } else {
      plen = 0;
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
        resp_str_error(err_CALL_FAIL, gerr->message);
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
        DBG("status returned error : %s (0x%02x)",
            att_ecode2str(status), status);
        resp_att_error(status);
        return;
    }

    if (!dec_mtu_resp(pdu, plen, &mtu)) {
        resp_error(err_DECODING);
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
        resp_error(err_CALL_FAIL);
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
        resp_mgmt_err(status);
        return;
    }

    resp_mgmt(err_SUCCESS);
}

static bool set_mode(uint16_t opcode, char *p_mode)
{
    struct mgmt_mode cp;
    uint8_t val;

    if (!mgmt_master) {
        resp_error(err_NO_MGMT);
        return true;
    }

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
            mgmt_ind, sizeof(cp), &cp,
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

static void add_remote_oob_data_complete(uint8_t status, uint16_t len,
                    const void *param, void *user_data)
{
    const struct mgmt_addr_info *rp = param;
    char str[18];
    if (status) {
        DBG("status returned error : %s (0x%02x)",
            mgmt_errstr(status), status);
        resp_mgmt_err(status);
        return;
    }
    ba2str(&rp->bdaddr, str);
    DBG("  Remote data added for : %s\n", str);
}

static bool add_remote_oob_data(uint16_t index, const bdaddr_t *bdaddr,
                const uint8_t addr_type,
                const char *hash192, const char *rand192,
                const char *hash256, const char *rand256)
{
    struct mgmt_cp_add_remote_oob_data cp;
    uint8_t *oob;
    size_t len;

    if (!mgmt_master) {
        resp_error(err_NO_MGMT);
        return true;
    }

    memset(&cp, 0, sizeof(cp));
    bacpy(&cp.addr.bdaddr, bdaddr);
    cp.addr.type = addr_type;
    if (hash192 && rand192) {
        len = gatt_attr_data_from_string(hash192, &oob);
        if (len == 0) {
            resp_error(err_BAD_PARAM);
            g_free(oob);
            return false;
        }
        memcpy(cp.hash192, oob, 16);
        g_free(oob);
        len = gatt_attr_data_from_string(rand192, &oob);
        if (len == 0) {
            resp_error(err_BAD_PARAM);
            memset(cp.hash192, 0, 16);
            g_free(oob);
            return false;
        }
        memcpy(cp.rand192, rand192, 16);
        g_free(oob);
    } else {
        memset(cp.hash192, 0, 16);
        memset(cp.rand192, 0, 16);
    }
    if (hash256 && rand256) {
        len = gatt_attr_data_from_string(hash256, &oob);
        if (len == 0) {
            resp_error(err_BAD_PARAM);
            memset(cp.hash192, 0, 16);
            memset(cp.rand192, 0, 16);
            g_free(oob);
            return false;
        }
        memcpy(cp.hash256, oob, 16);
        g_free(oob);
        len = gatt_attr_data_from_string(rand256, &oob);
        if (len == 0) {
            resp_error(err_BAD_PARAM);
            memset(cp.hash192, 0, 16);
            memset(cp.rand192, 0, 16);
            memset(cp.hash256, 0, 16);
            g_free(oob);
            return false;
        }
        memcpy(cp.rand256, rand256, 16);
        g_free(oob);
    } else {
        memset(cp.hash256, 0, 16);
        memset(cp.rand256, 0, 16);
    }
    if (mgmt_send(mgmt_master, MGMT_OP_ADD_REMOTE_OOB_DATA, mgmt_ind, sizeof(cp), &cp,
                        add_remote_oob_data_complete,
                        NULL, NULL) == 0) {
        resp_error(err_SEND_FAIL);
        g_free(oob);
        return false;
    }
    g_free(oob);
    return true;
}

static void cmd_add_oob(int argcp, char **argvp)
{
    bdaddr_t bdaddr;
    char *C192 = NULL;
    char *R192 = NULL;
    char *C256 = NULL;
    char *R256 = NULL;
    uint8_t addr_type = BDADDR_LE_RANDOM;

    if (argcp < 7) {
        resp_mgmt(err_BAD_PARAM);
        return;
    }

    if (str2ba(argvp[1], &bdaddr)) {
        resp_mgmt(err_NOT_FOUND);
        return;
    }

    if (!memcmp(argvp[2], "public", 6)) {
        addr_type = BDADDR_LE_PUBLIC;
    }

    if ((!memcmp(argvp[3], "C_192", 5)) && (!memcmp(argvp[5], "R_192", 5))) {
        C192 = argvp[4];
        R192 = argvp[6];
        if ((argcp > 8) && !memcmp(argvp[5], "C_256", 5) && (!memcmp(argvp[7], "R_256", 5))) {
            C256 = argvp[6];
            R256 = argvp[8];
        }
    } else if ((!memcmp(argvp[3], "C_256", 5)) && (!memcmp(argvp[5], "R_256", 5))) {
        C256 = argvp[4];
        R256 = argvp[6];
    }

    if (!add_remote_oob_data(0, &bdaddr, addr_type, C192, R192, C256, R256)) {
        DBG("Failed to add remote oob data");
    }
}

static void read_local_oob_data_complete(uint8_t status, uint16_t len,
                    const void *param, void *user_data)
{
    const struct mgmt_rp_read_local_oob_ext_data *rp = param;
    uint32_t eir_len = rp->eir_len;
    unsigned int i;

    if (status) {
        DBG("status returned error : %s (0x%02x)",
            mgmt_errstr(status), status);
        resp_mgmt_err(status);
        return;
    }
    DBG("received local OOB ext with eir_len = %d",eir_len);
    for (i = 0; i<eir_len; i++)
        DBG("0x%02x ", rp->eir[i]);
    
    resp_begin(rsp_OOB);
    send_data(rp->eir, eir_len);
    resp_end();
}

static bool read_local_oob_data(uint16_t index)
{
    struct mgmt_cp_read_local_oob_ext_data cp;

    if (!mgmt_master) {
        resp_error(err_NO_MGMT);
        return true;
    }
    /* For now we only handle BLE OOB */
    cp.type = 6;
    if (mgmt_send(mgmt_master, MGMT_OP_READ_LOCAL_OOB_EXT_DATA, mgmt_ind, sizeof(cp), &cp,
                        read_local_oob_data_complete,
                        NULL, NULL) == 0) {
        resp_error(err_SEND_FAIL);
        return false;
    }
    return true;
}

static void cmd_read_oob(int argcp, char **argvp)
{
    if (!read_local_oob_data(0)) {
        DBG("Failed to read local oob data");
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
        resp_mgmt_err(status);
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

    if (!mgmt_master) {
        resp_error(err_NO_MGMT);
        return;
    }

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
            mgmt_ind, sizeof(cp), &cp,
                pair_device_complete, NULL,
                NULL) == 0) {
        DBG("mgmt_send(MGMT_OP_PAIR_DEVICE) failed for %s for hci%u", opt_dst, mgmt_ind);
        resp_mgmt(err_SEND_FAIL);
        return;
    }
}

static void unpair_device_complete(uint8_t status, uint16_t length,
                    const void *param, void *user_data)
{
    if (status != MGMT_STATUS_SUCCESS) {
        DBG("status returned error : %s (0x%02x)",
                mgmt_errstr(status), status);
        resp_mgmt_err(status);
        return;
    }

    resp_mgmt(err_SUCCESS);
}

static void cmd_unpair(int argcp, char **argvp)
{
    struct mgmt_cp_unpair_device cp;
    bdaddr_t bdaddr;
    uint8_t addr_type = BDADDR_LE_RANDOM;

    if (!mgmt_master) {
        resp_error(err_NO_MGMT);
        return;
    }

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
            mgmt_ind, sizeof(cp), &cp,
            unpair_device_complete, NULL,
                NULL) == 0) {
        DBG("mgmt_send(MGMT_OP_UNPAIR_DEVICE) failed for %s for hci%u", opt_dst, mgmt_ind);
        resp_mgmt(err_SEND_FAIL);
        return;
    }
}

static void scan_cb(uint8_t status, uint16_t length, const void *param, void *user_data)
{
    if (status != MGMT_STATUS_SUCCESS) {
        DBG("Scan error: %s (0x%02x)", mgmt_errstr(status), status);
        if (status==MGMT_STATUS_BUSY)
          resp_mgmt(err_BUSY);
        else
          resp_mgmt_err(status);
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

    if (!mgmt_master) {
        resp_error(err_NO_MGMT);
        return;
    }

    DBG("Scan %s", start? "start" : "stop");

    if (mgmt_send(mgmt_master, opcode, mgmt_ind, sizeof(cp),
        &cp, scan_cb, NULL, NULL) == 0)
    {
        DBG("mgmt_send(MGMT_OP_%s_DISCOVERY) failed", start? "START" : "STOP");
        resp_mgmt(err_SEND_FAIL);
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

#include "hci.h"
#include "hci_lib.h"

static gboolean hci_monitor_cb(GIOChannel *chan, GIOCondition cond, gpointer user_data)
{
    unsigned char buf[HCI_MAX_FRAME_SIZE], *ptr;
    int type;
    gsize len;
    GError *err= NULL;
    int r;

    if ((r= g_io_channel_read_chars(chan, (gchar *) buf, 1, &len, &err)) != G_IO_STATUS_NORMAL) {
        if (err) DBG("reading pkt type reports state %d: %s", r, err->message);
        //andy: stop passive scan
        return TRUE;
    }
    type= *buf;
    switch (type) {
        case HCI_COMMAND_PKT: {
            hci_command_hdr *ch;
            if ((r= g_io_channel_read_chars(chan, (gchar *) buf, HCI_COMMAND_HDR_SIZE, &len, &err)) != G_IO_STATUS_NORMAL) {
                if (err) DBG("g_io_channel_read_chars() reports state %d: %s", r, err->message);
                return TRUE;
            }
            ch = (hci_command_hdr *) buf;
            ptr = buf + HCI_COMMAND_HDR_SIZE;
            if ((r= g_io_channel_read_chars(chan, (gchar *) ptr, ch->plen, &len, &err)) != G_IO_STATUS_NORMAL) {
                if (err) DBG("g_io_channel_read_chars() reports state %d: %s", r, err->message);
                return TRUE;
            }
            switch(ch->opcode) {
                case 0x2000|OCF_LE_SET_SCAN_ENABLE: {
                    le_set_scan_enable_cp *lescan = (le_set_scan_enable_cp *) ptr;
                    if (lescan->enable) {
                        DBG("Start of passive scan.");
                    } else {
                        if (conn_state == STATE_SCANNING) {
                            set_state(STATE_DISCONNECTED);
                        }
                        DBG("End of passive scan - removing watch.");
                        return FALSE; // remove watch
                    }
                }
                break;

                default:
                    DBG("Ignoring HCI COMMAND 0x%04x", ch->opcode);
            } // switch(ch->opcode)
        } break;

        case HCI_EVENT_PKT: {
            hci_event_hdr *eh;
            if ((r= g_io_channel_read_chars(chan, (gchar *) buf, HCI_EVENT_HDR_SIZE, &len, &err)) != G_IO_STATUS_NORMAL) {
                if (err) DBG("g_io_channel_read_chars() reports state %d: %s", r, err->message);
                return TRUE;
            }
            eh = (hci_event_hdr *) buf;
            ptr = buf + HCI_EVENT_HDR_SIZE;
            if ((r= g_io_channel_read_chars(chan, (gchar *) ptr, eh->plen, &len, &err)) != G_IO_STATUS_NORMAL) {
                if (err) DBG("g_io_channel_read_chars() reports state %d: %s", r, err->message);
                return TRUE;
            }
            switch(eh->evt) {
                case EVT_CMD_COMPLETE: {
                    // evt_cmd_complete *cmpl = (void *) ptr;
                    // DBG("command complete (0x%02x|0x%04x) 0x%02x 0x%02x", cmpl->ncmd, cmpl->opcode, *(uint8_t *)(ptr+3), *(uint8_t *)(ptr+4));
                }
                break;

                case EVT_LE_META_EVENT: {
                    evt_le_meta_event *meta = (void *) ptr;

                    switch(meta->subevent) {
                        case EVT_LE_ADVERTISING_REPORT: {
                            le_advertising_info *ev = (le_advertising_info *) (meta->data + 1);
                            // const uint8_t *val= ev->bdaddr.b;
                            const uint8_t rssi= ev->data[ev->length];
                            struct mgmt_addr_info addr;
                            switch (ev->bdaddr_type) {
                                case LE_PUBLIC_ADDRESS: addr.type= BDADDR_LE_PUBLIC; break;
                                case LE_RANDOM_ADDRESS: addr.type= BDADDR_LE_RANDOM; break;
                                default: addr.type= 0;
                            }
                            addr.bdaddr= ev->bdaddr;
                            // DBG("Device found: %02X:%02X:%02X:%02X:%02X:%02X type=%X length=%d data[0]=0x%02x rssi=0x%02x",
                            //     val[5], val[4], val[3], val[2], val[1], val[0],
                            //     ev->bdaddr_type, ev->length, ev->data[0], ev->data[ev->length]);
                            if (0) {
                                int i=0;
                                for (i=0; i<ev->length; i++)
                                    DBG("buf: %02x", ev->data[i]);
                            }

                            if (conn_state == STATE_SCANNING) {
                                resp_begin(rsp_SCAN);
                                send_addr(&addr);
                                send_uint(tag_RSSI, 256-rssi);
                                send_uint(tag_FLAG, 0);   //andy: where do we get these from?
                                if (ev->length)
                                    send_data(ev->data, ev->length);
                                resp_end();
                            }
                        }
                        break;

                        default:
                            DBG("Ignoring EVT_LE_ADVERTISING_REPORT subevent %02x", meta->subevent);
                            return TRUE;
                    } // switch (meta->subevent)

                } // case EVT_LE_META_EVENT
                break;
                default:
                    DBG("Ignoring event %02x", eh->evt);
                    return TRUE;
            } // switch(eh->evt)

        } // case HCI_EVENT_PKT
        break;

        default:
            DBG("Ignoring packet type %02x", type);
            return TRUE;
    }// switch (type)
    return TRUE;
}


// perform a passive scan, i.e. report ADV_IND packets but do not request SCN_RSP packets
static void discover(bool start)
{
    int err;
    uint8_t own_type = LE_PUBLIC_ADDRESS;
    uint8_t scan_type = 0x00;  // passive
    uint8_t filter_policy = 0x00;
    uint16_t interval = htobs(0x0010);
    uint16_t window = htobs(0x0010);
    uint8_t filter_dup = 0x00;  // do not filter duplicates

    struct hci_filter nf, of;
    //struct sigaction sa;
    socklen_t olen;

    hci_dd = hci_open_dev(mgmt_ind);
    DBG("hcidev handle is 0x%x, mgmt_ind is %d", hci_dd, mgmt_ind);
    if (start) {
        err = hci_le_set_scan_enable(hci_dd, 0x00, filter_dup, 10000);
        err = hci_le_set_scan_parameters(hci_dd, scan_type, interval, window,
                                             own_type, filter_policy, 10000);
        if (err < 0) {
            DBG("Set scan parameters failed");
            resp_mgmt(err_BAD_STATE);
            return;
        }
        hci_io = g_io_channel_unix_new(hci_dd);
        g_io_channel_set_encoding(hci_io, NULL, NULL);
        g_io_channel_set_close_on_unref(hci_io, TRUE);
        g_io_add_watch(hci_io, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, hci_monitor_cb, NULL);
        g_io_channel_unref(hci_io);

        // setup filter
        olen = sizeof(of);
        if (getsockopt(hci_dd, SOL_HCI, HCI_FILTER, &of, &olen) < 0) {
            printf("Could not get socket options\n");
            resp_mgmt(err_BAD_STATE);
            return;
        }
        hci_filter_clear(&nf);
        hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
        hci_filter_set_event(EVT_LE_META_EVENT, &nf);
        hci_filter_set_event(EVT_CMD_COMPLETE, &nf);
        hci_filter_set_ptype(HCI_COMMAND_PKT, &nf);
        hci_filter_set_event(OCF_LE_SET_SCAN_ENABLE, &nf);

        if (setsockopt(hci_dd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
            printf("Could not set socket options\n");
            resp_mgmt(err_BAD_STATE);
            return;
        }

        DBG("LE Scan ...");
        err = hci_le_set_scan_enable(hci_dd, 0x01, filter_dup, 10000);
        if (err < 0) {
            //andy: signal error
            DBG("Enable scan failed");
            resp_mgmt(err_BAD_STATE);
            return;
        }

        resp_mgmt(err_SUCCESS);
        set_state(STATE_SCANNING);
    } else {
        const char* errcode = err_SUCCESS;

        // set filter to receive no events
        DBG(" stop pasv scan -----------------------------------");
        setsockopt(hci_dd, SOL_HCI, HCI_FILTER, &of, sizeof(of));

        err = hci_le_set_scan_enable(hci_dd, 0x00, filter_dup, 10000);
        if (err < 0) {
            DBG("Disable scan failed");
            errcode = err_BAD_STATE;
        }
        hci_close_dev(hci_dd);
        hci_dd= -1;
        hci_io= NULL;
        resp_mgmt(errcode);
        set_state(STATE_DISCONNECTED);
    }
}

static void cmd_pasvend(int argcp, char **argvp)
{
    if (1 < argcp) {
        resp_mgmt(err_BAD_PARAM);
    } else {
        discover(FALSE);
    }
}

static void cmd_pasv(int argcp, char **argvp)
{
    if (1 < argcp) {
        resp_mgmt(err_BAD_PARAM);
    } else {
        discover(TRUE);
    }
}

static struct {
    const char *cmd;
    void (*func)(int argcp, char **argvp);
    const char *params;
    const char *desc;
} commands[] = {
    { "help",       cmd_help,   "",
        "Show this help"},
    { "stat",       cmd_status, "",
        "Show current status" },
    { "quit",       cmd_exit,   "",
        "Exit interactive mode" },
    { "conn",       cmd_connect,    "[address [address type [interface]]]",
        "Connect to a remote device" },
    { "disc",       cmd_disconnect, "",
        "Disconnect from a remote device" },
    { "svcs",       cmd_primary,    "[UUID]",
        "Primary Service Discovery" },
    { "incl",       cmd_included,   "[start hnd [end hnd]]",
        "Find Included Services" },
    { "char",       cmd_char,   "[start hnd [end hnd [UUID]]]",
        "Characteristics Discovery" },
    { "desc",       cmd_char_desc,  "[start hnd] [end hnd]",
        "Characteristics Descriptor Discovery" },
    { "rd",         cmd_read_hnd,   "<handle>",
        "Characteristics Value/Descriptor Read by handle" },
    { "rdu",        cmd_read_uuid,  "<UUID> [start hnd] [end hnd]",
        "Characteristics Value/Descriptor Read by UUID" },
    { "wrr",        cmd_char_write_rsp, "<handle> [<new value>]",
        "Characteristic Value Write (Write Request)" },
    { "wr",         cmd_char_write, "<handle> [<new value>]",
        "Characteristic Value Write (No response)" },
    { "secu",       cmd_sec_level,  "[low | medium | high]",
        "Set security level. Default: low" },
    { "mtu",        cmd_mtu,    "<value>",
        "Exchange MTU for GATT/ATT" },
    { "le",      cmd_le,  "[on | off]",
        "Control LE feature on the controller" },
    { "remote_oob",      cmd_add_oob,  "address [[C_192 c192] [R_192 r192]] [[C_256 c256] [R_256 r256]]",
        "Add OOB data for remote address" },
    { "local_oob",      cmd_read_oob,  "",
        "Read local OOB data" },
    { "pairable",   cmd_pairable,  "[on | off]",
        "Control PAIRABLE feature on the controller" },
    { "pair",      cmd_pair,  "",
        "Start pairing with the device" },
    { "unpair",  cmd_unpair,  "",
        "Start unpairing with the device" },
    { "scan",       cmd_scan,   "",
        "Start scan" },
    { "scanend",    cmd_scanend,    "",
        "Force scan end" },
    { "pasv",       cmd_pasv,  "",
        "Start passive scan" },
    { "pasvend",    cmd_pasvend,  "",
        "Force passive scan end" },
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

    if (!g_shell_parse_argv(line_read, &argcp, &argvp, NULL)) {
        resp_error(err_BAD_CMD);
        goto done;
    }

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
    // const uint8_t *val = ev->addr.bdaddr.b;
    assert(length == sizeof(*ev) + ev->eir_len);
    // DBG("Device found: %02X:%02X:%02X:%02X:%02X:%02X type=%X flags=%X", val[5], val[4], val[3], val[2], val[1], val[0], ev->addr.type, ev->flags);

    // Result sometimes sent too early
    if (conn_state != STATE_SCANNING)
        return;
    //confirm_name(&ev->addr, 1);

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

static void mgmt_setup(unsigned int idx)
{
    mgmt_master = mgmt_new_default();
    if (!mgmt_master) {
        DBG("Could not connect to the BT management interface, try with su rights");
        return;
    }
    DBG("Setting up mgmt on hci%u", idx);
    mgmt_ind = idx;
    mgmt_set_debug(mgmt_master, mgmt_debug, "mgmt: ", NULL);

    if (mgmt_send(mgmt_master, MGMT_OP_READ_VERSION,
        MGMT_INDEX_NONE, 0, NULL,
        read_version_complete, NULL, NULL) == 0) {
        DBG("mgmt_send(MGMT_OP_READ_VERSION) failed");
    }

    if (!mgmt_register(mgmt_master, MGMT_EV_DEVICE_CONNECTED, mgmt_ind, mgmt_device_connected, NULL, NULL)) {
        DBG("mgmt_register(MGMT_EV_DEVICE_CONNECTED) failed");
    }

    if (!mgmt_register(mgmt_master, MGMT_EV_DISCOVERING, mgmt_ind, mgmt_scanning, NULL, NULL)) {
        DBG("mgmt_register(MGMT_EV_DISCOVERING) failed");
    }

    if (!mgmt_register(mgmt_master, MGMT_EV_DEVICE_FOUND, mgmt_ind, mgmt_device_found, NULL, NULL)) {
        DBG("mgmt_register(MGMT_EV_DEVICE_FOUND) failed");
    }
}

int main(int argc, char *argv[])
{
    GIOChannel *pchan;
    gint events;

    opt_sec_level = g_strdup("low");

    opt_src = NULL;
    opt_dst = NULL;
    opt_dst_type = g_strdup("public");

    printf("# " __FILE__ " version " VERSION_STRING " built at " __TIME__ " on " __DATE__ "\n");

    if (argc > 1) {
        int index;

        if (sscanf (argv[1], "%i", &index)!=1) {
            printf("# ERROR: cannot convert '%s' to device index integer\n",argv[1]);
            exit(1);
        } else {
            mgmt_setup(index);
        }
    } else {
        // If no argument given, use index 0
        mgmt_setup(0);
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

    mgmt_unregister_index(mgmt_master, mgmt_ind);
    mgmt_cancel_index(mgmt_master, mgmt_ind);
    mgmt_unref(mgmt_master);
    mgmt_master = NULL;

    return EXIT_SUCCESS;
}


