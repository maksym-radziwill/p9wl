# p9wl - Wayland compositor for Plan 9
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -O3 -Wall -Wextra -DWLR_USE_UNSTABLE -I. -Isrc
CFLAGS += $(shell pkg-config --cflags wlroots-0.19 wayland-server xkbcommon pixman-1)
LDFLAGS = $(shell pkg-config --libs wlroots-0.19 wayland-server xkbcommon pixman-1)
LDFLAGS += -lpthread -lm -lssl -lcrypto -lfftw3f

# Source files
SRCS = main.c p9/p9.c p9/p9_tls.c input/input.c draw/draw.c draw/compress.c draw/scroll.c draw/send.c \
       input/clipboard.c input/kbmap.c wayland/focus_manager.c wayland/popup.c wayland/toplevel.c wayland/wl_input.c wayland/output.c wayland/client.c \
       draw/phase_correlate.c draw/thread_pool.c
OBJS = $(SRCS:.c=.o)

# Headers
HDRS = types.h p9/p9.h p9/p9_tls.h input/input.h draw/draw.h draw/compress.h draw/scroll.h draw/send.h \
       input/clipboard.h input/kbmap.h wayland/focus_manager.h wayland/popup.h wayland/toplevel.h wayland/wl_input.h wayland/output.h wayland/client.h wayland/wayland.h \
       draw/phase_correlate.h draw/thread_pool.h

TARGET = p9wl

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# For now, build from monolithic file
monolithic: p9wl.c
	$(CC) $(CFLAGS) -o p9wl p9wl.c $(LDFLAGS)
