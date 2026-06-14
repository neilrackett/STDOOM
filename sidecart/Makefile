# Version from file
VERSION := $(shell cat version.txt)
ROOT_DIR := $(abspath ..)
DEFAULT_APP_UUID_KEY := 44444444-4444-4444-8444-444444444444
UUID_FROM_FILE := $(strip $(shell [ -f uuid.txt ] && tr -d '\r\n' < uuid.txt))
APP_UUID_KEY_RESOLVED := $(if $(strip $(APP_UUID_KEY)),$(strip $(APP_UUID_KEY)),$(if $(UUID_FROM_FILE),$(UUID_FROM_FILE),$(DEFAULT_APP_UUID_KEY)))
UART_BAUD ?= 115200
UART_DEV ?=

export STCMD_NO_TTY = 1

## Build production firmware for pico_w using APP_UUID_KEY env var, then uuid.txt, then default
.PHONY: build
build:
	@echo "Using APP_UUID_KEY: $(APP_UUID_KEY_RESOLVED)"
	./build.sh pico release "$(APP_UUID_KEY_RESOLVED)"

## Build debug firmware for pico using APP_UUID_KEY env var, then uuid.txt, then default
.PHONY: debug
debug: tests
	@echo "Using APP_UUID_KEY: $(APP_UUID_KEY_RESOLVED)"
	./build.sh pico debug "$(APP_UUID_KEY_RESOLVED)"

## Build all standalone test apps.
.PHONY: tests
tests: pingtest c2ptest recttest rndrtest uptest

## Build the standalone ping smoke test app.
.PHONY: pingtest
pingtest:
	ST_WORKING_FOLDER=$(ROOT_DIR) stcmd m68k-atari-mint-gcc -O2 \
		-I sidecart/tests -I linuxdoom-1.10 \
		-o sidecart/tests/PINGTEST.TOS \
		sidecart/tests/pingtest.c linuxdoom-1.10/sidecart_md.c \
		linuxdoom-1.10/sidecart_stubs.S

## Build the standalone C2P round-trip test app.
.PHONY: c2ptest
c2ptest:
	ST_WORKING_FOLDER=$(ROOT_DIR) stcmd m68k-atari-mint-gcc -O2 \
		-I sidecart/tests -I linuxdoom-1.10 \
		-o sidecart/tests/C2PTEST.TOS \
		sidecart/tests/c2ptest.c linuxdoom-1.10/sidecart_md.c \
		linuxdoom-1.10/sidecart_stubs.S

## Build the standalone rect C2P test app.
.PHONY: recttest
recttest:
	ST_WORKING_FOLDER=$(ROOT_DIR) stcmd m68k-atari-mint-gcc -O2 \
		-I sidecart/tests -I linuxdoom-1.10 \
		-o sidecart/tests/RECTTEST.TOS \
		sidecart/tests/recttest.c linuxdoom-1.10/sidecart_md.c \
		linuxdoom-1.10/sidecart_stubs.S

## Build the standalone render-mode (M4) test app.
.PHONY: rndrtest
rndrtest:
	ST_WORKING_FOLDER=$(ROOT_DIR) stcmd m68k-atari-mint-gcc -O2 \
		-I sidecart/tests -I linuxdoom-1.10 \
		-o sidecart/tests/RNDRTEST.TOS \
		sidecart/tests/rndrtest.c linuxdoom-1.10/sidecart_md.c \
		linuxdoom-1.10/sidecart_stubs.S

## Build the standalone ST->cart upload benchmark.
## Regenerates doom_frames.h from the PNG screenshots first (host Python step,
## stdlib only - runs on the host, not via stcmd).
.PHONY: uptest
uptest:
	python3 $(ROOT_DIR)/sidecart/tests/png2chunky.py
	ST_WORKING_FOLDER=$(ROOT_DIR) stcmd m68k-atari-mint-gcc -O2 \
		-I sidecart/tests -I linuxdoom-1.10 \
		-o sidecart/tests/UPTEST.TOS \
		sidecart/tests/uptest.c linuxdoom-1.10/sidecart_md.c \
		linuxdoom-1.10/sidecart_stubs.S

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"

## List candidate UART devices (macOS + Linux)
.PHONY: uart-list
uart-list:
	@for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/tty.usbserial* /dev/ttyACM* /dev/ttyUSB* /dev/cu.J* /dev/tty.J*; do \
		[ -e "$$p" ] && echo "$$p"; \
	done

## Open UART console on the first matching serial device using screen.
## Override with: make uart UART_DEV=/dev/cu.usbmodemXXXX
.PHONY: uart
uart:
	@dev="$(UART_DEV)"; \
	if [ -z "$$dev" ]; then \
		dev="$$(for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/tty.usbserial* /dev/ttyACM* /dev/ttyUSB* /dev/cu.J* /dev/tty.J*; do [ -e "$$p" ] && echo "$$p"; done | head -n 1)"; \
	fi; \
	if [ -z "$$dev" ]; then \
		echo "No serial device found. Run: make uart-list"; \
		exit 1; \
	fi; \
	echo "Opening UART on $$dev at $(UART_BAUD) baud"; \
	echo "Detach with Ctrl-A then K (or Ctrl-A then \\)"; \
	screen "$$dev" $(UART_BAUD)
