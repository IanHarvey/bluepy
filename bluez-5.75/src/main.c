// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <glib.h>

#include <dbus/dbus.h>

#include "lib/bluetooth.h"
#include "lib/sdp.h"

#include "gdbus/gdbus.h"
#include "btio/btio.h"

#include "log.h"
#include "backtrace.h"

#include "shared/att-types.h"
#include "shared/mainloop.h"
#include "shared/timeout.h"
#include "shared/queue.h"
#include "shared/crypto.h"
#include "lib/uuid.h"
#include "shared/util.h"
#include "btd.h"
#include "sdpd.h"
#include "adapter.h"
#include "device.h"
#include "dbus-common.h"
#include "agent.h"
#include "profile.h"

#define BLUEZ_NAME "org.bluez"

#define DEFAULT_PAIRABLE_TIMEOUT           0 /* disabled */
#define DEFAULT_DISCOVERABLE_TIMEOUT     180 /* 3 minutes */
#define DEFAULT_TEMPORARY_TIMEOUT         30 /* 30 seconds */
#define DEFAULT_NAME_REQUEST_RETRY_DELAY 300 /* 5 minutes */

#define SHUTDOWN_GRACE_SECONDS 10

struct btd_opts btd_opts;
static GKeyFile *main_conf;
static char main_conf_file_path[PATH_MAX];

static const char *supported_options[] = {
	"Name",
	"Class",
	"DiscoverableTimeout",
	"AlwaysPairable",
	"PairableTimeout",
	"DeviceID",
	"ReverseServiceDiscovery",
	"NameResolving",
	"DebugKeys",
	"ControllerMode",
	"MaxControllers",
	"MultiProfile",
	"FastConnectable",
	"SecureConnections",
	"Privacy",
	"JustWorksRepairing",
	"TemporaryTimeout",
	"RefreshDiscovery",
	"Experimental",
	"KernelExperimental",
	"RemoteNameRequestRetryDelay",
	NULL
};

static const char *br_options[] = {
	"PageScanType",
	"PageScanInterval",
	"PageScanWindow",
	"InquiryScanType",
	"InquiryScanInterval",
	"InquiryScanWindow",
	"LinkSupervisionTimeout",
	"PageTimeout",
	"MinSniffInterval",
	"MaxSniffInterval",
	NULL
};

static const char *le_options[] = {
	"MinAdvertisementInterval",
	"MaxAdvertisementInterval",
	"MultiAdvertisementRotationInterval",
	"ScanIntervalAutoConnect",
	"ScanWindowAutoConnect",
	"ScanIntervalSuspend",
	"ScanWindowSuspend",
	"ScanIntervalDiscovery",
	"ScanWindowDiscovery",
	"ScanIntervalAdvMonitoring",
	"ScanWindowAdvMonitoring",
	"ScanIntervalConnect",
	"ScanWindowConnect",
	"MinConnectionInterval",
	"MaxConnectionInterval",
	"ConnectionLatency",
	"ConnectionSupervisionTimeout",
	"Autoconnecttimeout",
	"AdvMonAllowlistScanDuration",
	"AdvMonNoFilterScanDuration",
	"EnableAdvMonInterleaveScan",
	NULL
};

static const char *policy_options[] = {
	"ReconnectUUIDs",
	"ReconnectAttempts",
	"ReconnectIntervals",
	"AutoEnable",
	"ResumeDelay",
	NULL
};

static const char *gatt_options[] = {
	"Cache",
	"KeySize",
	"ExchangeMTU",
	"Channels",
	"Client",
	NULL
};

static const char *csip_options[] = {
	"SIRK",
	"Encryption",
	"Size",
	"Rank",
	NULL
};

static const char *avdtp_options[] = {
	"SessionMode",
	"StreamMode",
	NULL
};

static const char *advmon_options[] = {
	"RSSISamplingPeriod",
	NULL
};

static const struct group_table {
	const char *name;
	const char **options;
} valid_groups[] = {
	{ "General",	supported_options },
	{ "BR",		br_options },
	{ "LE",		le_options },
	{ "Policy",	policy_options },
	{ "GATT",	gatt_options },
	{ "CSIS",	csip_options },
	{ "AVDTP",	avdtp_options },
	{ "AdvMon",	advmon_options },
	{ }
};

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

static int8_t check_sirk_alpha_numeric(char *str)
{
	int8_t val = 0;
	char *s = str;

	if (strlen(s) != 32) /* 32 Bytes of Alpha numeric string */
		return 0;

	for ( ; *s; s++) {
		if (((*s >= '0') & (*s <= '9'))
			|| ((*s >= 'a') && (*s <= 'z'))
			|| ((*s >= 'A') && (*s <= 'Z'))) {
			val = 1;
		} else {
			val = 0;
			break;
		}
	}

	return val;
}

static size_t hex2bin(const char *hexstr, uint8_t *buf, size_t buflen)
{
	size_t i, len;

	if (!hexstr)
		return 0;

	len = MIN((strlen(hexstr) / 2), buflen);
	memset(buf, 0, len);

	for (i = 0; i < len; i++) {
		if (sscanf(hexstr + (i * 2), "%02hhX", &buf[i]) != 1)
			continue;
	}

	return len;
}

