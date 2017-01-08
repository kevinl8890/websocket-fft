CC=gcc
AIRSPY_SRCS=libairspy/libairspy/src/*.c
SRCS=$(AIRSPY_SRCS) json/json.c main.c
LIBS= -lfftw3 -lwebsockets -lm -I/usr/include/libusb-1.0/ -lusb-1.0 -lpthread
CFLAGS=-O3 -march=native

BINARY=fft_daemon

daemon: json/json.c main.c
	$(CC) $(SRCS) -o $(BINARY) $(CFLAGS) $(LIBS)
	
.PHONY: clean

clean:
	rm -f $(BINARY)
