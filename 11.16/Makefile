CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

TARGETS = rsu_server obu rsu_server.exe obu.exe

all: $(TARGETS)

rsu_server: rsu_server.c v2x_protocol.h
	$(CC) $(CFLAGS) -o $@ $<

obu: obu.c v2x_protocol.h
	$(CC) $(CFLAGS) -o $@ $<

rsu_server.exe: rsu_server
	cp $< $@

obu.exe: obu
	cp $< $@

clean:
	rm -f $(TARGETS)

.PHONY: all clean
