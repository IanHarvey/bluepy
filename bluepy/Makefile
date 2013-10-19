BLUEZ_PATH=../bluez-5.4

BLUEZ_SRCS  = lib/bluetooth.c lib/hci.c lib/sdp.c lib/uuid.c
BLUEZ_SRCS += attrib/att.c attrib/gatt.c attrib/gattrib.c attrib/utils.c
BLUEZ_SRCS += btio/btio.c src/log.c

IMPORT_SRCS = $(addprefix $(BLUEZ_PATH)/, $(BLUEZ_SRCS))
LOCAL_SRCS  = bluepy-helper.c

CC = gcc
CFLAGS = -O0 -g

CPPFLAGS = -DHAVE_CONFIG_H

#ARCH=i386-linux-gnu
ARCH=arm-linux-gnueabihf

CPPFLAGS +=  -I/usr/include/dbus-1.0 -I/usr/lib/$(ARCH)/dbus-1.0/include
CPPFLAGS += -I/usr/include/glib-2.0 -I/usr/lib/$(ARCH)/glib-2.0/include
CPPFLAGS += -I$(BLUEZ_PATH)/attrib -I$(BLUEZ_PATH) -I$(BLUEZ_PATH)/lib -I$(BLUEZ_PATH)/src -I$(BLUEZ_PATH)/gdbus -I$(BLUEZ_PATH)/btio

LDLIBS = -lglib-2.0

all: bluepy-helper 

bluepy-helper: $(LOCAL_SRCS) $(IMPORT_SRCS)
	$(CC) -L. $(CFLAGS) $(CPPFLAGS) -o $@ $(LOCAL_SRCS) $(IMPORT_SRCS) $(LDLIBS)

clean:
	rm -f *.o bluepy-helper

