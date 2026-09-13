# SPDX-License-Identifier: GPL-2.0-only
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man
CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64
DEBUG ?= 0
ifeq ($(DEBUG),0)
CFLAGS = -std=c11 -Os -g -Wall -Wextra -Wpedantic -Werror
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
WITH_AV ?= 1
ifeq ($(WITH_AV),1)
AV_CFLAGS ?= $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale)
AV_LIBS ?= $(shell pkg-config --libs libavformat libavcodec libavutil libswresample libswscale)
else ifeq ($(WITH_AV),0)
AV_CFLAGS =
AV_LIBS =
else
$(error WITH_AV must be 1 (linked audio/video) or 0 (custom lean build))
endif
override CPPFLAGS += -DSNAJPAGENT_AV=$(WITH_AV) $(AV_CFLAGS)
override LDLIBS += $(AV_LIBS)
WITH_PDF ?= 1
ifeq ($(WITH_PDF),1)
CXX ?= c++
CXXFLAGS ?= $(filter-out -std=c11,$(CFLAGS)) -std=c++20
PDF_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags poppler libpng))
PDF_LIBS ?= $(shell pkg-config --libs poppler libpng) -lstdc++
PDF_OBJ = src/pdf.o
else ifeq ($(WITH_PDF),0)
PDF_CFLAGS =
PDF_LIBS =
PDF_OBJ = src/pdf_stub.o
else
$(error WITH_PDF must be 1 (linked PDF) or 0 (custom lean build))
endif
override CPPFLAGS += -DSNAJPAGENT_PDF=$(WITH_PDF)
override LDLIBS += $(PDF_LIBS)
WITH_AUDIO_DEVICE ?= 1
ifeq ($(WITH_AUDIO_DEVICE),1)
MINIAUDIO_CFLAGS ?= $(shell pkg-config --cflags miniaudio)
AUDIO_DEVICE_LIBS ?= $(if $(filter Windows Windows_NT,$(TARGET_OS)),-lole32 -lwinmm,$(if $(filter Darwin,$(TARGET_OS)),-framework CoreFoundation -framework CoreAudio -framework AudioToolbox,$(if $(filter FreeBSD OpenBSD NetBSD,$(TARGET_OS)),-lm,-ldl -lm)))
AUDIO_DEVICE_OBJ = src/miniaudio.o
else ifeq ($(WITH_AUDIO_DEVICE),0)
MINIAUDIO_CFLAGS =
AUDIO_DEVICE_LIBS =
AUDIO_DEVICE_OBJ =
else
$(error WITH_AUDIO_DEVICE must be 1 or 0 (custom lean build))
endif
override CPPFLAGS += -DSNAJPAGENT_AUDIO_DEVICE=$(WITH_AUDIO_DEVICE) $(MINIAUDIO_CFLAGS)
override LDLIBS += $(AUDIO_DEVICE_LIBS)

WITH_OFFICE ?= 1
WITH_OFFICE_COMMANDS ?= 0
ifeq ($(WITH_OFFICE),1)
ifeq ($(WITH_OFFICE_COMMANDS),1)
$(error WITH_OFFICE=1 and WITH_OFFICE_COMMANDS=1 are mutually exclusive Office states)
endif
ifneq ($(WITH_PDF),1)
$(error WITH_OFFICE=1 requires WITH_PDF=1; disable both for a lean build)
endif
# Relative roots are resolved from the executable, not the conversion cwd.
# Supply OFFICE_CFLAGS/OFFICE_LIBS explicitly when compiling a relative bundle.
OFFICE_ROOT ?= /usr/lib/libreoffice
OFFICE_CFLAGS ?= $(shell pkg-config --cflags libarchive libxml-2.0 libpng) -I$(OFFICE_ROOT)/../../include
OFFICE_LIBS ?= $(shell pkg-config --libs libarchive libxml-2.0 libpng) -L$(OFFICE_ROOT)/program -Wl,-rpath,$(OFFICE_ROOT)/program -lsofficeapp
else ifeq ($(WITH_OFFICE),0)
OFFICE_CFLAGS =
OFFICE_LIBS =
else
$(error WITH_OFFICE must be 1 or 0 (custom lean build))
endif
ifneq ($(WITH_OFFICE_COMMANDS),0)
ifneq ($(WITH_OFFICE_COMMANDS),1)
$(error WITH_OFFICE_COMMANDS must be 1 or 0 (custom lean build))
endif
endif
override CPPFLAGS += -DSNAJPAGENT_OFFICE=$(WITH_OFFICE) -DSNAJPAGENT_OFFICE_COMMANDS=$(WITH_OFFICE_COMMANDS) -DSNAJPAGENT_OFFICE_ROOT='"$(OFFICE_ROOT)"' $(OFFICE_CFLAGS)
override LDLIBS += $(OFFICE_LIBS)
