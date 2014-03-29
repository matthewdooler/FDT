CC = gcc
UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
	CFLAGS = -std=c99 -g `pkg-config --cflags gtk+-2.0`
	LIBS = -ldl -lpthread -lm `pkg-config --cflags --libs gtk+-2.0`
else
        CFLAGS = -std=c99 -g `jhbuild run pkg-config --cflags gtk+-2.0`
        LIBS = -ldl -lpthread -lm `jhbuild run pkg-config --cflags --libs gtk+-2.0`
endif

all: fdt

clean:
	rm -f fdt *.o
	rm -f FUSE\ Diagnostic\ Tool.app/Contents/MacOS/fdt
	rm -rf FUSE\ Diagnostic\ Tool.app/Contents/MacOS/*.png
	rm -rf FUSE\ Diagnostic\ Tool.app/Contents/MacOS/osxfuse

build: all
	cp fdt FUSE\ Diagnostic\ Tool.app/Contents/MacOS/fdt
	cp *.png FUSE\ Diagnostic\ Tool.app/Contents/MacOS/
	cp resources.json FUSE\ Diagnostic\ Tool.app/Contents/MacOS/
	mkdir -p FUSE\ Diagnostic\ Tool.app/Contents/MacOS/osxfuse/fuse && cp osxfuse/fuse/fsigs.json FUSE\ Diagnostic\ Tool.app/Contents/MacOS/osxfuse/fuse/
	mkdir -p FUSE\ Diagnostic\ Tool.app/Contents/MacOS/osxfuse/out/osxfuse-core-10.7-2.6.1/osxfuse/usr/local/lib && cp -Ra osxfuse/out/osxfuse-core-10.7-2.6.1/osxfuse/usr/local/lib FUSE\ Diagnostic\ Tool.app/Contents/MacOS/osxfuse/out/osxfuse-core-10.7-2.6.1/osxfuse/usr/local

# Compile to object files
fdt.o: fdt.c fdt.h
	$(CC) $(CFLAGS) fdt.c -c -o fdt.o

wizard.o: wizard.c wizard.h
	$(CC) $(CFLAGS) wizard.c -c -o wizard.o

testsuite.o: testsuite.c testsuite.h
	$(CC) $(CFLAGS) testsuite.c -c -o testsuite.o

debugger.o: debugger.c debugger.h
	$(CC) $(CFLAGS) debugger.c -c -o debugger.o

logger.o: logger.c logger.h
	$(CC) $(CFLAGS) logger.c -c -o logger.o

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) cJSON.c -c -o cJSON.o

# Link object files to executables
fdt: fdt.o wizard.o testsuite.o debugger.o logger.o cJSON.o
	$(CC) $(CFLAGS) $(LIBS) fdt.o wizard.o testsuite.o debugger.o logger.o cJSON.o -o fdt
