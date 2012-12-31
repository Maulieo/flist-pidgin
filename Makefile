#Customisable stuff here
LINUX_COMPILER = gcc

PIDGIN_CFLAGS = `pkg-config pidgin --cflags --libs`
LIBPURPLE_CFLAGS = -DPURPLE_PLUGINS -DENABLE_NLS -DHAVE_ZLIB
GLIB_CFLAGS = -I/usr/include/json-glib-1.0 -ljson-glib-1.0

PIDGIN_DIR = /usr/lib/purple-2/

FLIST_SOURCES = \
        f-list.c \
		f-list_admin.c \
        f-list_autobuddy.c \
        f-list_bbcode.c \
        f-list_callbacks.c \
        f-list_channels.c \
        f-list_commands.c \
        f-list_connection.c \
        f-list_http.c \
        f-list_icon.c \
        f-list_kinks.c \
        f-list_profile.c \
	f-list_pidgin.c

#Standard stuff here
.PHONY:	all clean install

all: 	flist.so

clean:
	rm -f flist.so
	
install: 
	cp flist.so ${PIDGIN_DIR}
	
flist.so:	${FLIST_SOURCES}
	${LINUX_COMPILER} -Wall -I. -g -O2 -pipe ${FLIST_SOURCES} -o $@ -shared -fPIC ${LIBPURPLE_CFLAGS} ${PIDGIN_CFLAGS} ${GLIB_CFLAGS}


