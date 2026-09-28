CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
BUILD ?= build
CORE = src/common.c src/cli.c src/hidpp.c src/devices.c src/discovery.c src/transport.c src/operations.c
ifeq ($(shell uname -s),Linux)
BACKEND = src/discovery_linux.c
CPPFLAGS += $(shell pkg-config --cflags libudev)
LDLIBS += $(shell pkg-config --libs libudev)
else
BACKEND = src/discovery_stub.c
endif

.PHONY: all test sanitize clean install
all: $(BUILD)/unifyctl

$(BUILD):
	mkdir -p $@

$(BUILD)/unifyctl: $(CORE) $(BACKEND) src/main.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) $(BACKEND) src/main.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test_core: $(CORE) tests/mock.c tests/test_core.c $(wildcard src/*.h) tests/mock.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/mock.c tests/test_core.c $(LDFLAGS) -o $@

$(BUILD)/test_operations: $(CORE) tests/mock.c tests/test_operations.c $(wildcard src/*.h) tests/mock.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/mock.c tests/test_operations.c $(LDFLAGS) -o $@

$(BUILD)/test_transport: src/common.c src/transport.c tests/test_transport.c src/common.h src/transport.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/common.c src/transport.c tests/test_transport.c $(LDFLAGS) -o $@

test: all $(BUILD)/test_core $(BUILD)/test_operations $(BUILD)/test_transport
	python3 tests/test_cli.py $(BUILD)/unifyctl
	$(BUILD)/test_core
	$(BUILD)/test_operations
	$(BUILD)/test_transport

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

clean:
	rm -rf build

PREFIX ?= /usr/local
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/unifyctl $(DESTDIR)$(PREFIX)/bin/unifyctl