GKeyFile *btd_get_main_conf(void)
{
	return main_conf;
}

static GKeyFile *load_config(const char *name)
{
	GError *err = NULL;
	GKeyFile *keyfile;
	int len;

	if (name)
		snprintf(main_conf_file_path, PATH_MAX, "%s", name);
	else {
		const char *configdir = getenv("CONFIGURATION_DIRECTORY");

		/* Check if running as service */
		if (configdir) {
			/* Check if there multiple paths given */
			if (strstr(configdir, ":"))
				len = strstr(configdir, ":") - configdir;
			else
				len = strlen(configdir);
		} else {
			configdir = CONFIGDIR;
			len = strlen(configdir);
		}

		snprintf(main_conf_file_path, PATH_MAX, "%*s/main.conf", len,
						 configdir);
	}

	keyfile = g_key_file_new();

	g_key_file_set_list_separator(keyfile, ',');

	if (!g_key_file_load_from_file(keyfile, main_conf_file_path, 0, &err)) {
		if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			error("Parsing %s failed: %s", main_conf_file_path,
				err->message);
		g_error_free(err);
		g_key_file_free(keyfile);
		return NULL;
	}

	return keyfile;
}

static void parse_did(const char *did)
{
	int result;
	uint16_t vendor, product, version , source;

	vendor = 0x0000;
	product = 0x0000;
	version = 0x0000;
	source = 0x0002;

	if (!strcasecmp(did, "false")) {
		source = 0x0000;
		goto done;
	}

	result = sscanf(did, "bluetooth:%4hx:%4hx:%4hx",
					&vendor, &product, &version);
	if (result != EOF && result >= 2) {
		source = 0x0001;
		goto done;
	}

	result = sscanf(did, "usb:%4hx:%4hx:%4hx",
					&vendor, &product, &version);
	if (result != EOF && result >= 2)
		goto done;

	result = sscanf(did, "%4hx:%4hx:%4hx", &vendor, &product, &version);
	if (result == EOF || result < 2)
		return;

done:
	btd_opts.did_source = source;
	btd_opts.did_vendor = vendor;
	btd_opts.did_product = product;
	btd_opts.did_version = version;
}

static bt_gatt_cache_t parse_gatt_cache_str(const char *cache)
{
	if (!strcmp(cache, "always")) {
		return BT_GATT_CACHE_ALWAYS;
	} else if (!strcmp(cache, "yes")) {
		return BT_GATT_CACHE_YES;
	} else if (!strcmp(cache, "no")) {
		return BT_GATT_CACHE_NO;
	} else {
		DBG("Invalid value for KeepCache=%s", cache);
		return BT_GATT_CACHE_ALWAYS;
	}
}

static enum jw_repairing_t parse_jw_repairing(const char *jw_repairing)
{
	if (!strcmp(jw_repairing, "never")) {
		return JW_REPAIRING_NEVER;
	} else if (!strcmp(jw_repairing, "confirm")) {
		return JW_REPAIRING_CONFIRM;
	} else if (!strcmp(jw_repairing, "always")) {
		return JW_REPAIRING_ALWAYS;
	} else {
		return JW_REPAIRING_NEVER;
	}
}


static void check_options(GKeyFile *config, const char *group,
						const char **options)
{
	char **keys;
	int i;

	keys = g_key_file_get_keys(config, group, NULL, NULL);

	for (i = 0; keys != NULL && keys[i] != NULL; i++) {
		bool found;
		unsigned int j;

		found = false;
		for (j = 0; options != NULL && options[j] != NULL; j++) {
			if (g_str_equal(keys[i], options[j])) {
				found = true;
				break;
			}
		}

		if (!found)
			warn("Unknown key %s for group %s in %s",
					keys[i], group, main_conf_file_path);
	}

	g_strfreev(keys);
}

static void check_config(GKeyFile *config)
{
	char **keys;
	int i;
	const struct group_table *group;

	if (!config)
		return;

	keys = g_key_file_get_groups(config, NULL);

	for (i = 0; keys != NULL && keys[i] != NULL; i++) {
		bool match = false;

		for (group = valid_groups; group && group->name ; group++) {
			if (g_str_equal(keys[i], group->name)) {
				match = true;
				break;
			}
		}

		if (!match)
			warn("Unknown group %s in %s", keys[i],
						main_conf_file_path);
	}

	g_strfreev(keys);

	for (group = valid_groups; group && group->name; group++)
		check_options(config, group->name, group->options);
}

static int get_mode(const char *str)
{
	if (strcmp(str, "dual") == 0)
		return BT_MODE_DUAL;
	else if (strcmp(str, "bredr") == 0)
		return BT_MODE_BREDR;
	else if (strcmp(str, "le") == 0)
		return BT_MODE_LE;

	error("Unknown controller mode \"%s\"", str);

	return BT_MODE_DUAL;
}

static bool parse_config_string(GKeyFile *config, const char *group,
					const char *key, char **val)
{
	GError *err = NULL;
	char *tmp;

	tmp = g_key_file_get_string(config, group, key, &err);
	if (err) {
		if (err->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND)
			DBG("%s", err->message);
		g_error_free(err);
		return false;
	}

	DBG("%s.%s = %s", group, key, tmp);

	if (val) {
		g_free(*val);
		*val = tmp;
	}

	return true;
}

