CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
TARGETS = rsu_server obu

all: $(TARGETS)

rsu_server: rsu_server.c common.h
	$(CC) $(CFLAGS) -o rsu_server rsu_server.c

obu: obu.c common.h
	$(CC) $(CFLAGS) -o obu obu.c

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean
