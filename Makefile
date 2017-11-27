CC=gcc
# CC=clang
RM=rm -f
CFLAGS=-Wall -Ofast -I/opt/local/include
# CFLAGS=-Wall -g -I/opt/local/include
# DEFS=
LDFLAGS=-L/opt/local/lib -lpthread

# Uncomment under Win32 (CYGWIN/MinGW):
# EXE=.exe

dump868=dump868$(EXE)

all: $(dump868)
	strip $(dump868)

$(dump868): dump868.o lib_crc.o net_io.o anet.o util.o
	$(CC) ${LDFLAGS} -o $(dump868) dump868.o lib_crc.o net_io.o anet.o util.o -lm

lib_crc.o: lib_crc.h

dump868.o: dump868.h

net_io.o: net_io.h

anet.o: anet.h

util.o: util.h

.c.o:
	$(CC) ${CFLAGS} ${DEFS} -c $*.c

clean:
	$(RM) $(NRF905_DEMOD) $(dump868) *.o core
