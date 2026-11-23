CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
TARGETS = rsu_server obu_car01 obu_car02

all: $(TARGETS)

rsu_server: rsu_server.c common.h
	$(CC) $(CFLAGS) -o rsu_server rsu_server.c

obu_car01: obu_car01.c common.h
	$(CC) $(CFLAGS) -o obu_car01 obu_car01.c

obu_car02: obu_car02.c common.h
	$(CC) $(CFLAGS) -o obu_car02 obu_car02.c

clean:
	rm -f $(TARGETS) obu *.o

.PHONY: all clean