static bool parse_config_int(GKeyFile *config, const char *group,
					const char *key, int *val,
					size_t min, size_t max)
{
	size_t tmp;
	char *str = NULL;
	char *endptr = NULL;

	if (!parse_config_string(config, group, key, &str))
		return false;

	tmp = strtol(str, &endptr, 0);
	if (!endptr || *endptr != '\0') {
		error("%s.%s = %s is not integer", group, key, str);
		return false;
	}

	if (tmp < min) {
		warn("%s.%s = %zu is out of range (< %zu)", group, key, tmp,
									min);
		return false;
	}

	if (tmp > max) {
		warn("%s.%s = %zu is out of range (> %zu)", group, key, tmp,
									max);
		return false;
	}

	if (val)
		*val = tmp;

	return true;
}

struct config_param {
	const char * const val_name;
	void * const val;
	const size_t size;
	const uint16_t min;
	const uint16_t max;
};

static void parse_mode_config(GKeyFile *config, const char *group,
				const struct config_param *params,
				size_t params_len)
{
	uint16_t i;

	if (!config)
		return;

	for (i = 0; i < params_len; ++i) {
		int val;

		if (parse_config_int(config, group, params[i].val_name,
					&val, params[i].min, params[i].max)) {
			val = htobl(val);
			memcpy(params[i].val, &val, params[i].size);
		}

		++btd_opts.defaults.num_entries;
	}
}

static void parse_br_config(GKeyFile *config)
{
	static const struct config_param params[] = {
		{ "PageScanType",
		  &btd_opts.defaults.br.page_scan_type,
		  sizeof(btd_opts.defaults.br.page_scan_type),
		  0,
		  1},
		{ "PageScanInterval",
		  &btd_opts.defaults.br.page_scan_interval,
		  sizeof(btd_opts.defaults.br.page_scan_interval),
		  0x0012,
		  0x1000},
		{ "PageScanWindow",
		  &btd_opts.defaults.br.page_scan_win,
		  sizeof(btd_opts.defaults.br.page_scan_win),
		  0x0011,
		  0x1000},
		{ "InquiryScanType",
		  &btd_opts.defaults.br.scan_type,
		  sizeof(btd_opts.defaults.br.scan_type),
		  0,
		  1},
		{ "InquiryScanInterval",
		  &btd_opts.defaults.br.scan_interval,
		  sizeof(btd_opts.defaults.br.scan_interval),
		  0x0012,
		  0x1000},
		{ "InquiryScanWindow",
		  &btd_opts.defaults.br.scan_win,
		  sizeof(btd_opts.defaults.br.scan_win),
		  0x0011,
		  0x1000},
		{ "LinkSupervisionTimeout",
		  &btd_opts.defaults.br.link_supervision_timeout,
		  sizeof(btd_opts.defaults.br.link_supervision_timeout),
		  0x0001,
		  0xFFFF},
		{ "PageTimeout",
		  &btd_opts.defaults.br.page_timeout,
		  sizeof(btd_opts.defaults.br.page_timeout),
		  0x0001,
		  0xFFFF},
		{ "MinSniffInterval",
		  &btd_opts.defaults.br.min_sniff_interval,
		  sizeof(btd_opts.defaults.br.min_sniff_interval),
		  0x0001,
		  0xFFFE},
		{ "MaxSniffInterval",
		  &btd_opts.defaults.br.max_sniff_interval,
		  sizeof(btd_opts.defaults.br.max_sniff_interval),
		  0x0001,
		  0xFFFE},
	};

	if (btd_opts.mode == BT_MODE_LE)
		return;

	parse_mode_config(config, "BR", params, ARRAY_SIZE(params));
}

