# Makefile for rebuilding / running a recompiled PSP game
#
# Usage:
#   mingw32-make GAME_NAME=mygame GAME_ELF=eboot.elf GAME_BASE=0x08804000 GAME_ENTRY=0x08804128
#

# Override these on the command line for your game (see README).
# GAME_BASE is the usual PSP user-module base; GAME_ENTRY is game-specific and
# must be set to your ELF's entry point.
GAME_NAME  ?= mygame
GAME_ELF   ?= eboot.elf
GAME_BASE  ?= 0x08804000
GAME_ENTRY ?= 0x08804000

CC         ?= gcc
CXX        ?= g++
PYTHON     ?= python
CFLAGS     ?= -O1 -foptimize-sibling-calls -Isrc/rt -DSR_SDL3VK -DSR_D3D12 -Wall -Wextra
CXXFLAGS   ?= -std=c++17 -O1 -Isrc/rt -DSR_D3D12 -Wall -Wextra -fno-exceptions -fno-rtti
LDFLAGS    ?= -fuse-ld=lld
LIBS       ?= -lSDL3 -lvulkan-1 -ld3d12 -ldxgi -lmfplat -lgdi32 -ldinput8 -ldxguid -lole32 -lwinmm -lxaudio2_9 -lbcrypt

BUILD_DIR  ?= build/$(GAME_NAME)

RT_GE_O    := $(BUILD_DIR)/ge.o
RT_AUDIO_XAUDIO2_O := $(BUILD_DIR)/audio_xaudio2.o
RT_D3D12_O := $(BUILD_DIR)/gpu_d3d12_device.o
RT_D3D12_FRAMEBUFFER_O := $(BUILD_DIR)/gpu_d3d12_framebuffer.o
RT_D3D12_FRAMEBUFFER_KEY_O := $(BUILD_DIR)/gpu_d3d12_framebuffer_key.o
RT_D3D12_VERTEX_O := $(BUILD_DIR)/gpu_d3d12_vertex.o
RT_D3D12_PIPELINE_O := $(BUILD_DIR)/gpu_d3d12_pipeline.o
RT_D3D12_PIPELINE_KEY_O := $(BUILD_DIR)/gpu_d3d12_pipeline_key.o
RT_D3D12_PRESENT_O := $(BUILD_DIR)/gpu_d3d12_present.o
RT_D3D12_PRESENT_LAYOUT_O := $(BUILD_DIR)/gpu_d3d12_present_layout.o
RT_D3D12_SHADER_STATE_O := $(BUILD_DIR)/gpu_d3d12_shader_state.o
RT_D3D12_TEXTURE_O := $(BUILD_DIR)/gpu_d3d12_texture.o
RT_D3D12_TEXTURE_KEY_O := $(BUILD_DIR)/gpu_d3d12_texture_key.o
RT_SRCS    := src/rt/recomp.c \
              src/rt/dispatch_table.c \
              src/rt/fpu_mode.c \
src/rt/interp_runtime.cpp \
src/ref/interp.cpp \
              src/rt/module.c \
              src/rt/vfpu_interp.c \
              src/rt/vfpu_asin.c \
              src/rt/hle.c \
              src/rt/net.c \
              src/rt/kmem.c \
              src/rt/intr.c \
              src/rt/gelist.c \
              src/rt/callback.c \
              src/rt/sched.c \
              src/rt/iso.c \
              src/rt/mpeg.c \
              src/rt/pgf.c \
              src/rt/gui.c \
              src/rt/settings_path.c \
              src/rt/graphics_config.c \
              src/rt/graphics_settings.c \
              src/rt/input_config.c \
              src/rt/input_settings.c \
              src/rt/kzl_twinstick.c \
              src/rt/startup_check.c \
              src/rt/audio_mixer.c \
              src/rt/sas_reverb.c \
              src/rt/h264_mf.c \
              src/rt/savedata.c \
              src/rt/savedata_crypto.cpp \
              third_party/ppsspp_savedata/AES.c \
              third_party/ppsspp_savedata/SHA1.c \
              third_party/ppsspp_savedata/bn.c \
              third_party/ppsspp_savedata/ec.c \
              third_party/ppsspp_savedata/kirk_engine.c \
              third_party/ppsspp_savedata/chnnlsv.cpp \
              src/rt/osk_win.c \
              src/rt/driver.c \
              src/rt/gpu_sdl3vk/sdl3vk.c \
              src/rt/gpu_sdl3vk/ge_gpu.c
