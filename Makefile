PLATFORM ?= $(shell uname -s | sed 's/Darwin/macos-arm64/;s/Linux/linux-x64/')
CXX ?= c++
BUILD_ROOT ?= build
SDK_ROOT ?= 3rd/$(PLATFORM)
INCLUDE_DIR := $(SDK_ROOT)/include
LIB_DIR := $(SDK_ROOT)/lib
BUILD_DIR := $(BUILD_ROOT)/$(PLATFORM)
TARGET := $(BUILD_DIR)/tirtc_client_example
SOURCE := src/main.cc

CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
COMMON_LIBS := \
	$(LIB_DIR)/libmatrix_runtime_facade.a \
	$(LIB_DIR)/libmatrix_runtime_transport.a \
	$(LIB_DIR)/libmatrix_runtime_media.a \
	$(LIB_DIR)/libmatrix_runtime_audio.a \
	$(LIB_DIR)/libmatrix_runtime_video.a \
	$(LIB_DIR)/libmatrix_runtime_foundation_logging.a \
	$(LIB_DIR)/libmatrix_runtime_foundation_http.a \
	$(LIB_DIR)/libwebrtc_apm.a \
	$(LIB_DIR)/libxlog.a \
	$(LIB_DIR)/libTiRTC.a \
	$(LIB_DIR)/libssl.a \
	$(LIB_DIR)/libcrypto.a \
	$(LIB_DIR)/libavcodec.a \
	$(LIB_DIR)/libavutil.a \
	$(LIB_DIR)/libswscale.a \
	$(LIB_DIR)/libswresample.a \
	$(LIB_DIR)/libavformat.a \
	$(LIB_DIR)/libavfilter.a \
	$(LIB_DIR)/libpostproc.a \
	$(LIB_DIR)/libx264.a

.PHONY: all clean check-sdk

all: $(TARGET)

check-sdk:
	@test -f "$(INCLUDE_DIR)/tirtc/trp.h" || { echo "[client-example] missing runtime headers under $(INCLUDE_DIR)" >&2; exit 3; }
	@test -f "$(LIB_DIR)/libmatrix_runtime_facade.a" || { echo "[client-example] missing runtime libs under $(LIB_DIR)" >&2; exit 3; }

$(TARGET): $(SOURCE) check-sdk
	@mkdir -p "$(BUILD_DIR)"
ifeq ($(PLATFORM),macos-arm64)
	$(CXX) $(CXXFLAGS) -I "$(INCLUDE_DIR)" "$(SOURCE)" -o "$(TARGET)" \
		$(COMMON_LIBS) \
		$(LIB_DIR)/libTGTRP.a \
		-framework AudioToolbox \
		-framework Foundation \
		-framework CoreFoundation \
		-framework CoreMedia \
		-framework CoreVideo \
		-framework VideoToolbox \
		-framework CoreGraphics \
		-framework AppKit \
		-framework Security \
		-lobjc -lz -lpthread -lm
	@test ! -f "$(LIB_DIR)/libtgrtc.dylib" || ln -sf "../$(LIB_DIR)/libtgrtc.dylib" "$(BUILD_DIR)/libtgrtc.dylib"
else ifeq ($(PLATFORM),linux-x64)
	$(CXX) $(CXXFLAGS) -I "$(INCLUDE_DIR)" "$(SOURCE)" -o "$(TARGET)" \
		-Wl,--start-group \
		$(COMMON_LIBS) \
		$(LIB_DIR)/libwebrtc.a \
		$(LIB_DIR)/libusrsctp.a \
		$(LIB_DIR)/libmbedtls.a \
		-Wl,--end-group \
		-ldl -lpthread -lm -lz
else
	@echo "[client-example] unsupported PLATFORM=$(PLATFORM), expected macos-arm64 or linux-x64" >&2
	@exit 3
endif

clean:
	rm -rf "$(BUILD_ROOT)"