static void parse_le_config(GKeyFile *config)
{
	static const struct config_param params[] = {
		{ "MinAdvertisementInterval",
		  &btd_opts.defaults.le.min_adv_interval,
		  sizeof(btd_opts.defaults.le.min_adv_interval),
		  0x0020,
		  0x4000},
		{ "MaxAdvertisementInterval",
		  &btd_opts.defaults.le.max_adv_interval,
		  sizeof(btd_opts.defaults.le.max_adv_interval),
		  0x0020,
		  0x4000},
		{ "MultiAdvertisementRotationInterval",
		  &btd_opts.defaults.le.adv_rotation_interval,
		  sizeof(btd_opts.defaults.le.adv_rotation_interval),
		  0x0001,
		  0xFFFF},
		{ "ScanIntervalAutoConnect",
		  &btd_opts.defaults.le.scan_interval_autoconnect,
		  sizeof(btd_opts.defaults.le.scan_interval_autoconnect),
		  0x0004,
		  0x4000},
		{ "ScanWindowAutoConnect",
		  &btd_opts.defaults.le.scan_win_autoconnect,
		  sizeof(btd_opts.defaults.le.scan_win_autoconnect),
		  0x0004,
		  0x4000},
		{ "ScanIntervalSuspend",
		  &btd_opts.defaults.le.scan_interval_suspend,
		  sizeof(btd_opts.defaults.le.scan_interval_suspend),
		  0x0004,
		  0x4000},
		{ "ScanWindowSuspend",
		  &btd_opts.defaults.le.scan_win_suspend,
		  sizeof(btd_opts.defaults.le.scan_win_suspend),
		  0x0004,
		  0x4000},
		{ "ScanIntervalDiscovery",
		  &btd_opts.defaults.le.scan_interval_discovery,
		  sizeof(btd_opts.defaults.le.scan_interval_discovery),
		  0x0004,
		  0x4000},
		{ "ScanWindowDiscovery",
		  &btd_opts.defaults.le.scan_win_discovery,
		  sizeof(btd_opts.defaults.le.scan_win_discovery),
		  0x0004,
		  0x4000},
		{ "ScanIntervalAdvMonitor",
		  &btd_opts.defaults.le.scan_interval_adv_monitor,
		  sizeof(btd_opts.defaults.le.scan_interval_adv_monitor),
		  0x0004,
		  0x4000},
		{ "ScanWindowAdvMonitor",
		  &btd_opts.defaults.le.scan_win_adv_monitor,
		  sizeof(btd_opts.defaults.le.scan_win_adv_monitor),
		  0x0004,
		  0x4000},
		{ "ScanIntervalConnect",
		  &btd_opts.defaults.le.scan_interval_connect,
		  sizeof(btd_opts.defaults.le.scan_interval_connect),
		  0x0004,
		  0x4000},
		{ "ScanWindowConnect",
		  &btd_opts.defaults.le.scan_win_connect,
		  sizeof(btd_opts.defaults.le.scan_win_connect),
		  0x0004,
		  0x4000},
		{ "MinConnectionInterval",
		  &btd_opts.defaults.le.min_conn_interval,
		  sizeof(btd_opts.defaults.le.min_conn_interval),
		  0x0006,
		  0x0C80},
		{ "MaxConnectionInterval",
		  &btd_opts.defaults.le.max_conn_interval,
		  sizeof(btd_opts.defaults.le.max_conn_interval),
		  0x0006,
		  0x0C80},
		{ "ConnectionLatency",
		  &btd_opts.defaults.le.conn_latency,
		  sizeof(btd_opts.defaults.le.conn_latency),
		  0x0000,
		  0x01F3},
		{ "ConnectionSupervisionTimeout",
		  &btd_opts.defaults.le.conn_lsto,
		  sizeof(btd_opts.defaults.le.conn_lsto),
		  0x000A,
		  0x0C80},
		{ "Autoconnecttimeout",
		  &btd_opts.defaults.le.autoconnect_timeout,
		  sizeof(btd_opts.defaults.le.autoconnect_timeout),
		  0x0001,
		  0x4000},
		{ "AdvMonAllowlistScanDuration",
		  &btd_opts.defaults.le.advmon_allowlist_scan_duration,
		  sizeof(btd_opts.defaults.le.advmon_allowlist_scan_duration),
		  1,
		  10000},
		{ "AdvMonNoFilterScanDuration",
		  &btd_opts.defaults.le.advmon_no_filter_scan_duration,
		  sizeof(btd_opts.defaults.le.advmon_no_filter_scan_duration),
		  1,
		  10000},
		{ "EnableAdvMonInterleaveScan",
		  &btd_opts.defaults.le.enable_advmon_interleave_scan,
		  sizeof(btd_opts.defaults.le.enable_advmon_interleave_scan),
		  0,
		  1},
	};

	if (btd_opts.mode == BT_MODE_BREDR)
		return;

	parse_mode_config(config, "LE", params, ARRAY_SIZE(params));
}

static bool match_experimental(const void *data, const void *match_data)
{
	const char *value = data;
	const char *uuid = match_data;

	if (!strcmp(value, "*"))
		return true;

	return !strcasecmp(value, uuid);
}

bool btd_kernel_experimental_enabled(const char *uuid)
{
	if (!btd_opts.kernel)
		false;

	return queue_find(btd_opts.kernel, match_experimental, uuid);
}

static const char *valid_uuids[] = {
	"d4992530-b9ec-469f-ab01-6c481c47da1c",
	"671b10b5-42c0-4696-9227-eb28d1b049d6",
	"15c0a148-c273-11ea-b3de-0242ac130004",
	"330859bc-7506-492d-9370-9a6f0614037f",
	"a6695ace-ee7f-4fb9-881a-5fac66c629af",
	"6fbaf188-05e0-496a-9885-d6ddfdb4e03e",
	"69518c4c-b69f-4679-8bc1-c021b47b5733",
	"*"
};

static void btd_parse_kernel_experimental(char **list)
{
	int i;

	if (btd_opts.kernel) {
		warn("Unable to parse KernelExperimental: list already set");
		return;
	}

	btd_opts.kernel = queue_new();

	for (i = 0; list[i]; i++) {
		size_t j;
		const char *uuid = list[i];

		if (!strcasecmp("false", uuid) || !strcasecmp("off", uuid)) {
			queue_destroy(btd_opts.kernel, free);
			btd_opts.kernel = NULL;
		}

		if (!strcasecmp("true", uuid) || !strcasecmp("on", uuid))
			uuid = "*";

		for (j = 0; j < ARRAY_SIZE(valid_uuids); j++) {
			if (!strcasecmp(valid_uuids[j], uuid))
				break;
		}

		/* Ignored if UUID is considered invalid */
		if (j == ARRAY_SIZE(valid_uuids)) {
			warn("Invalid KernelExperimental UUID: %s", uuid);
			continue;
		}

		DBG("%s", uuid);

		queue_push_tail(btd_opts.kernel, strdup(uuid));
	}
}