AT3_NAMES  := atrac atrac3 atrac3plus atrac3plusdec atrac3plusdsp get_bits compat fft mem
AT3_OBJS   := $(addprefix $(BUILD_DIR)/at3_,$(addsuffix .o,$(AT3_NAMES))) \
              $(BUILD_DIR)/atrac_decode.o

DXC        ?= dxc

.PHONY: all pipeline compile clean run shaders-d3d12 check-shaders-d3d12 \
        test-d3d12-vertex test-module-loader test-interpreter test-runtime

all: pipeline compile

pipeline: $(BUILD_DIR)/$(GAME_NAME)_image.bin $(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_imports.toml

$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_ELF) tools/prxload.py
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) tools/prxload.py $(GAME_ELF) $(GAME_BASE) --out=$@

$(BUILD_DIR)/$(GAME_NAME)_recomp.c: $(GAME_ELF) tools/codegen.py tools/analyze.py tools/prxload.py
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) tools/codegen.py $(GAME_ELF) $@ --base=$(GAME_BASE)

$(BUILD_DIR)/$(GAME_NAME)_imports.toml: $(GAME_ELF) tools/imports.py tools/analyze.py tools/prxload.py
	@mkdir -p $(BUILD_DIR)
	$(PYTHON) tools/imports.py $(GAME_ELF) $(GAME_BASE) --toml=$@

# ge.c is the software rasterizer fallback, compile with -O2
$(RT_GE_O): src/rt/ge.c src/rt/recomp.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -fno-math-errno -Wall -Wextra -Isrc/rt -c src/rt/ge.c -o $@

$(BUILD_DIR)/at3_%.o: third_party/at3_standalone/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Ithird_party/at3_standalone -c $< -o $@

$(BUILD_DIR)/atrac_decode.o: src/rt/atrac_decode.cpp src/rt/atrac_decode.h \
                            third_party/at3_standalone/at3_decoders.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Ithird_party/at3_standalone -c $< -o $@

$(RT_AUDIO_XAUDIO2_O): src/rt/audio_xaudio2.cpp src/rt/audio.h src/rt/audio_mixer.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(RT_D3D12_O): src/rt/gpu_d3d12/device.cpp src/rt/gpu_d3d12/device.h \
               src/rt/gpu_d3d12/framebuffer.h \
               src/rt/gpu_d3d12/vertex.h \
               src/rt/gpu_d3d12/texture.h \
               src/rt/gpu_d3d12/shaders/psp_vs.inc src/rt/gpu_d3d12/shaders/psp_ps.inc \
               src/rt/gpu_d3d12/shaders/psp_ps_direct.inc \
               src/rt/ge_shared.h src/rt/recomp.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/device.cpp -o $@

$(RT_D3D12_FRAMEBUFFER_O): src/rt/gpu_d3d12/framebuffer.cpp \
                           src/rt/gpu_d3d12/framebuffer.h \
                           src/rt/gpu_d3d12/texture.h \
                           src/rt/gpu_d3d12/device.h src/rt/ge_shared.h \
                           src/rt/recomp.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/framebuffer.cpp -o $@

$(RT_D3D12_FRAMEBUFFER_KEY_O): src/rt/gpu_d3d12/framebuffer_key.cpp \
                               src/rt/gpu_d3d12/framebuffer.h \
                               src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/framebuffer_key.cpp -o $@

$(RT_D3D12_VERTEX_O): src/rt/gpu_d3d12/vertex.cpp src/rt/gpu_d3d12/vertex.h \
                      src/rt/gpu_d3d12/device.h src/rt/gpu_d3d12/pipeline.h \
                      src/rt/gpu_d3d12/texture.h \
                      src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/vertex.cpp -o $@

