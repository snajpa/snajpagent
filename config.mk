# SPDX-License-Identifier: GPL-2.0-only
PREFIX = /usr/local
CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
LDFLAGS =
JANSSON_CFLAGS = $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS = $(shell pkg-config --libs jansson 2>/dev/null || printf '%s' '-l:libjansson.so.4')
LDLIBS = $(JANSSON_LIBS)
CURL_CFLAGS =
CURL_LIBS = -lcurl
