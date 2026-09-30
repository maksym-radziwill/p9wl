CC = gcc
CFLAGS = -O3 -Wall -Wextra -DWLR_USE_UNSTABLE -Isrc -I. -include sys/types.h -include sys/select.h -include unistd.h
CFLAGS += $(shell pkg-config --cflags wlroots-0.19 wayland-server xkbcommon pixman-1 freerdp3 winpr3)
CFLAGS += -g -O0

LDFLAGS = $(shell pkg-config --libs wlroots-0.19 wayland-server xkbcommon pixman-1 freerdp3 winpr3)
LDFLAGS += -lpthread -lm -lssl -lcrypto -lfftw3f

SRCS = src/main.c \
       src/input.c \
       src/keymap.c \
       src/draw.c \
       src/draw_cmd.c \
       src/compress.c \
       src/scroll.c \
       src/send.c \
       src/clipboard.c \
       src/focus_manager.c \
       src/popup.c \
       src/toplevel.c \
       src/wl_input.c \
       src/output.c \
       src/client.c \
       src/phase_correlate.c \
       src/parallel.c \
       src/tls.c

OBJS = $(SRCS:.c=.o)
HDRS = src/types.h xdg-shell-protocol.h
TARGET = p9wl-rdp-alpine

.PHONY: all clean

all: xdg-shell-protocol.h $(TARGET)

xdg-shell-protocol.h:
	WAYLAND_PROTOCOLS_DIR=$$(pkg-config --variable=pkgdatadir wayland-protocols); \
	wayland-scanner server-header $$WAYLAND_PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.h

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) xdg-shell-protocol.h