$(RT_D3D12_PIPELINE_O): src/rt/gpu_d3d12/pipeline.cpp src/rt/gpu_d3d12/pipeline.h \
                        src/rt/gpu_d3d12/device.h src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/pipeline.cpp -o $@

$(RT_D3D12_PIPELINE_KEY_O): src/rt/gpu_d3d12/pipeline_key.cpp \
                            src/rt/gpu_d3d12/pipeline.h src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/pipeline_key.cpp -o $@

$(RT_D3D12_PRESENT_O): src/rt/gpu_d3d12/present.cpp src/rt/gpu_d3d12/present.h \
                       src/rt/gpu_d3d12/device.h \
                       src/rt/gpu_d3d12/shaders/present_vs.inc \
                       src/rt/gpu_d3d12/shaders/present_ps.inc
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/present.cpp -o $@

$(RT_D3D12_PRESENT_LAYOUT_O): src/rt/gpu_d3d12/present_layout.cpp \
                              src/rt/gpu_d3d12/present.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/present_layout.cpp -o $@

$(RT_D3D12_SHADER_STATE_O): src/rt/gpu_d3d12/shader_state.cpp \
                            src/rt/gpu_d3d12/shader_state.h \
                            src/rt/gpu_d3d12/texture.h \
                            src/rt/gpu_d3d12/device.h src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/shader_state.cpp -o $@

$(RT_D3D12_TEXTURE_O): src/rt/gpu_d3d12/texture.cpp \
                       src/rt/gpu_d3d12/texture.h \
                       src/rt/gpu_d3d12/framebuffer.h \
                       src/rt/gpu_d3d12/device.h \
                       src/rt/ge_shared.h src/rt/recomp.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/texture.cpp -o $@

$(RT_D3D12_TEXTURE_KEY_O): src/rt/gpu_d3d12/texture_key.cpp \
                           src/rt/gpu_d3d12/texture.h src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c src/rt/gpu_d3d12/texture_key.cpp -o $@

$(BUILD_DIR)/d3d12_vertex_test.exe: tools/tests/d3d12_vertex_test.cpp \
                                   src/rt/gpu_d3d12/vertex.cpp \
                                   src/rt/gpu_d3d12/framebuffer_key.cpp \
                                   src/rt/gpu_d3d12/pipeline_key.cpp \
                                   src/rt/gpu_d3d12/shader_state.cpp \
                                   src/rt/gpu_d3d12/present_layout.cpp \
                                   src/rt/gpu_d3d12/texture_key.cpp \
                                   src/rt/gpu_d3d12/vertex.h \
                                   src/rt/gpu_d3d12/pipeline.h \
                                   src/rt/gpu_d3d12/present.h \
                                   src/rt/gpu_d3d12/framebuffer.h \
                                   src/rt/gpu_d3d12/device.h src/rt/ge_shared.h
	@$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CXX) $(CXXFLAGS) tools/tests/d3d12_vertex_test.cpp \
		src/rt/gpu_d3d12/vertex.cpp src/rt/gpu_d3d12/pipeline_key.cpp \
		src/rt/gpu_d3d12/framebuffer_key.cpp \
		src/rt/gpu_d3d12/shader_state.cpp \
		src/rt/gpu_d3d12/present_layout.cpp \
		src/rt/gpu_d3d12/texture_key.cpp -o $@

test-d3d12-vertex: $(BUILD_DIR)/d3d12_vertex_test.exe
	$<

shaders-d3d12:
	$(PYTHON) tools/compile_d3d12_shaders.py --dxc "$(DXC)"

check-shaders-d3d12:
	$(PYTHON) tools/compile_d3d12_shaders.py --dxc "$(DXC)" --check

test-module-loader:
	@mkdir -p build/tests
	$(CC) -std=c11 -O0 -Isrc/rt -Wall -Wextra \
		tools/tests/module_loader_test.c -o build/tests/module_loader_test.exe
	./build/tests/module_loader_test.exe

