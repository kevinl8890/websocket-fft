CC=gcc
SRCS=json/json.c main.c
LIBS=-lwebsockets -lm -lrtlsdr -lfftw3
CFLAGS=-O3

BINARY=fft_daemon

daemon: json/json.c main.c
	$(CC) $(SRCS) -o $(BINARY) $(CFLAGS) $(LIBS)
	
.PHONY: clean

clean:
	rm -f $(BINARY)