static bool gen_sirk(const char *str)
{
	struct bt_crypto *crypto;
	int ret;

	crypto = bt_crypto_new();
	if (!crypto) {
		error("Failed to open crypto");
		return false;
	}

	ret = bt_crypto_sirk(crypto, str, btd_opts.did_vendor,
			   btd_opts.did_product, btd_opts.did_version,
			   btd_opts.did_source, btd_opts.csis.sirk);
	if (!ret)
		error("Failed to generate SIRK");

	bt_crypto_unref(crypto);
	return ret;
}

static bool parse_config_u32(GKeyFile *config, const char *group,
					const char *key, uint32_t *val,
					uint32_t min, uint32_t max)
{
	int tmp;

	if (!parse_config_int(config, group, key, &tmp, min, max))
		return false;

	if (val)
		*val = tmp;

	return true;
}

static bool parse_config_u16(GKeyFile *config, const char *group,
					const char *key, uint16_t *val,
					uint16_t min, uint16_t max)
{
	int tmp;

	if (!parse_config_int(config, group, key, &tmp, min, max))
		return false;

	if (val)
		*val = tmp;

	return true;
}

static bool parse_config_u8(GKeyFile *config, const char *group,
					const char *key, uint8_t *val,
					uint8_t min, uint8_t max)
{
	int tmp;

	if (!parse_config_int(config, group, key, &tmp, min, max))
		return false;

	if (val)
		*val = tmp;

	return true;
}

static bool parse_config_bool(GKeyFile *config, const char *group,
					const char *key, bool *val)
{
	GError *err = NULL;
	gboolean tmp;

	tmp = g_key_file_get_boolean(config, group, key, &err);
	if (err) {
		if (err->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND)
			DBG("%s", err->message);
		g_error_free(err);
		return false;
	}

	DBG("%s.%s = %s", group, key, tmp ? "true" : "false");

	if (val)
		*val = tmp;

	return true;
}

static void parse_privacy(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "General", "Privacy", &str)) {
		btd_opts.privacy = 0x00;
		btd_opts.device_privacy = true;
		return;
	}

	if (!strcmp(str, "network") || !strcmp(str, "on")) {
		btd_opts.privacy = 0x01;
	} else if (!strcmp(str, "device")) {
		btd_opts.privacy = 0x01;
		btd_opts.device_privacy = true;
	} else if (!strcmp(str, "limited-network")) {
		if (btd_opts.mode != BT_MODE_DUAL) {
			DBG("Invalid privacy option: %s", str);
			btd_opts.privacy = 0x00;
		}
		btd_opts.privacy = 0x01;
	} else if (!strcmp(str, "limited-device")) {
		if (btd_opts.mode != BT_MODE_DUAL) {
			DBG("Invalid privacy option: %s", str);
			btd_opts.privacy = 0x00;
		}
		btd_opts.privacy = 0x02;
		btd_opts.device_privacy = true;
	} else if (!strcmp(str, "off")) {
		btd_opts.privacy = 0x00;
		btd_opts.device_privacy = true;
	} else {
		DBG("Invalid privacy option: %s", str);
		btd_opts.privacy = 0x00;
	}

	g_free(str);
}

static void parse_repairing(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "General", "JustWorksRepairing",
						&str)) {
		btd_opts.jw_repairing = JW_REPAIRING_NEVER;
		return;
	}

	btd_opts.jw_repairing = parse_jw_repairing(str);
	g_free(str);
}

static bool parse_config_hex(GKeyFile *config, char *group,
					const char *key, uint32_t *val)
{
	char *str = NULL;

	if (!parse_config_string(config, group, key, &str))
		return false;

	if (val)
		*val = strtol(str, NULL, 16);

	g_free(str);
	return true;
}

static void parse_device_id(GKeyFile *config)
{
	char *str = NULL;

	parse_config_string(config, "General", "DeviceID", &str);
	if (!str)
		return;

	parse_did(str);
	g_free(str);
}

static void parse_ctrl_mode(GKeyFile *config)
{
	char *str = NULL;

	parse_config_string(config, "General", "ControllerMode", &str);
	if (!str)
		return;

	btd_opts.mode = get_mode(str);
	g_free(str);
}

static void parse_multi_profile(GKeyFile *config)
{
	char *str = NULL;

	parse_config_string(config, "General", "MultiProfile", &str);
	if (!str)
		return;

	if (!strcmp(str, "single"))
		btd_opts.mps = MPS_SINGLE;
	else if (!strcmp(str, "multiple"))
		btd_opts.mps = MPS_MULTIPLE;
	else
		btd_opts.mps = MPS_OFF;

	g_free(str);
}

