BLUEZ_PATH=./bluez-5.47

BLUEZ_SRCS  = lib/bluetooth.c lib/hci.c lib/sdp.c lib/uuid.c
BLUEZ_SRCS += attrib/att.c attrib/gatt.c attrib/gattrib.c attrib/utils.c
BLUEZ_SRCS += btio/btio.c src/log.c src/shared/mgmt.c
BLUEZ_SRCS += src/shared/crypto.c src/shared/att.c src/shared/queue.c src/shared/util.c
BLUEZ_SRCS += src/shared/io-glib.c src/shared/timeout-glib.c

IMPORT_SRCS = $(addprefix $(BLUEZ_PATH)/, $(BLUEZ_SRCS))
LOCAL_SRCS  = bluepy-helper.c
VERSION_SRCS = version.c

IMPORT_OBJS = $(IMPORT_SRCS:.c=.o)
LOCAL_OBJS  = $(LOCAL_SRCS:.c=.o)
VERSION_OBJS = $(VERSION_SRCS:.c=.o)

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

# Generate the "version.c" based on the python package version
# and GIT revision (if available).
#
# The 'grep|sed|tr' extract the version string from the 'setup.py' file.
# The 'git describe' extract the git version (if there is a .git directory).
# The output of the above is a single string like so:
#        1.2.0 (v/1.2.0-16-g3ce360c-dirty)
# The top and bottom printf's generate valid C code, like so:
#    const char* bluepy_helper_version = "1.2.0 (v/1.2.0-16-g3ce360c-dirty)";
version.c: ../setup.py $(LOCAL_SRCS) $(IMPORT_SRCS)
	@echo Regenerating $@
	@( \
	  printf "const char* bluepy_helper_version = \"" ; \
	  \
	  grep "version=" ../setup.py \
	        | sed -e "s/[^0-9]*\([0-9\.]*\).*/\1/" \
		| tr -d '\n' ; \
	  \
	  test -d ../.git \
	        && { printf " (" ; \
		     git describe --always --dirty | tr -d '\n' ; \
		     printf ")" ; } ; \
          \
	  printf '";\n' ) > $@

bluepy-helper: $(LOCAL_OBJS) $(IMPORT_OBJS) $(VERSION_OBJS)
	$(CC) -L. -o $@ $^ $(LDLIBS)

# NOTE:
# make's built-in rule for compiling C to obj files is sufficient
# to build the objects, no need for explicit rule.

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
	rm -rf $(VERSION_OBJS) $(VERSION_SRCS) $(LOCAL_OBJS) $(IMPORT_OBJS) bluepy-helper TAGS $(BLUEZ_PATH)



