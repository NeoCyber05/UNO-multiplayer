CC = gcc
BUILD ?= build
CPPFLAGS += -I. -Ishared $(shell pkg-config --cflags json-c)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell pkg-config --libs json-c)

shared := $(wildcard shared/*.c)
modules := $(filter-out server/main.c,$(wildcard server/*.c))
headers := $(wildcard shared/*.h server/*.h desktop/*.h)
test_names := $(patsubst tests/test_%.c,%,$(wildcard tests/test_*.c))
tests := $(addprefix $(BUILD)/tests/test_,$(if $(NAME),$(NAME),$(test_names)))

.PHONY: all server desktop test integration sanitize package
all: $(patsubst tests/test_%.c,$(BUILD)/tests/test_%,$(wildcard tests/test_*.c)) $(if $(wildcard server/main.c),server) $(if $(wildcard desktop/main.c),desktop)

server: $(BUILD)/uno-server
desktop: $(BUILD)/uno-desktop

$(BUILD)/uno-server: server/main.c $(modules) $(shared) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/uno-desktop: desktop/main.c $(filter-out desktop/main.c,$(wildcard desktop/*.c)) $(shared) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/tests/test_%: tests/test_%.c $(shared) $(modules) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -o $@

test: $(tests)
	@set -e; for t in $(tests); do "$$t"; done

integration: server
	python3 -m unittest $(if $(NAME),tests.integration.test_$(NAME) -v,discover -s tests/integration -v)

sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize CFLAGS="$(CFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" test

package: server desktop
	tar -czf $(BUILD)/uno-ubuntu.tar.gz $(BUILD)/uno-server $(BUILD)/uno-desktop client/dist docs/ubuntu-demo.md
