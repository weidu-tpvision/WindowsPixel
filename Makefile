UNAME_S := $(shell uname -s)
CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

ifeq ($(UNAME_S),Darwin)
SLIRP_CFLAGS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --cflags slirp 2>/dev/null)
SLIRP_LIBS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --libs slirp 2>/dev/null)
SLIRP_OBJ =
LDFLAGS_TEST =
else
CFLAGS += -D_GNU_SOURCE
SLIRP_CFLAGS = -Ithird_party/libslirp -DHAVE_SLIRP=1
SLIRP_LIBS =
SLIRP_SRC = $(wildcard third_party/libslirp/*.c)
SLIRP_OBJ = $(patsubst third_party/libslirp/%.c,$(B)/slirp_%.o,$(SLIRP_SRC))
LDFLAGS_TEST = -pthread
endif

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
ENGINE = $(CORE) $(HOST)

DEV_COMMON = $(B)/acpi.o $(B)/fb_display.o $(B)/fdt.o $(B)/fw_cfg.o $(B)/gicv3.o $(B)/hda.o \
             $(B)/nvme.o $(B)/pcie.o $(B)/pflash.o $(B)/pl011.o $(B)/pl031.o $(B)/pl061.o $(B)/timer.o \
             $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_input.o $(B)/virtio_net.o $(B)/virtio_snd.o \
             $(B)/winvirt.o $(B)/xhci.o

ifeq ($(UNAME_S),Darwin)
DEV = $(DEV_COMMON) $(B)/fb_metal.o
VM_HOST = $(B)/winvm.o $(B)/net_slirp.o $(B)/share.o $(B)/audio_mac.o $(B)/hvsparse.o
else
DEV = $(DEV_COMMON)
VM_HOST = $(B)/winvm.o $(B)/net_slirp.o $(B)/share.o $(B)/hvsparse.o $(SLIRP_OBJ)
endif

MAC_FRAMEWORKS = -framework Cocoa -framework Foundation -framework QuartzCore -framework Metal \
                 -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
                 -framework AudioToolbox

CORE_TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
             $(B)/test_mmu $(B)/test_exc

ifeq ($(UNAME_S),Darwin)
all: $(CORE_TESTS) $(B)/hvsparse $(B)/winvm
else
all: $(CORE_TESTS) $(B)/hvsparse $(B)/winvm $(B)/libwinvm.a
endif

# mmu.c: no SIMD (hv_tlb_refill runs with the guest's SIMD registers live).
$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@
$(B)/net_slirp.o: host/net_slirp.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(B)/slirp_%.o: third_party/libslirp/%.c third_party/libslirp/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -c $< -o $@
$(B)/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/main_mac.o: host/main_mac.m host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/%.o: core/%.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(UNAME_S),Darwin)
$(B)/winvm: $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST)
	$(CC) $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST) $(SLIRP_LIBS) $(MAC_FRAMEWORKS) -o $@
else
$(B)/winvm: $(B)/main_linux.o $(ENGINE) $(DEV) $(VM_HOST)
	$(CC) $(B)/main_linux.o $(ENGINE) $(DEV) $(VM_HOST) $(SLIRP_LIBS) -pthread -lm -o $@
endif

$(B)/libwinvm.a: $(ENGINE) $(DEV) $(VM_HOST)
	rm -f $@
	ar rcs $@ $(ENGINE) $(DEV) $(VM_HOST)

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS_TEST) -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS_TEST) -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS_TEST) -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) $(LDFLAGS_TEST) -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) $(LDFLAGS_TEST) -o $@

test-core: $(CORE_TESTS)
	@ulimit -s 65520; for t in $(CORE_TESTS); do echo "== $$t"; $$t || exit 1; done

$(B):
	mkdir -p $(B)

# ---- iOS static library target: build/ios/libwinvm.a ----
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
IOS_SLIRP_INC ?= ios/deps/include/slirp
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -I$(IOS_SLIRP_INC)
IOS_OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
IB = build/ios

IOS_C_SRC = $(wildcard core/*.c) \
            host/jit_region.c host/disk.c host/winvm.c host/net_slirp.c host/share.c \
            host/audio_mac.c host/hvsparse.c host/prof_ios.c \
            $(wildcard devices/*.c)
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_C_SRC)) $(IB)/host/enter.o $(IB)/devices/fb_metal.o

$(IB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/devices/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_OBJCFLAGS) -c $< -o $@
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
$(IB)/libwinvm.a: $(IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)

ios-lib: $(IB)/libwinvm.a

# ---- Android (Pixel 9 / Tensor G4) targets: build/android/libwinvm.{a,so} ----
ANDROID_CC ?= aarch64-linux-android34-clang
ANDROID_AR ?= llvm-ar
ANDROID_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -fPIC -D_GNU_SOURCE -DHAVE_SLIRP=1 -fno-sanitize=shadow-call-stack -Icore -Ihost -Idevices -Ithird_party/libslirp
AB = build/android

ANDROID_C_SRC = $(wildcard core/*.c) \
                host/jit_region.c host/disk.c host/winvm.c host/net_slirp.c host/share.c host/hvsparse.c \
                $(wildcard devices/*.c) \
                $(wildcard third_party/libslirp/*.c)
ANDROID_OBJ = $(patsubst %.c,$(AB)/%.o,$(ANDROID_C_SRC)) $(AB)/host/enter.o

$(AB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(ANDROID_CC) $(ANDROID_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(AB)/third_party/libslirp/%.o: third_party/libslirp/%.c third_party/libslirp/*.h
	@mkdir -p $(dir $@)
	$(ANDROID_CC) $(ANDROID_CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -c $< -o $@
$(AB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(ANDROID_CC) $(ANDROID_CFLAGS) -c $< -o $@
$(AB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(ANDROID_CC) -fPIC -c $< -o $@
$(AB)/libwinvm.a: $(ANDROID_OBJ)
	rm -f $@
	$(ANDROID_AR) rcs $@ $(ANDROID_OBJ)
$(AB)/libwinvm.so: $(ANDROID_OBJ)
	$(ANDROID_CC) -shared -o $@ $(ANDROID_OBJ) -llog -lm

android-lib: $(AB)/libwinvm.a $(AB)/libwinvm.so

clean:
	rm -rf $(B)

.PHONY: all test-core ios-lib android-lib clean
