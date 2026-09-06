# SPDX-License-Identifier: GPL-2.0-only
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man
CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
DEBUG ?= 0
ifeq ($(DEBUG),0)
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
else ifeq ($(DEBUG),1)
CFLAGS = -std=c11 -Og -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror
else
$(error DEBUG must be 0 (production) or 1 (debug))
endif
STRIP ?= strip
OBJCOPY ?= objcopy
DSYMUTIL ?= dsymutil
LDFLAGS =
JANSSON_CFLAGS = $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS = $(shell pkg-config --libs jansson 2>/dev/null || printf '%s' '-l:libjansson.so.4')
LDLIBS = $(JANSSON_LIBS)
CURL_CFLAGS =
CURL_LIBS = -lcurl
