PREFIX ?= $(HOME)/.local
BUILD := build
CC := gcc
MINGW := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra

.PHONY: all install clean

all: \
	$(BUILD)/ni-s49-usb \
	$(BUILD)/ni-nks-bridge \
	$(BUILD)/winusb.dll \
	$(BUILD)/pnputil.exe \
	$(BUILD)/s49-iface.exe \
	$(BUILD)/ni-afunix-hook.dll \
	$(BUILD)/version.dll

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ni-s49-usb: src/ni-s49-usb.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< -lusb-1.0 -lpthread

$(BUILD)/ni-nks-bridge: src/ni-nks-bridge.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/winusb.dll: src/winusb.c src/winusb.def | $(BUILD)
	$(MINGW) $(CFLAGS) -shared -o $@ src/winusb.c src/winusb.def -lws2_32

$(BUILD)/pnputil.exe: src/pnputil.c | $(BUILD)
	$(MINGW) $(CFLAGS) -o $@ $<

$(BUILD)/s49-iface.exe: src/s49-iface.c | $(BUILD)
	$(MINGW) $(CFLAGS) -Wno-cast-function-type -municode -o $@ $< -lsetupapi

$(BUILD)/ni-afunix-hook.dll: src/ni-afunix-hook.c | $(BUILD)
	$(MINGW) $(CFLAGS) -Wno-cast-function-type -shared -o $@ $< -lws2_32

$(BUILD)/version.dll: src/ni-version-proxy.c src/ni-version-proxy.def | $(BUILD)
	$(MINGW) $(CFLAGS) -Wno-cast-function-type -shared -o $@ src/ni-version-proxy.c src/ni-version-proxy.def -lkernel32

install: all
	install -d $(PREFIX)/bin $(PREFIX)/lib/ni-s49 $(HOME)/.config/systemd/user
	install -m 755 $(BUILD)/ni-s49-usb $(BUILD)/ni-nks-bridge $(PREFIX)/bin/
	install -m 755 scripts/ni-s49-hcs scripts/ni-s49-plug $(PREFIX)/bin/
	install -m 755 $(BUILD)/s49-iface.exe $(PREFIX)/lib/ni-s49/
	install -m 644 systemd/*.service $(HOME)/.config/systemd/user/

clean:
	rm -rf $(BUILD)