test-interpreter:
	@mkdir -p build/tests
	$(CC) -std=c11 -O0 -Isrc/rt -c src/rt/recomp.c -o build/tests/interp_recomp.o
	$(CC) -std=c11 -O0 -Isrc/rt -c src/rt/dispatch_table.c -o build/tests/interp_dispatch.o
	$(CC) -std=c11 -O0 -Isrc/rt -c src/rt/fpu_mode.c -o build/tests/interp_fpu.o
	$(CC) -std=c11 -O0 -Isrc/rt -c src/rt/settings_path.c -o build/tests/interp_settings.o
	$(CXX) -std=c++17 -O0 -Isrc/rt -Isrc/ref -c \
		src/rt/interp_runtime.cpp -o build/tests/interp_runtime.o
	$(CXX) -std=c++17 -O0 -Isrc/rt -Isrc/ref -c \
		src/ref/interp.cpp -o build/tests/interp_ref.o
	$(CXX) -std=c++17 -O0 -Isrc/rt -Isrc/ref -c \
		tools/tests/interp_fallback_test.cpp -o build/tests/interp_test.o
	$(CXX) -o build/tests/interp_fallback_test.exe \
		build/tests/interp_test.o build/tests/interp_recomp.o \
		build/tests/interp_dispatch.o build/tests/interp_fpu.o \
		build/tests/interp_settings.o build/tests/interp_runtime.o \
		build/tests/interp_ref.o -lm
	./build/tests/interp_fallback_test.exe

test-runtime: test-module-loader test-interpreter

# Generated C code gets huge. Cache the compiled object to avoid rebuilding it on runtime changes.
$(BUILD_DIR)/$(GAME_NAME)_recomp.o: $(BUILD_DIR)/$(GAME_NAME)_recomp.c src/rt/recomp.h
	$(CC) $(CFLAGS) -c $< -o $@

compile: $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(RT_GE_O) $(RT_AUDIO_XAUDIO2_O) $(RT_D3D12_O) \
         $(RT_D3D12_FRAMEBUFFER_O) $(RT_D3D12_FRAMEBUFFER_KEY_O) \
         $(RT_D3D12_VERTEX_O) $(RT_D3D12_PIPELINE_O) $(RT_D3D12_PIPELINE_KEY_O) \
         $(RT_D3D12_PRESENT_O) $(RT_D3D12_PRESENT_LAYOUT_O) \
         $(RT_D3D12_SHADER_STATE_O) \
         $(RT_D3D12_TEXTURE_O) $(RT_D3D12_TEXTURE_KEY_O) $(AT3_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(GAME_NAME).exe \
		$(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(RT_GE_O) \
		$(RT_AUDIO_XAUDIO2_O) \
		$(RT_D3D12_O) \
		$(RT_D3D12_FRAMEBUFFER_O) \
		$(RT_D3D12_FRAMEBUFFER_KEY_O) \
		$(RT_D3D12_VERTEX_O) \
		$(RT_D3D12_PIPELINE_O) \
		$(RT_D3D12_PIPELINE_KEY_O) \
		$(RT_D3D12_PRESENT_O) \
		$(RT_D3D12_PRESENT_LAYOUT_O) \
		$(RT_D3D12_SHADER_STATE_O) \
		$(RT_D3D12_TEXTURE_O) \
		$(RT_D3D12_TEXTURE_KEY_O) \
		$(AT3_OBJS) \
		$(RT_SRCS) \
		$(LIBS) -lstdc++ -lm
	@cp SDL3.dll $(BUILD_DIR)/ 2>/dev/null || cp ../SDL3.dll $(BUILD_DIR)/ 2>/dev/null || true
	@cp -r font $(BUILD_DIR)/ 2>/dev/null || cp -r ../font $(BUILD_DIR)/ 2>/dev/null || true
	@echo "Build finished: $(BUILD_DIR)/$(GAME_NAME).exe"

clean:
	rm -rf $(BUILD_DIR)

run: all
	./$(BUILD_DIR)/$(GAME_NAME).exe --image $(BUILD_DIR)/$(GAME_NAME)_image.bin $(GAME_BASE) $(GAME_ENTRY) none none --gui
