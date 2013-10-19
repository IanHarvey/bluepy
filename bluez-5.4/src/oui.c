/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
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

#include "oui.h"

#ifdef HAVE_UDEV_HWDB_NEW
#include <libudev.h>

char *batocomp(const bdaddr_t *ba)
{
	struct udev *udev;
	struct udev_hwdb *hwdb;
	struct udev_list_entry *head, *entry;
	char modalias[11], *comp = NULL;

	sprintf(modalias, "OUI:%2.2X%2.2X%2.2X", ba->b[5], ba->b[4], ba->b[3]);

	udev = udev_new();
	if (!udev)
		return NULL;

	hwdb = udev_hwdb_new(udev);
	if (!hwdb)
		goto done;

	head = udev_hwdb_get_properties_list_entry(hwdb, modalias, 0);

	udev_list_entry_foreach(entry, head) {
		const char *name = udev_list_entry_get_name(entry);

		if (name && !strcmp(name, "ID_OUI_FROM_DATABASE")) {
			comp = strdup(udev_list_entry_get_value(entry));
			break;
		}
	}

	hwdb = udev_hwdb_unref(hwdb);

done:
	udev = udev_unref(udev);

	return comp;
}
#else
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* http://standards.ieee.org/regauth/oui/oui.txt */

#ifndef OUIFILE
#define OUIFILE "/usr/share/hwdata/oui.txt"
#endif

static char *ouitocomp(const char *oui)
{
	struct stat st;
	char *str, *map, *off, *end;
	int fd;

	fd = open(OUIFILE, O_RDONLY);
	if (fd < 0)
		return NULL;

	if (fstat(fd, &st) < 0) {
		close(fd);
		return NULL;
	}

	str = malloc(128);
	if (!str) {
		close(fd);
		return NULL;
	}

	memset(str, 0, 128);

	map = mmap(0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (!map || map == MAP_FAILED) {
		free(str);
		close(fd);
		return NULL;
	}

	off = strstr(map, oui);
	if (off) {
		off += 18;
		end = strpbrk(off, "\r\n");
		strncpy(str, off, end - off);
	} else {
		free(str);
		str = NULL;
	}

	munmap(map, st.st_size);

	close(fd);

	return str;
}

char *batocomp(const bdaddr_t *ba)
{
	char oui[9];

	sprintf(oui, "%2.2X-%2.2X-%2.2X", ba->b[5], ba->b[4], ba->b[3]);

	return ouitocomp(oui);
}
#endif
