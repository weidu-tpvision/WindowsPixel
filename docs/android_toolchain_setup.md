# Android CLI, SDK & NDK Toolchain Setup (Linux ARM64 / DGX Spark)

## Overview

This document details the headless, user-space Android build and cross-compilation environment configured on the **NVIDIA DGX Spark server** (`spark-20aa`, Ubuntu 24.04 on 64-bit ARM / AArch64).

The entire toolchain runs natively on ARM64 without requiring `sudo` or root permissions, enabling command-line development, native C/C++ NDK compilation, and APK building for **`WindowsPixel`**.

---

## 1. Installed Components & Paths

| Tool | Version | Installation Directory | Binary Path |
| :--- | :--- | :--- | :--- |
| **Java Development Kit (JDK)** | OpenJDK 17.0.21 (Temurin) | `~/.local/jdk-17` | `~/.local/jdk-17/bin/java` |
| **Android CLI** | 1.1.16717390 | `~/.local/bin` | `~/.local/bin/android` |
| **Android Debug Bridge (ADB)** | 1.0.41 (API 34) | `~/.local/usr/lib/android-sdk/` | `~/.local/bin/adb` |
| **LLVM Compiler Suite** | Clang 18.1.3 + LLD | `~/.local/usr/lib/llvm-18/` | `~/.local/bin/clang`, `lld` |
| **Android NDK** | r27c (`27.2.12479018`) | `~/Android/Sdk/ndk/27.2.12479018` | NDK sysroots and headers |
| **Android NDK C Compiler** | Target `aarch64-linux-android34` | `~/.local/bin` | `~/.local/bin/aarch64-linux-android34-clang` |
| **Android NDK C++ Compiler** | Target `aarch64-linux-android34` | `~/.local/bin` | `~/.local/bin/aarch64-linux-android34-clang++` |

---

## 2. Environment Variables

The following configuration has been added to the top of `~/.bashrc`:

```bash
# Android SDK, NDK, Clang and Java 17 configuration
export JAVA_HOME="$HOME/.local/jdk-17"
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_NDK_ROOT="$HOME/Android/Sdk/ndk/27.2.12479018"
export NDK="$ANDROID_NDK_ROOT"
export PATH="$HOME/.local/bin:$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
```

To reload in any current shell:
```bash
source ~/.bashrc
```

---

## 3. Important Implementation Notes

### A. Why Native ARM64 Clang is Used Instead of NDK Prebuilts
Google does not distribute a native `linux-aarch64` host build of the NDK. The prebuilt Clang inside `toolchains/llvm/prebuilt/linux-x86_64/` contains x86_64 binaries.

**Solution:**  
We installed a native ARM64 build of **Clang 18.1.3** and created compiler wrappers that combine:
1. Native ARM64 host Clang (`~/.local/bin/clang`).
2. Google's NDK sysroot headers and Bionic C libraries (`$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot`).
3. Google's NDK compiler-rt builtins (`libclang_rt.builtins-aarch64-android.a`).

### B. The NDK Compiler Wrappers
The custom wrapper script at `~/.local/bin/aarch64-linux-android34-clang`:
```sh
#!/bin/sh
NDK_DIR="$HOME/Android/Sdk/ndk/27.2.12479018"
SYSROOT="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
RESOURCE_DIR="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18"
exec "$HOME/.local/bin/clang" \
  --target=aarch64-linux-android34 \
  --sysroot="$SYSROOT" \
  -resource-dir "$RESOURCE_DIR" \
  -rtlib=compiler-rt \
  -fuse-ld=lld \
  "$@"
```

---

## 4. Verification & Testing

### Verification Commands
Run the following to check that all tools are recognized:
```bash
java -version               # OpenJDK 17.0.21
android --version           # 1.1.16717390
adb version                 # 1.0.41 (34.0.4)
clang --version             # 18.1.3 (aarch64-unknown-linux-gnu)
aarch64-linux-android34-clang --version # Target: aarch64-unknown-linux-android34
```

### Test Compiling an Android Shared Library (`.so`)
```bash
cat << 'EOF' > test.c
#include <jni.h>
JNIEXPORT jint JNICALL Java_com_example_test_add(JNIEnv *env, jobject thiz, jint a, jint b) {
    return a + b;
}
EOF

aarch64-linux-android34-clang -shared -fPIC test.c -o libtest.so
readelf -h libtest.so
# Output confirms: Class: ELF64, Machine: AArch64, Type: DYN (Shared object file)
```

---

## 5. Building & Testing `WindowsPixel` on `spark-20aa`

### A. Build & Run the Core Engine Unit Tests + Benchmarks (Linux ARM64)
```bash
PATH=$HOME/.local/bin:$PATH make -j$(nproc)
PATH=$HOME/.local/bin:$PATH make test-core
PATH=$HOME/.local/bin:$PATH make build/test_bench && ./build/test_bench
```

### B. Cross-Compile `libwinvm.so` & `libwinvm.a` for Android (Pixel 9 / API 34)
```bash
PATH=$HOME/.local/bin:$PATH make android-lib -j$(nproc)
file build/android/libwinvm.so build/android/libwinvm.a
```

### C. Using with CMake (`CMakeLists.txt`)
```bash
# Native Linux ARM64 build (with unit tests):
cmake -B build/cmake \
  -DCMAKE_C_COMPILER=$HOME/.local/bin/clang \
  -DCMAKE_ASM_COMPILER=$HOME/.local/bin/clang
cmake --build build/cmake -j$(nproc)
```
