CC = gcc
BUILD ?= build
# GTK/WebKit is only needed by the desktop app and test_bridge; server and engine tests must build without it.
HAVE_GTK := $(shell pkg-config --exists gtk+-3.0 webkit2gtk-4.1 && echo 1)
ifeq ($(HAVE_GTK),1)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 webkit2gtk-4.1)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 webkit2gtk-4.1)
endif
CPPFLAGS += -I. $(shell pkg-config --cflags json-c sqlite3 libsodium)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS += $(shell pkg-config --libs json-c sqlite3 libsodium) -pthread -lm

shared := $(wildcard shared/*.c)
server_dirs := server server/auth server/game server/lobby server/storage
modules := $(filter-out server/main.c,$(wildcard $(addsuffix /*.c,$(server_dirs))))
desktop_modules := $(filter-out desktop/main.c,$(wildcard desktop/*.c))
headers := $(wildcard shared/*.h $(addsuffix /*.h,$(server_dirs)) desktop/*.h)
ifeq ($(origin NAME),environment)
NAME :=
endif
gtk_test_names := bridge
test_dirs := tests/unit
test_sources := $(wildcard $(addsuffix /test_*.c,$(test_dirs)))
test_names := $(patsubst test_%.c,%,$(notdir $(test_sources)))
vpath test_%.c $(test_dirs)
ifneq ($(HAVE_GTK),1)
test_names := $(filter-out $(gtk_test_names),$(test_names))
endif
tests := $(addprefix $(BUILD)/tests/test_,$(if $(NAME),$(NAME),$(test_names)))

.PHONY: all server desktop desktop-smoke test integration sanitize package
all: $(addprefix $(BUILD)/tests/test_,$(test_names)) $(if $(wildcard server/main.c),server) $(if $(and $(HAVE_GTK),$(wildcard desktop/main.c)),desktop)

server: $(BUILD)/uno-server
desktop: $(BUILD)/uno-desktop

$(BUILD)/uno-server: server/main.c $(modules) $(shared) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/uno-desktop: desktop/main.c $(desktop_modules) $(shared) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) $(GTK_LIBS) -o $@

$(BUILD)/tests/test_bridge: tests/unit/test_bridge.c $(desktop_modules) $(shared) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) $(GTK_LIBS) -o $@

$(BUILD)/tests/test_net: desktop/tcp_client.c
$(BUILD)/tests/test_net_session: desktop/net_session.c

$(BUILD)/tests/test_%: test_%.c $(shared) $(modules) $(headers) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -o $@

test: $(tests)
	@set -e; for t in $(tests); do "$$t"; done

desktop-smoke: server desktop
	@set -e; \
	SERVER_LOG=$$(mktemp); \
	./$(BUILD)/uno-server --port 0 --bind 127.0.0.1 --db :memory: > "$$SERVER_LOG" 2>&1 & \
	SERVER_PID=$$!; \
	trap 'kill $$SERVER_PID 2>/dev/null || true; rm -f "$$SERVER_LOG"' EXIT INT TERM; \
	PORT=""; \
	for i in $$(seq 1 50); do \
		PORT=$$(grep -o 'PORT [0-9]*' "$$SERVER_LOG" 2>/dev/null | awk '{print $$2}' || true); \
		if [ -n "$$PORT" ]; then break; fi; \
		sleep 0.1; \
	done; \
	if [ -z "$$PORT" ]; then \
		echo "Failed to start server"; \
		cat "$$SERVER_LOG"; \
		exit 1; \
	fi; \
	RUN_DESKTOP="./$(BUILD)/uno-desktop --port $$PORT --timeout 5 --assets tests/fixtures"; \
	if [ -z "$$DISPLAY" ]; then \
		xvfb-run -a $$RUN_DESKTOP; \
	else \
		$$RUN_DESKTOP; \
	fi; \
	echo "desktop-smoke: OK"; \
	kill $$SERVER_PID 2>/dev/null || true; \
	rm -f "$$SERVER_LOG"; \
	trap - EXIT INT TERM

integration: server
	python3 -m unittest $(if $(NAME),tests.integration.test_$(NAME) -v,discover -s tests/integration -v)

sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize CFLAGS="$(CFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" test

package: server desktop
	tar -czf $(BUILD)/uno-ubuntu.tar.gz $(BUILD)/uno-server $(BUILD)/uno-desktop client/dist docs/ubuntu-demo.md
