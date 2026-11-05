CC_ARM  ?= aarch64-linux-gnu-gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -fPIC -Isrc
ID      := audio-stream

all: dist/$(ID)-module.tar.gz

dist/$(ID)-module.tar.gz: src/audio_stream.c module.json scripts/build.py scripts/package.py
	python3 scripts/build.py

TAPDEF := -DAUDIO_LIVE_SHM_PATH='"$(CURDIR)/build/host/fake-live"'

build/host/$(ID).so: src/audio_stream.c
	@mkdir -p build/host
	$(CC) $(CFLAGS) $(TAPDEF) -shared -o $@ $< -lpthread

test: build/host/$(ID).so test/host_test.c
	$(CC) -O1 -Wall -Isrc $(TAPDEF) -o build/host/host_test test/host_test.c -ldl -lpthread -lm
	build/host/host_test build/host/$(ID).so

check-glibc: dist/$(ID)-module.tar.gz
	@objdump -T build/arm/$(ID).so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | tee /dev/stderr | grep -qx 'GLIBC_2.17' && echo 'glibc: OK (<= 2.17)'

clean:
	rm -rf build dist
.PHONY: all test clean check-glibc
