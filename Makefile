CC        ?= cc
MINGW_CC  ?= x86_64-w64-mingw32-gcc
CXX       ?= g++
MINGW_CXX ?= x86_64-w64-mingw32-g++
CFLAGS    ?= -O2 -Wall -Wextra -Wunused-parameter -Wmisleading-indentation -Wsign-compare

VENDOR = third_party
OBJDIR = obj

ZLIB_SRCS = zlib/adler32.c zlib/compress.c zlib/crc32.c zlib/deflate.c \
            zlib/gzclose.c zlib/gzlib.c zlib/gzread.c zlib/gzwrite.c \
            zlib/infback.c zlib/inffast.c zlib/inflate.c zlib/inftrees.c \
            zlib/trees.c zlib/uncompr.c zlib/zutil.c

PNG_SRCS = libpng/png.c libpng/pngerror.c libpng/pngget.c libpng/pngmem.c \
           libpng/pngpread.c libpng/pngread.c libpng/pngrio.c \
           libpng/pngrtran.c libpng/pngrutil.c libpng/pngset.c \
           libpng/pngsimd.c libpng/pngtrans.c libpng/pngwio.c libpng/pngwrite.c \
           libpng/pngwtran.c libpng/pngwutil.c

JPEG_SRCS := $(wildcard $(VENDOR)/libjpeg/*.c)

LIB_SRCS = aglib.c agvideo.c agthread.c aggl.c \
           agmp4.c agmkv.c agh264.c agaac.c agaudio.c agminimp3.c \
           $(addprefix $(VENDOR)/,$(ZLIB_SRCS) $(PNG_SRCS)) $(JPEG_SRCS)
COMMON   = main.c $(LIB_SRCS)

# --- built in media decoders ------------------------------------------------
# OpenH264 (H.264, BSD-2) and fdk-aac (AAC, Fraunhofer FDK) are compiled
# into every binary — no system packages, no runtime loading.

OH264_DIR   = $(VENDOR)/openh264
OH264_INC   = -I$(OH264_DIR)/codec/api
OH264_LIB_N = $(OBJDIR)/libopenh264-linux.a
OH264_LIB_W = $(OBJDIR)/libopenh264-win.a
# the two builds share the submodule tree — never run them at the same time
.NOTPARALLEL: $(OH264_LIB_N) $(OH264_LIB_W)

$(OH264_LIB_N):
	@mkdir -p $(OBJDIR)
	$(MAKE) -C $(OH264_DIR) clean >/dev/null
	$(MAKE) -C $(OH264_DIR) -j4 V=No libopenh264.a
	cp $(OH264_DIR)/libopenh264.a $@

$(OH264_LIB_W):
	@mkdir -p $(OBJDIR)
	$(MAKE) -C $(OH264_DIR) clean >/dev/null
	$(MAKE) -C $(OH264_DIR) -j4 V=No OS=mingw_nt ARCH=x86_64 \
	    CC="$(MINGW_CC)" CXX="$(MINGW_CXX)" libopenh264.a
	cp $(OH264_DIR)/libopenh264.a $@

# fdk-aac is C++; its objects are built with $(CXX) into two trees so that
# the native and the MinGW builds can coexist.
FDK_DIR = $(VENDOR)/fdk-aac
FDK_INC = -I$(FDK_DIR)/libAACdec/include -I$(FDK_DIR)/libSBRdec/include \
          -I$(FDK_DIR)/libMpegTPDec/include -I$(FDK_DIR)/libDRCdec/include \
          -I$(FDK_DIR)/libPCMutils/include -I$(FDK_DIR)/libSACdec/include \
          -I$(FDK_DIR)/libArithCoding/include -I$(FDK_DIR)/libFDK/include \
          -I$(FDK_DIR)/libSYS/include
FDK_SRCS := $(wildcard $(FDK_DIR)/libAACdec/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libSBRdec/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libMpegTPDec/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libDRCdec/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libPCMutils/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libSACdec/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libArithCoding/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libFDK/src/*.cpp) \
            $(wildcard $(FDK_DIR)/libSYS/src/*.cpp)

FDK_OBJS_N := $(patsubst $(FDK_DIR)/%.cpp,$(OBJDIR)/n/fdk/%.o,$(FDK_SRCS))
FDK_OBJS_W := $(patsubst $(FDK_DIR)/%.cpp,$(OBJDIR)/w/fdk/%.o,$(FDK_SRCS))

$(OBJDIR)/n/fdk/%.o: $(FDK_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w $(FDK_INC) -c $< -o $@

$(OBJDIR)/w/fdk/%.o: $(FDK_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(MINGW_CXX) -O2 -w $(FDK_INC) -c $< -o $@

# ALSA when the headers are installed, otherwise the native library builds
# without sound (video only) — graceful degradation of a built in backend.
# Windows always has waveOut (winmm), selected inside the code by _WIN32.
ALSA_DEFS :=
NATIVE_MEDIA_LIBS := -lstdc++
ifeq ($(shell pkg-config --exists alsa && echo yes),yes)
ALSA_DEFS += -DAG_HAVE_ALSA
NATIVE_MEDIA_LIBS += $(shell pkg-config --libs alsa)
endif
WIN_MEDIA_LIBS := -lstdc++ -lwinmm

X11_CFLAGS := $(shell pkg-config --cflags x11 freetype2)
X11_LIBS   := $(shell pkg-config --libs x11 freetype2) -lm -lpthread -lGL

WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_CFLAGS  := $(shell pkg-config --cflags wayland-client xkbcommon freetype2)
WAYLAND_LIBS    := $(shell pkg-config --libs wayland-client xkbcommon freetype2) -lm -lpthread -lEGL -lGL

WL_DIR  = $(VENDOR)/wayland
WL_XML  = $(WL_DIR)/xdg-shell.xml $(WL_DIR)/xdg-decoration-unstable-v1.xml
WL_HDR  = $(WL_XML:$(WL_DIR)/%.xml=$(WL_DIR)/%-client-protocol.h)
WL_SRC  = $(WL_XML:$(WL_DIR)/%.xml=$(WL_DIR)/%-protocol.c)

WIN_LIBS  := -lgdi32 -static
VEND_INC  := -I$(VENDOR)/zlib -I$(VENDOR)/libpng -I$(VENDOR)/libjpeg -I$(VENDOR)/nanosvg/src -I$(VENDOR)/minimp3

.PHONY: all clean vend wayland test

# Wayland is the preferred backend, X11 is the fallback.
ifeq ($(shell pkg-config --exists wayland-client && echo yes),yes)
all: app-wayland app.exe
else
all: app app.exe
endif

vend:
	@cp -f $(VENDOR)/libpng/pnglibconf.h.prebuilt $(VENDOR)/libpng/pnglibconf.h 2>/dev/null || true

app: $(COMMON) aglib.h aglib_int.h agthread.h aggl.h vend $(OH264_LIB_N) $(FDK_OBJS_N)
	$(CC) $(CFLAGS) $(ALSA_DEFS) -DHAVE_UNISTD_H $(VEND_INC) $(OH264_INC) $(FDK_INC) $(X11_CFLAGS) \
	      $(COMMON) -o app $(OH264_LIB_N) $(FDK_OBJS_N) $(X11_LIBS) $(NATIVE_MEDIA_LIBS)

app.exe: $(COMMON) aglib.h aglib_int.h agthread.h aggl.h $(OH264_LIB_W) $(FDK_OBJS_W)
	$(MINGW_CC) $(CFLAGS) $(VEND_INC) $(OH264_INC) $(FDK_INC) \
	      $(COMMON) -o app.exe $(OH264_LIB_W) $(FDK_OBJS_W) $(WIN_LIBS) $(WIN_MEDIA_LIBS)

# --- Wayland backend -------------------------------------------------------
$(WL_DIR)/%-client-protocol.h: $(WL_DIR)/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

$(WL_DIR)/%-protocol.c: $(WL_DIR)/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

wayland: app-wayland

app-wayland: $(COMMON) aglib.h aglib_int.h agthread.h aggl.h vend $(WL_HDR) $(WL_SRC) $(OH264_LIB_N) $(FDK_OBJS_N)
	$(CC) $(CFLAGS) $(ALSA_DEFS) -DHAVE_UNISTD_H -DAG_USE_WAYLAND \
	      $(VEND_INC) $(OH264_INC) $(FDK_INC) -I$(WL_DIR) $(WAYLAND_CFLAGS) \
	      $(COMMON) $(WL_SRC) -o app-wayland $(OH264_LIB_N) $(FDK_OBJS_N) $(WAYLAND_LIBS) $(NATIVE_MEDIA_LIBS)

# --- Feature smoke test ----------------------------------------------------
test: tests/agtest.c $(LIB_SRCS) aglib.h aglib_int.h agthread.h aggl.h vend $(OH264_LIB_N) $(FDK_OBJS_N)
	$(CC) $(CFLAGS) $(ALSA_DEFS) -DHAVE_UNISTD_H -I. $(VEND_INC) $(OH264_INC) $(FDK_INC) $(X11_CFLAGS) \
	      tests/agtest.c $(LIB_SRCS) -o agtest $(OH264_LIB_N) $(FDK_OBJS_N) $(X11_LIBS) $(NATIVE_MEDIA_LIBS)
	./agtest

clean:
	rm -f app app.exe app-wayland agtest $(WL_HDR) $(WL_SRC)
	rm -rf $(OBJDIR)
