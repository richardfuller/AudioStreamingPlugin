CC_ARM  ?= aarch64-linux-gnu-gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -fPIC -Isrc
ID      := audio-stream

all: dist/$(ID)-module.tar.gz

dist/$(ID)-module.tar.gz: src/audio_stream.c module.json scripts/build.py scripts/package.py
	python3 scripts/build.py

build/host/$(ID).so: src/audio_stream.c
	@mkdir -p build/host
	$(CC) $(CFLAGS) -shared -o $@ $< -lpthread

test: build/host/$(ID).so test/host_test.c
	$(CC) -O1 -Wall -Isrc -o build/host/host_test test/host_test.c -ldl -lpthread -lm
	build/host/host_test build/host/$(ID).so

clean:
	rm -rf build dist
.PHONY: all test clean