static gboolean parse_kernel_experimental(const char *key, const char *value,
					gpointer user_data, GError **error)
{
	char **strlist;

	if (value && value[0] != '*') {
		strlist = g_strsplit(value, ",", -1);
		btd_parse_kernel_experimental(strlist);
		g_strfreev(strlist);
	} else {
		if (!btd_opts.kernel)
			btd_opts.kernel = queue_new();
		queue_push_head(btd_opts.kernel, strdup("*"));
	}

	return TRUE;
}

static void parse_kernel_exp(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "General", "KernelExperimental",
						&str))
		return;

	parse_kernel_experimental(NULL, str, NULL, NULL);

	g_free(str);
}

static void parse_secure_conns(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "General", "SecureConnections",
								&str))
		return;

	if (!strcmp(str, "off"))
		btd_opts.secure_conn = SC_OFF;
	else if (!strcmp(str, "on"))
		btd_opts.secure_conn = SC_ON;
	else if (!strcmp(str, "only"))
		btd_opts.secure_conn = SC_ONLY;

	g_free(str);
}

static void parse_general(GKeyFile *config)
{
	parse_config_string(config, "General", "Name", &btd_opts.name);
	parse_config_hex(config, "General", "Class", &btd_opts.class);
	parse_config_u32(config, "General", "DiscoverableTimeout",
						&btd_opts.discovto,
						0, UINT32_MAX);
	parse_config_bool(config, "General", "AlwaysPairable",
						&btd_opts.pairable);
	parse_config_u32(config, "General", "PairableTimeout",
						&btd_opts.pairto,
						0, UINT32_MAX);
	parse_device_id(config);
	parse_config_bool(config, "General", "ReverseServiceDiscovery",
						&btd_opts.reverse_discovery);
	parse_config_bool(config, "General", "NameResolving",
						&btd_opts.name_resolv);
	parse_config_bool(config, "General", "DebugKeys",
						&btd_opts.debug_keys);
	parse_ctrl_mode(config);
	parse_config_u16(config, "General", "MaxControllers",
						&btd_opts.max_adapters,
						0, UINT16_MAX);
	parse_multi_profile(config);
	parse_config_bool(config, "General", "FastConnectable",
						&btd_opts.fast_conn);
	parse_privacy(config);
	parse_repairing(config);
	parse_config_u32(config, "General", "TemporaryTimeout",
						&btd_opts.tmpto,
						0, UINT32_MAX);
	parse_config_bool(config, "General", "RefreshDiscovery",
						&btd_opts.refresh_discovery);
	parse_secure_conns(config);
	parse_config_bool(config, "General", "Experimental",
						&btd_opts.experimental);
	parse_kernel_exp(config);
	parse_config_u32(config, "General", "RemoteNameRequestRetryDelay",
					&btd_opts.name_request_retry_delay,
					0, UINT32_MAX);
}

static void parse_gatt_cache(GKeyFile *config)
{
	char *str = NULL;

	parse_config_string(config, "GATT", "Cache", &str);
	if (!str)
		return;

	btd_opts.gatt_cache = parse_gatt_cache_str(str);
	g_free(str);
}

static void parse_gatt(GKeyFile *config)
{
	parse_gatt_cache(config);
	parse_config_u8(config, "GATT", "KeySize", &btd_opts.key_size, 7, 16);
	parse_config_u16(config, "GATT", "ExchangeMTU", &btd_opts.gatt_mtu,
				BT_ATT_DEFAULT_LE_MTU, BT_ATT_MAX_LE_MTU);
	parse_config_u8(config, "GATT", "Channels", &btd_opts.gatt_channels,
				1, 5);
	parse_config_bool(config, "GATT", "Client", &btd_opts.gatt_client);
}

static void parse_csis_sirk(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "CSIS", "SIRK", &str))
		return;

	if (strlen(str) == 32 && check_sirk_alpha_numeric(str))
		hex2bin(str, btd_opts.csis.sirk, sizeof(btd_opts.csis.sirk));
	else if (!gen_sirk(str))
		DBG("Unable to generate SIRK from string");

	g_free(str);
}

static void parse_csis(GKeyFile *config)
{
	parse_csis_sirk(config);
	parse_config_bool(config, "CSIS", "Encryption",
					&btd_opts.csis.encrypt);
	parse_config_u8(config, "CSIS", "Size", &btd_opts.csis.size,
					0, UINT8_MAX);
	parse_config_u8(config, "CSIS", "Rank", &btd_opts.csis.rank,
					0, UINT8_MAX);
}

static void parse_avdtp_session_mode(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "AVDTP", "SessionMode", &str))
		return;

	if (!strcmp(str, "basic"))
		btd_opts.avdtp.session_mode = BT_IO_MODE_BASIC;
	else if (!strcmp(str, "ertm"))
		btd_opts.avdtp.session_mode = BT_IO_MODE_ERTM;
	else {
		DBG("Invalid mode option: %s", str);
		btd_opts.avdtp.session_mode = BT_IO_MODE_BASIC;
	}

	g_free(str);
}

