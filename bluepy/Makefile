BLUEZ_PATH=./bluez-5.47

BLUEZ_SRCS  = lib/bluetooth.c lib/hci.c lib/sdp.c lib/uuid.c
BLUEZ_SRCS += attrib/att.c attrib/gatt.c attrib/gattrib.c attrib/utils.c
BLUEZ_SRCS += btio/btio.c src/log.c src/shared/mgmt.c
BLUEZ_SRCS += src/shared/crypto.c src/shared/att.c src/shared/queue.c src/shared/util.c
BLUEZ_SRCS += src/shared/io-glib.c src/shared/timeout-glib.c

IMPORT_SRCS = $(addprefix $(BLUEZ_PATH)/, $(BLUEZ_SRCS))
LOCAL_SRCS  = bluepy-helper.c

CC ?= gcc
CFLAGS += -g -Wall # -Werror

CPPFLAGS += -DHAVE_CONFIG_H
ifneq ($(DEBUGGING),)
CFLAGS += -DBLUEPY_DEBUG=1 -O0
else
CFLAGS += -Os
endif

CPPFLAGS += -I$(BLUEZ_PATH)/attrib -I$(BLUEZ_PATH) -I$(BLUEZ_PATH)/lib -I$(BLUEZ_PATH)/src -I$(BLUEZ_PATH)/gdbus -I$(BLUEZ_PATH)/btio -I$(BLUEZ_PATH)/sys

CPPFLAGS += $(shell pkg-config glib-2.0 --cflags)
LDLIBS += $(shell pkg-config glib-2.0 --libs)

all: bluepy-helper

bluepy-helper: $(LOCAL_SRCS) $(IMPORT_SRCS)
	$(CC) -L. $(CFLAGS) $(CPPFLAGS) -o $@ $(LOCAL_SRCS) $(IMPORT_SRCS) $(LDLIBS)

$(IMPORT_SRCS): bluez-src.tgz
	tar xzf $<
	touch $(IMPORT_SRCS)

.PHONY: bluez-tarfile

bluez-tarfile:
	(cd ..; tar czf bluepy/bluez-src.tgz $(BLUEZ_PATH))

GET_SERVICES=get_services.py

uuids.json: $(GET_SERVICES)
	python $(GET_SERVICES) > uuids.json

TAGS: *.c $(BLUEZ_PATH)/attrib/*.[ch] $(BLUEZ_PATH)/btio/*.[ch]
	etags $^

clean:
	rm -rf *.o bluepy-helper TAGS $(BLUEZ_PATH)



