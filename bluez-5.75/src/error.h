/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2007-2008  Fabien Chevalier <fabchevalier@free.fr>
 *
 *
 */

#include <dbus/dbus.h>
#include <stdint.h>

#define ERROR_INTERFACE "org.bluez.Error"

/* BR/EDR connection failure reasons */
#define ERR_BREDR_CONN_ALREADY_CONNECTED	"br-connection-already-"\
						"connected"
#define ERR_BREDR_CONN_PAGE_TIMEOUT		"br-connection-page-timeout"
#define ERR_BREDR_CONN_PROFILE_UNAVAILABLE	"br-connection-profile-"\
						"unavailable"
#define ERR_BREDR_CONN_SDP_SEARCH		"br-connection-sdp-search"
#define ERR_BREDR_CONN_CREATE_SOCKET		"br-connection-create-socket"
#define ERR_BREDR_CONN_INVALID_ARGUMENTS	"br-connection-invalid-"\
						"argument"
#define ERR_BREDR_CONN_ADAPTER_NOT_POWERED	"br-connection-adapter-not-"\
						"powered"
#define ERR_BREDR_CONN_NOT_SUPPORTED		"br-connection-not-supported"
#define ERR_BREDR_CONN_BAD_SOCKET		"br-connection-bad-socket"
#define ERR_BREDR_CONN_MEMORY_ALLOC		"br-connection-memory-"\
						"allocation"
#define ERR_BREDR_CONN_BUSY			"br-connection-busy"
#define ERR_BREDR_CONN_CNCR_CONNECT_LIMIT	"br-connection-concurrent-"\
						"connection-limit"
#define ERR_BREDR_CONN_TIMEOUT			"br-connection-timeout"
#define ERR_BREDR_CONN_REFUSED			"br-connection-refused"
#define ERR_BREDR_CONN_ABORT_BY_REMOTE		"br-connection-aborted-by-"\
						"remote"
#define ERR_BREDR_CONN_ABORT_BY_LOCAL		"br-connection-aborted-by-"\
						"local"
#define ERR_BREDR_CONN_LMP_PROTO_ERROR		"br-connection-lmp-protocol-"\
						"error"
#define ERR_BREDR_CONN_CANCELED			"br-connection-canceled"
#define ERR_BREDR_CONN_KEY_MISSING		"br-connection-key-missing"
#define ERR_BREDR_CONN_UNKNOWN			"br-connection-unknown"

/* LE connection failure reasons */
#define ERR_LE_CONN_INVALID_ARGUMENTS	"le-connection-invalid-arguments"
#define ERR_LE_CONN_ADAPTER_NOT_POWERED	"le-connection-adapter-not-powered"
#define ERR_LE_CONN_NOT_SUPPORTED	"le-connection-not-supported"
#define ERR_LE_CONN_ALREADY_CONNECTED	"le-connection-already-connected"
#define ERR_LE_CONN_BAD_SOCKET		"le-connection-bad-socket"
#define ERR_LE_CONN_MEMORY_ALLOC	"le-connection-memory-allocation"
#define ERR_LE_CONN_BUSY		"le-connection-busy"
#define ERR_LE_CONN_REFUSED		"le-connection-refused"
#define ERR_LE_CONN_CREATE_SOCKET	"le-connection-create-socket"
#define ERR_LE_CONN_TIMEOUT		"le-connection-timeout"
#define ERR_LE_CONN_SYNC_CONNECT_LIMIT	"le-connection-concurrent-connection-"\
					"limit"
#define ERR_LE_CONN_ABORT_BY_REMOTE	"le-connection-abort-by-remote"
#define ERR_LE_CONN_ABORT_BY_LOCAL	"le-connection-abort-by-local"
#define ERR_LE_CONN_LL_PROTO_ERROR	"le-connection-link-layer-protocol-"\
					"error"
#define ERR_LE_CONN_GATT_BROWSE		"le-connection-gatt-browsing"
#define ERR_LE_CONN_KEY_MISSING		"le-connection-key-missing"
#define ERR_LE_CONN_UNKNOWN		"le-connection-unknown"

DBusMessage *btd_error_invalid_args(DBusMessage *msg);
DBusMessage *btd_error_invalid_args_str(DBusMessage *msg, const char *str);
DBusMessage *btd_error_busy(DBusMessage *msg);
DBusMessage *btd_error_already_exists(DBusMessage *msg);
DBusMessage *btd_error_not_supported(DBusMessage *msg);
DBusMessage *btd_error_not_connected(DBusMessage *msg);
DBusMessage *btd_error_already_connected(DBusMessage *msg);
DBusMessage *btd_error_not_available(DBusMessage *msg);
DBusMessage *btd_error_not_available_str(DBusMessage *msg, const char *str);
DBusMessage *btd_error_in_progress(DBusMessage *msg);
DBusMessage *btd_error_in_progress_str(DBusMessage *msg, const char *str);
DBusMessage *btd_error_does_not_exist(DBusMessage *msg);
DBusMessage *btd_error_not_authorized(DBusMessage *msg);
DBusMessage *btd_error_not_permitted(DBusMessage *msg, const char *str);
DBusMessage *btd_error_no_such_adapter(DBusMessage *msg);
DBusMessage *btd_error_agent_not_available(DBusMessage *msg);
DBusMessage *btd_error_not_ready(DBusMessage *msg);
DBusMessage *btd_error_not_ready_str(DBusMessage *msg, const char *str);
DBusMessage *btd_error_failed(DBusMessage *msg, const char *str);

const char *btd_error_bredr_conn_from_errno(int errno_code);
const char *btd_error_le_conn_from_errno(int errno_code);