static void parse_avdtp_stream_mode(GKeyFile *config)
{
	char *str = NULL;

	if (!parse_config_string(config, "AVDTP", "StreamMode", &str))
		return;

	if (!strcmp(str, "basic"))
		btd_opts.avdtp.stream_mode = BT_IO_MODE_BASIC;
	else if (!strcmp(str, "streaming"))
		btd_opts.avdtp.stream_mode = BT_IO_MODE_STREAMING;
	else {
		DBG("Invalid mode option: %s", str);
		btd_opts.avdtp.stream_mode = BT_IO_MODE_BASIC;
	}

	g_free(str);
}

static void parse_avdtp(GKeyFile *config)
{
	parse_avdtp_session_mode(config);
	parse_avdtp_stream_mode(config);
}

static void parse_advmon(GKeyFile *config)
{
	parse_config_u8(config, "AdvMon", "RSSISamplingPeriod",
				&btd_opts.advmon.rssi_sampling_period,
				0, UINT8_MAX);
}

static void parse_config(GKeyFile *config)
{
	if (!config)
		return;

	check_config(config);

	DBG("parsing %s", main_conf_file_path);

	/* Parse Groups */
	parse_general(config);
	parse_br_config(config);
	parse_le_config(config);
	parse_gatt(config);
	parse_csis(config);
	parse_avdtp(config);
	parse_advmon(config);
}

static void init_defaults(void)
{
	uint8_t major, minor;

	/* Default HCId settings */
	memset(&btd_opts, 0, sizeof(btd_opts));
	btd_opts.name = g_strdup_printf("BlueZ %s", VERSION);
	btd_opts.class = 0x000000;
	btd_opts.pairto = DEFAULT_PAIRABLE_TIMEOUT;
	btd_opts.discovto = DEFAULT_DISCOVERABLE_TIMEOUT;
	btd_opts.tmpto = DEFAULT_TEMPORARY_TIMEOUT;
	btd_opts.reverse_discovery = TRUE;
	btd_opts.name_resolv = TRUE;
	btd_opts.debug_keys = FALSE;
	btd_opts.refresh_discovery = TRUE;
	btd_opts.name_request_retry_delay = DEFAULT_NAME_REQUEST_RETRY_DELAY;
	btd_opts.secure_conn = SC_ON;

	btd_opts.defaults.num_entries = 0;
	btd_opts.defaults.br.page_scan_type = 0xFFFF;
	btd_opts.defaults.br.scan_type = 0xFFFF;
	btd_opts.defaults.le.enable_advmon_interleave_scan = 0xFF;

	if (sscanf(VERSION, "%hhu.%hhu", &major, &minor) != 2)
		return;

	btd_opts.did_source = 0x0002;		/* USB */
	btd_opts.did_vendor = 0x1d6b;		/* Linux Foundation */
	btd_opts.did_product = 0x0246;		/* BlueZ */
	btd_opts.did_version = (major << 8 | minor);

	btd_opts.gatt_cache = BT_GATT_CACHE_ALWAYS;
	btd_opts.gatt_mtu = BT_ATT_MAX_LE_MTU;
	btd_opts.gatt_channels = 1;
	btd_opts.gatt_client = true;

	btd_opts.avdtp.session_mode = BT_IO_MODE_BASIC;
	btd_opts.avdtp.stream_mode = BT_IO_MODE_BASIC;

	btd_opts.advmon.rssi_sampling_period = 0xFF;
	btd_opts.csis.encrypt = true;
}

static void log_handler(const gchar *log_domain, GLogLevelFlags log_level,
				const gchar *message, gpointer user_data)
{
	int priority;

	if (log_level & (G_LOG_LEVEL_ERROR |
				G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING))
		priority = 0x03;
	else
		priority = 0x06;

	btd_log(0xffff, priority, "GLib: %s", message);
	btd_backtrace(0xffff);
}

void btd_exit(void)
{
	mainloop_quit();
}

static bool quit_eventloop(gpointer user_data)
{
	btd_exit();
	return FALSE;
}

static void signal_callback(int signum, void *user_data)
{
	static bool terminated = false;

	switch (signum) {
	case SIGINT:
	case SIGTERM:
		if (!terminated) {
			info("Terminating");
			timeout_add_seconds(SHUTDOWN_GRACE_SECONDS,
						quit_eventloop, NULL, NULL);

			mainloop_sd_notify("STATUS=Powering down");
			adapter_shutdown();
		}

		terminated = true;
		break;
	case SIGUSR2:
		__btd_toggle_debug();
		break;
	}
}

static char *option_debug = NULL;
static char *option_plugin = NULL;
static char *option_noplugin = NULL;
static char *option_configfile = NULL;
static gboolean option_compat = FALSE;
static gboolean option_detach = TRUE;
static gboolean option_version = FALSE;

static void free_options(void)
{
	g_free(option_debug);
	option_debug = NULL;

	g_free(option_plugin);
	option_plugin = NULL;

	g_free(option_noplugin);
	option_noplugin = NULL;

	g_free(option_configfile);
	option_configfile = NULL;
}

static void disconnect_dbus(void)
{
	DBusConnection *conn = btd_get_dbus_connection();

	if (!conn || !dbus_connection_get_is_connected(conn))
		return;

	g_dbus_detach_object_manager(conn);
	set_dbus_connection(NULL);

	dbus_connection_unref(conn);
}

