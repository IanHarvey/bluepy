// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>

#include "lib/bluetooth.h"

#include "btio/btio.h"
#include "src/plugin.h"
#include "src/log.h"
#include "src/btd.h"

#define IS_ENABLED(x) (x)

static GSList *plugins = NULL;

struct bluetooth_plugin {
	void *handle;
	const struct bluetooth_plugin_desc *desc;
};

static int compare_priority(gconstpointer a, gconstpointer b)
{
	const struct bluetooth_plugin *plugin1 = a;
	const struct bluetooth_plugin *plugin2 = b;

	return plugin2->desc->priority - plugin1->desc->priority;
}

static int init_plugin(const struct bluetooth_plugin_desc *desc)
{
	int err;

	err = desc->init();
	if (err < 0) {
		if (err == -ENOSYS || err == -ENOTSUP)
			warn("System does not support %s plugin",
						desc->name);
		else
			error("Failed to init %s plugin",
						desc->name);
	}
	return err;
}

static gboolean add_external_plugin(void *handle,
				const struct bluetooth_plugin_desc *desc)
{
	struct bluetooth_plugin *plugin;

	if (desc->init == NULL)
		return FALSE;

	if (g_str_equal(desc->version, VERSION) == FALSE) {
		error("Version mismatch for %s", desc->name);
		return FALSE;
	}

	plugin = g_try_new0(struct bluetooth_plugin, 1);
	if (plugin == NULL)
		return FALSE;

	plugin->handle = handle;
	plugin->desc = desc;

	if (init_plugin(desc) < 0) {
		g_free(plugin);
		return FALSE;
	}

	__btd_enable_debug(desc->debug_start, desc->debug_stop);

	plugins = g_slist_insert_sorted(plugins, plugin, compare_priority);
	DBG("Plugin %s loaded", desc->name);

	return TRUE;
}

static void add_plugin(const struct bluetooth_plugin_desc *desc)
{
	struct bluetooth_plugin *plugin;

	DBG("Loading %s plugin", desc->name);

	plugin = g_try_new0(struct bluetooth_plugin, 1);
	if (plugin == NULL)
		return;

	plugin->desc = desc;

	if (init_plugin(desc) < 0) {
		g_free(plugin);
		return;
	}

	plugins = g_slist_insert_sorted(plugins, plugin, compare_priority);
	DBG("Plugin %s loaded", desc->name);
}

static gboolean enable_plugin(const char *name, char **cli_enable,
							char **cli_disable)
{
	if (cli_disable) {
		for (; *cli_disable; cli_disable++)
			if (g_pattern_match_simple(*cli_disable, name))
				break;
		if (*cli_disable) {
			info("Excluding (cli) %s", name);
			return FALSE;
		}
	}

	if (cli_enable) {
		for (; *cli_enable; cli_enable++)
			if (g_pattern_match_simple(*cli_enable, name))
				break;
		if (!*cli_enable) {
			info("Ignoring (cli) %s", name);
			return FALSE;
		}
	}

	return TRUE;
}


static void external_plugin_init(char **cli_disabled, char **cli_enabled)
{
	GDir *dir;
	const char *file;

	info("Using external plugins is not officially supported.\n");
	info("Consider upstreaming your plugins into the BlueZ project.");

	if (strlen(PLUGINDIR) == 0)
		return;

	DBG("Loading plugins %s", PLUGINDIR);

	dir = g_dir_open(PLUGINDIR, 0, NULL);
	if (!dir)
		return;

	while ((file = g_dir_read_name(dir)) != NULL) {
		const struct bluetooth_plugin_desc *desc;
		void *handle;
		char *filename;

		if (g_str_has_prefix(file, "lib") == TRUE ||
				g_str_has_suffix(file, ".so") == FALSE)
			continue;

		filename = g_build_filename(PLUGINDIR, file, NULL);

		handle = dlopen(filename, RTLD_NOW);
		if (handle == NULL) {
			error("Can't load plugin %s: %s", filename,
								dlerror());
			g_free(filename);
			continue;
		}

		g_free(filename);

		desc = dlsym(handle, "bluetooth_plugin_desc");
		if (desc == NULL) {
			error("Can't load plugin description: %s", dlerror());
			dlclose(handle);
			continue;
		}

		if (!enable_plugin(desc->name, cli_enabled, cli_disabled)) {
			dlclose(handle);
			continue;
		}

		if (add_external_plugin(handle, desc) == FALSE)
			dlclose(handle);
	}

	g_dir_close(dir);
}

#include "src/builtin.h"

void plugin_init(const char *enable, const char *disable)
{
	char **cli_disabled = NULL;
	char **cli_enabled = NULL;
	unsigned int i;

	/* Make a call to BtIO API so its symbols got resolved before the
	 * plugins are loaded. */
	bt_io_error_quark();

	if (enable)
		cli_enabled = g_strsplit_set(enable, ", ", -1);

	if (disable)
		cli_disabled = g_strsplit_set(disable, ", ", -1);

	DBG("Loading builtin plugins");

	for (i = 0; __bluetooth_builtin[i]; i++) {
		if (!enable_plugin(__bluetooth_builtin[i]->name, cli_enabled,
								cli_disabled))
			continue;

		add_plugin(__bluetooth_builtin[i]);
	}

	if IS_ENABLED(EXTERNAL_PLUGINS)
		external_plugin_init(cli_enabled, cli_disabled);

	g_strfreev(cli_enabled);
	g_strfreev(cli_disabled);
}

void plugin_cleanup(void)
{
	GSList *list;

	DBG("Cleanup plugins");

	for (list = plugins; list; list = list->next) {
		struct bluetooth_plugin *plugin = list->data;

		if (plugin->desc->exit)
			plugin->desc->exit();

		if (plugin->handle != NULL)
			dlclose(plugin->handle);

		g_free(plugin);
	}

	g_slist_free(plugins);
}
