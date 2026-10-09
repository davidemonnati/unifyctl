CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
BUILD ?= build
UNAME := $(shell uname -s)
CORE = src/export.c src/common.c src/cli.c src/hidpp.c src/devices.c src/discovery.c src/signals.c src/report_queue.c src/transport.c src/operations.c
ifeq ($(UNAME),Linux)
BACKEND = src/discovery_linux.c
CPPFLAGS += $(shell pkg-config --cflags libudev)
LDLIBS += $(shell pkg-config --libs libudev)
else ifeq ($(UNAME),Darwin)
BACKEND = src/discovery_macos.c src/transport_macos.c
# IOKit/CoreFoundation and confstr(_CS_DARWIN_USER_TEMP_DIR) need Darwin APIs
# that strict _POSIX_C_SOURCE would otherwise hide.
CPPFLAGS += -D_DARWIN_C_SOURCE
LDLIBS += -framework IOKit -framework CoreFoundation
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

$(BUILD)/test_transport: src/common.c src/signals.c src/transport.c tests/test_transport.c src/common.h src/signals.h src/transport.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/common.c src/signals.c src/transport.c tests/test_transport.c $(LDFLAGS) -o $@

$(BUILD)/test_export: $(CORE) tests/mock.c tests/test_export.c $(wildcard src/*.h) tests/mock.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter-out src/export.c,$(CORE)) tests/mock.c tests/test_export.c $(LDFLAGS) -o $@

test: all $(BUILD)/test_export $(BUILD)/test_core $(BUILD)/test_operations $(BUILD)/test_transport
	python3 tests/test_cli.py $(BUILD)/unifyctl
	python3 tests/test_export.py $(BUILD)/test_export
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