static void disconnected_dbus(DBusConnection *conn, void *data)
{
	info("Disconnected from D-Bus. Exiting.");
	mainloop_quit();
}

static int connect_dbus(void)
{
	DBusConnection *conn;
	DBusError err;

	dbus_error_init(&err);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, BLUEZ_NAME, &err);
	if (!conn) {
		if (dbus_error_is_set(&err)) {
			g_printerr("D-Bus setup failed: %s\n", err.message);
			dbus_error_free(&err);
			return -EIO;
		}
		return -EALREADY;
	}

	set_dbus_connection(conn);

	g_dbus_set_disconnect_function(conn, disconnected_dbus, NULL, NULL);
	g_dbus_attach_object_manager(conn);

	return 0;
}

static gboolean parse_debug(const char *key, const char *value,
				gpointer user_data, GError **error)
{
	if (value)
		option_debug = g_strdup(value);
	else
		option_debug = g_strdup("*");

	return TRUE;
}

static GOptionEntry options[] = {
	{ "debug", 'd', G_OPTION_FLAG_OPTIONAL_ARG,
				G_OPTION_ARG_CALLBACK, parse_debug,
				"Specify debug options to enable", "DEBUG" },
	{ "plugin", 'p', 0, G_OPTION_ARG_STRING, &option_plugin,
				"Specify plugins to load", "NAME,..," },
	{ "noplugin", 'P', 0, G_OPTION_ARG_STRING, &option_noplugin,
				"Specify plugins not to load", "NAME,..." },
	{ "configfile", 'f', 0, G_OPTION_ARG_STRING, &option_configfile,
			"Specify an explicit path to the config file", "FILE"},
	{ "compat", 'C', 0, G_OPTION_ARG_NONE, &option_compat,
				"Provide deprecated command line interfaces" },
	{ "experimental", 'E', 0, G_OPTION_ARG_NONE, &btd_opts.experimental,
				"Enable experimental D-Bus interfaces" },
	{ "kernel", 'K', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK,
				parse_kernel_experimental,
				"Enable kernel experimental features" },
	{ "nodetach", 'n', G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_detach,
				"Run with logging in foreground" },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version,
				"Show version information and exit" },
	{ NULL },
};

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GError *err = NULL;
	uint16_t sdp_mtu = 0;
	uint32_t sdp_flags = 0;
	int gdbus_flags = 0;

	init_defaults();

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &err) == FALSE) {
		if (err != NULL) {
			g_printerr("%s\n", err->message);
			g_error_free(err);
		} else
			g_printerr("An unknown error occurred\n");
		exit(1);
	}

	g_option_context_free(context);

	if (option_version == TRUE) {
		printf("%s\n", VERSION);
		exit(0);
	}

	umask(0077);

	btd_backtrace_init();

	mainloop_init();

	__btd_log_init(option_debug, option_detach);

	g_log_set_handler("GLib", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL |
							G_LOG_FLAG_RECURSION,
							log_handler, NULL);

	mainloop_sd_notify("STATUS=Starting up");

	main_conf = load_config(option_configfile);

	parse_config(main_conf);

	if (connect_dbus() < 0) {
		error("Unable to get on D-Bus");
		exit(1);
	}

	if (btd_opts.experimental)
		gdbus_flags = G_DBUS_FLAG_ENABLE_EXPERIMENTAL;

	g_dbus_set_flags(gdbus_flags);

	if (adapter_init() < 0) {
		error("Adapter handling initialization failed");
		exit(1);
	}

	btd_device_init();
	btd_agent_init();
	btd_profile_init();

	if (btd_opts.mode != BT_MODE_LE) {
		if (option_compat == TRUE)
			sdp_flags |= SDP_SERVER_COMPAT;

		start_sdp_server(sdp_mtu, sdp_flags);

		if (btd_opts.did_source > 0)
			register_device_id(btd_opts.did_source,
						btd_opts.did_vendor,
						btd_opts.did_product,
						btd_opts.did_version);
	}

	if (btd_opts.mps != MPS_OFF)
		register_mps(btd_opts.mps == MPS_MULTIPLE);

	/* Loading plugins has to be done after D-Bus has been setup since
	 * the plugins might wanna expose some paths on the bus. However the
	 * best order of how to init various subsystems of the Bluetooth
	 * daemon needs to be re-worked. */
	plugin_init(option_plugin, option_noplugin);

	/* no need to keep parsed option in memory */
	free_options();

	rfkill_init();

	DBG("Entering main loop");

	mainloop_sd_notify("STATUS=Running");
	mainloop_sd_notify("READY=1");

	mainloop_run_with_signal(signal_callback, NULL);

	mainloop_sd_notify("STATUS=Quitting");

	plugin_cleanup();

	btd_profile_cleanup();
	btd_agent_cleanup();
	btd_device_cleanup();

	adapter_cleanup();

	rfkill_exit();

	if (btd_opts.mode != BT_MODE_LE)
		stop_sdp_server();

	if (btd_opts.kernel)
		queue_destroy(btd_opts.kernel, free);

	if (main_conf)
		g_key_file_free(main_conf);

	disconnect_dbus();

	info("Exit");

	__btd_log_cleanup();

	return 0;
}
