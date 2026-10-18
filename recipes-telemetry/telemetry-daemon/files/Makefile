# telemetry_daemon — multithreaded skeleton (Phase 7)
# Each thread lives in its own .c/.h pair, kept independent of this Makefile
# EXCEPT when a thread's real implementation needs an external library —
# Phase 10 (mqtt_thread.c, Paho MQTT C) was the first one to need that, via
# -lpaho-mqtt3c below. Future phases needing their own library (e.g. Phase 9's
# libmodbus) will need the same kind of addition here.

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -pthread -lpaho-mqtt3c -lmodbus -lsqlite3 -lm

SRCS = telemetry_daemon.c logging.c core3_monitor.c can_thread.c modbus_thread.c mqtt_thread.c fdd_thread.c system_health.c status_writer.c
OBJS = $(SRCS:.c=.o)
BIN  = telemetry_daemon

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
