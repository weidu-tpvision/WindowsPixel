# Code Review: WindowsPixel Linux/Android Port

**Date:** 2026-11-13
**Scope:** everything this fork changed from upstream WindowsiOS (`git diff 0071d69 HEAD`, excluding `third_party/`): the core engine port (counter scaling, dispatch, JIT region, disk), the Linux runner (`host/main_linux.c`), host services (`winvm.c`, `share.c`, `net_slirp.c`), devices (`acpi.c`, `fb_display.c`, `winvirt.c`), and the Android layer (JNI, Kotlin, GLES renderer, AAudio).

**Baseline:** the build was clean (no warnings outside the vendored `libslirp`) and all 7 `test-core` suites passed.

---

## Summary

| # | Severity | Area | Finding | Status |
|---|----------|------|---------|--------|
| 1 | **Critical** | `host/share.c` | A WebDAV MOVE/COPY onto the share root or a parent of the source deleted the whole shared folder | Fixed, reproduced |
| 2 | **High** | `host/main_linux.c` | The web viewer and VNC bound `0.0.0.0` with no authentication, while the log said `127.0.0.1` | Fixed |
| 3 | High | `host/main_linux.c` | VNC could read past the end of the guest framebuffer | Fixed |
| 4 | High | `host/main_linux.c` | The SIGINT/SIGTERM handler called non-async-signal-safe code (possible deadlock on Ctrl-C) | Fixed |
| 5 | High | `host/winvm.c` | A failed `hv_winvm_create` leaked the disk fd and its exclusive `flock` | Fixed |
| 6 | High | Android audio | The guest could reopen an AAudio stream after shutdown, so its callback used the freed `HvHda` | Fixed |
| 7 | High | Android JNI | Stop/Start/SetSurface were not serialized (double destroy, stale framebuffer given to the renderer) | Fixed |
| 8 | High | `devices/fb_android.c` | The renderer kept a stale `g_fb_disp` after shutdown (use-after-free) | Fixed |
| 9 | Medium | `core/dispatch.c` | The vCPU WFI sleep used a `CLOCK_REALTIME` deadline, so wall-clock steps stall idle vCPUs | Fixed |
| 10 | Medium | `host/main_linux.c` | A single-threaded HTTP server with blocking `recv` could be wedged by one idle client | Fixed |
| 11 | Medium | `host/main_linux.c` | VNC ignored `SetPixelFormat`, desynced on unknown messages, and busy-looped on incremental updates | Fixed |
| 12 | Medium | `devices/acpi.c` | An empty host-derived SMBIOS string corrupted the string set; `0xd87` was mislabelled X925 | Fixed |
| 13 | Medium | Android audio | There was no AAudio error callback, so audio went silent for good after a route change | Fixed |
| 14 | Medium | `MainActivity.kt` | The foreground service was started before the VM and then stopped if the boot failed (crash on API 31+) | Fixed |
| 15 | Medium | `MainActivity.kt` | Nothing was cleaned up after the guest shut down on its own (gigabytes held with no foreground service) | Fixed |
| 16 | Medium | `devices/fb_android.c` | `surfaceDestroyed` did not wait for an EGL bind already in progress | Fixed |
| 17 | Low | `devices/fb_android.c` | If EGL init failed the "thread started" flag stayed set (permanent black screen); every `surfaceChanged` recreated the EGL surface | Fixed |
| 18 | Low | `MainActivity.kt` | A synthesized Shift could stick in the guest | Fixed |
| 19 | Low | `MainActivity.kt` | The Stop button stayed active during a multi-second teardown | Fixed |
| 20 | Low | Android manifest | The service type is `specialUse` only, so the guest mic is silent in the background; `POST_NOTIFICATIONS` is never requested | **Open** |
| 21 | Low | Android audio | `AAudioStreamBuilder_openStream` runs on a vCPU thread inside an MMIO handler (stalls the guest) | **Open** |
| 22 | Low | `devices/fb_display.c` | ramfb accepts `stride >= width*2` and ignores `fourcc`; consumers assume 32 bpp (garbage image, but no out-of-bounds read) | **Open** |
| 23 | Low | `tests/test_devices.c` | Stale upstream test (`vmapple.h`, macOS path); does not build and no target runs it | **Open** |

---

## Findings and Fixes

### 1. WebDAV MOVE/COPY could wipe the entire shared folder (Critical)

**`host/share.c`, `do_move_copy()`.** When the destination exists and `Overwrite: T` (the WebDAV default) is set, the handler calls `remove_path(to)` before the rename or copy. Nothing checked how `to` relates to `from`:

- With `Destination: /`, `to` is the share root, so `remove_path` recursively deletes everything, the source included.
- When the destination is any ancestor of the source, the source is deleted along with it.
- When the destination is inside the source, a COPY recurses into its own output.

`decode_path()` rejected `..` but not `.` segments or `//`. That means `Destination: /./` would also have got past a plain prefix check.

**Reproduced** on the original code: one `MOVE /a/b` with `Destination: http://10.0.2.4/` deleted the shared directory itself.

**Fix:**
- A new `path_within(dir, path)` helper.
- `do_move_copy` returns 403 when the destination is an ancestor of the source, or lies inside it.
- `decode_path` collapses `//` and rejects `.` segments, so the path comparisons are exact.
- Tested with the patched code: the destructive requests get 403/400, and legitimate COPY/MOVE still succeed.

### 2. Web viewer and VNC exposed to the network without authentication (High)

The servers bound to `INADDR_ANY`, and VNC offered security type "None". On the running system `ss` showed `0.0.0.0:8080` and `0.0.0.0:5900`. Anyone on the LAN had full keyboard and mouse control of the Windows guest. The startup log claimed `127.0.0.1`, and the README tells users to use `ssh -L`, so loopback was clearly the intent.

**Fix:**
- A shared `listen_tcp()` helper binds to `g_listen_addr`, which defaults to `127.0.0.1`.
- A new `--listen <ipv4>` option overrides it, and a warning is printed when the address isn't loopback.
- `scripts/launch_spark.sh` accepts a `LISTEN` environment variable.
- The README explains the default.

### 3. VNC could read out of bounds (High)

Without DesktopSize, `cur_w`/`cur_h` were only updated when the new frame was smaller in *both* dimensions. Going from 1024×768 to 1280×720 kept `cur_h = 768` and read 48 rows past the end of the 720-row guest framebuffer.

**Fix:**
- `cli_w × cli_h` is the size the client believes in; it changes only through DesktopSize.
- Each update sends the overlap of client and guest sizes and pads the rest with black. It never reads beyond `fw × fh`.
- Pointer coordinates are normalized by the guest frame size, since pixels map 1:1 (clipped, not scaled).

### 4. Signal handler not async-signal-safe (High)

`on_sig` called `hv_winvm_stop()`, which signals condition variables and takes per-CPU mutexes. If the signal landed on a CPU thread while it held its `wake_mu`, the process deadlocked on Ctrl-C.

**Fix:** SIGINT and SIGTERM are blocked with `pthread_sigmask` before any thread exists, and a dedicated `signal_thread` takes them with `sigwait()`. A second signal calls `_exit(130)`. Verified that Ctrl-C now stops the VM cleanly.

### 5. `hv_winvm_create` failure leaked the disk and its `flock` (High)

Every error return after `hv_disk_open(&disk0)` returned without cleaning up, and `vm_created` stayed 0, so `hv_winvm_destroy` never closed disk0. On Android (a long-lived process) every retry then failed with "Cannot open primary NVMe disk image", because the leaked fd still held `LOCK_EX`.

**Fix:**
- All failures go through a single `fail_out:` path. It runs `hv_winvirt_destroy` on a partially built board, frees the machine and closes both disks.
- `hv_winvm_stop()` returns early when no VM exists, because the machine may already have been freed.
- `destroy` zeroes `m` after freeing it.

### 6. Audio use-after-free during teardown (High)

`hv_winvm_stop()` shut audio down while the vCPUs were still running. An HDA register write could then call `hda_start_out` and reopen a stream with `userData = hda`. `hv_winvirt_destroy` then freed `hda` while the AAudio callback thread was still using it.

**Fix:**
- The `g_audio_closed` flag is set by `hv_audio_shutdown` and cleared by `hv_audio_attach_hda`. While it is set, no stream can be opened.
- `hv_winvm_destroy` calls `hv_audio_shutdown()` again after the joins and before the board is freed.
- `audio_android.c` was refactored around an `AudioDir` struct, which removes the duplicated in/out code.

### 7. JNI VM lifecycle not serialized (High)

- `nativeStopVm` could run twice at once (two Stop taps during a long flush): a double `pthread_join` and a double free.
- `nativeSetSurface` could pass `board->fb` to the renderer while a stop was about to free it.

**Fix:** two locks.
- `g_op_mu` serializes whole start and stop operations. It can be held for seconds, so it is never taken on the UI thread.
- `g_life_mu` is held only briefly. It guards `g_surface_win` and a `g_display_live` flag, so the renderer gets `board->fb` only while it is valid.

The `GetStringUTFChars` calls were also gathered into a helper.

### 8. Stale framebuffer pointer in the Android renderer (High)

`hv_fb_android_shutdown` never cleared `g_fb_disp`, and `hv_fb_android_set_window` updated it only `if (fb)`. A `surfaceChanged` after Stop restarted the render thread on the freed display.

**Fix:** `shutdown` clears `g_fb_disp`, and `set_window` assigns it unconditionally.

### 9. vCPU WFI wait used the wall clock (Medium)

**`core/dispatch.c`, `wait_for_interrupt()`.** The Linux branch built an absolute `CLOCK_REALTIME` deadline for `pthread_cond_timedwait` on a default-clock condvar. A backwards NTP/chrony step stalled idle vCPUs for the size of the step.

**Fix:** on non-Apple hosts `wake_cv` is initialized with `pthread_condattr_setclock(CLOCK_MONOTONIC)` (`core/machine.c`), and the deadline uses `CLOCK_MONOTONIC`.

### 10. HTTP viewer could be wedged by one client (Medium)

Connections were handled one at a time in the accept loop with a blocking `recv` and no timeout. One client that connected and never sent anything froze the viewer for everyone, and keyboard macros that sleep stalled frames. An `accept()` failure such as `EMFILE` also caused a busy spin.

**Fix:**
- One detached thread per connection, with 10-second `SO_RCVTIMEO`/`SO_SNDTIMEO`.
- `accept_retry()` waits 100 ms on persistent errors.
- Sockets use `SOCK_CLOEXEC`.
- Verified that a stalled client no longer blocks `/frame.rgba` (served in 0.01 s).

### 11. VNC protocol robustness (Medium)

- **`SetPixelFormat` was ignored**, so clients that pick lower colour depths (TigerVNC auto mode, for example) showed garbage. Fix: true-colour 8/16/32 bpp in either byte order is converted per row. The native BGRX format still takes the zero-copy path, and unsupported formats close the connection. Verified with 16 bpp RGB565.
- **Unknown message types** were skipped one byte at a time, which desynced the stream. Fix: the connection is closed with a log message.
- **Incremental update requests** were answered with full raw frames immediately, an unbounded loop. Fix: paced to about 30 fps (verified at roughly 33 ms per update).
- The receive timeout is used only during the handshake, so an idle viewer is not dropped.

### 12. SMBIOS strings (Medium)

- `read_sys_str()` overwrote the default with `""` when the file existed but was empty, and Android's `__system_property_get` writes `""` for a missing property. An empty SMBIOS string reads as the string-set terminator, which shifts every later string index.
  **Fix:** `b_str()` substitutes `"Unknown"`, and `read_sys_str()` leaves the default untouched on an empty value.
- Part `0xd87` is **Cortex-A725**, not X925 (this GB10 host reports ten of each). Implementer `0x4e` is NVIDIA Denver/Carmel; Grace and GB10 report ARM Ltd cores.

### 13. No AAudio error callback (Medium)

When a stream was disconnected (headphones or Bluetooth), its callbacks stopped, but `running` stayed 1, so the stream was never reopened.

**Fix:** `error_cb` handles `AAUDIO_ERROR_DISCONNECTED` by starting a detached `reopen_thread`, since AAudio forbids reopening inside the callback. That thread closes and reopens the stream with the same parameters, but only if the dead stream is still current and audio isn't closed.

### 14. Foreground service started before the VM (Medium)

`VmForegroundService.start()` ran before `nativeStartVm`, and `stop()` ran right after if the start failed. Stopping a service between `startForegroundService()` and `startForeground()` throws on API 31+.

**Fix:** the service is started only after `nativeStartVm` succeeds.

### 15. No cleanup after the guest shuts down on its own (Medium)

When the guest powered off, the UI stopped the foreground service but never called `hv_winvm_destroy`. Guest RAM, the JIT region, disk locks, audio and SLIRP stayed allocated in a background process with no foreground protection.

**Fix:** the polling loop calls `nativeStopVm()` on IO before stopping the service.

### 16. `surfaceDestroyed` raced an EGL bind already in progress (Medium)

The render thread took `g_win`, unlocked, and only set `g_surface_bound = 1` once `egl_bind_window` returned. A `set_window(NULL)` in that gap saw `bound == 0` and returned at once, while the thread went on to bind and draw on the destroyed Surface.

**Fix:** `g_surface_bound` is set under the lock before unlocking, and cleared (with a broadcast) if the bind fails.

### 17–19. Smaller Android fixes (Low)

- **EGL init failure:** the thread now clears `g_thread_started` and detaches itself, so the next `set_window` retries.
- **`surfaceChanged`:** `set_window` with the same `ANativeWindow*` no longer forces an EGL surface rebuild.
- **Stuck Shift:** a `shiftedUsages` set records which keys got a synthesized Shift, and that Shift is released on the key's up even if the IME has already dropped its meta state.
- **Stop button:** a `stopping` state disables it and shows "Stopping...".

---

## Second-Pass Review & Hardening (2026-11-27)

All 6 items from the initial "Open Items" list plus additional cross-platform bugs uncovered during the full-repo audit have now been resolved:

1. **`.hvsparse` protection & Android unpack UI (`C1`):** `hv_disk_open()` now rejects `"HVSPARSE"` stream headers so packed archives can never be mounted raw or corrupted; `MainActivity.kt` + `winvm_jni.c` unpack `.hvsparse` to `windows11.img` with a progress bar before booting, flush deferred guest disk writes in `onStop()`, and retry `EINTR` across `disk.c` and `hvsparse.c`.
2. **Android 15/16 16 KB page alignment & ART signal safety (`C2`, `C3`):** Added `-Wl,-z,max-page-size=16384` and `-fno-sanitize=shadow-call-stack` across all Android build targets (`0x4000` `PT_LOAD` alignment verified via `readelf -l`), rounded `hv_jit_region_open()` to `sysconf(_SC_PAGESIZE)`, and guarded `SIGSEGV`/`SIGBUS`/`SIGILL` installation behind `#ifndef __ANDROID__` + `sig_handlers_installed` so ART's `libsigchain` and `debuggerd` stay intact.
3. **Async AAudio worker & Android 14+ permissions/input (`M5`–`M7`, Open Items #1 & #2):** Moved blocking `AAudioStreamBuilder_openStream()` and stream reopen onto a dedicated joinable worker thread (`host/audio_android.c`), added `FOREGROUND_SERVICE_MICROPHONE` + permission-gated `FOREGROUND_SERVICE_TYPE_MICROPHONE`, requested `POST_NOTIFICATIONS` on API 33+, passed volume keys through to Android, and expanded hardware keyboard HID mappings.
4. **Dual-map JIT cache coherency, SMP `TLBI` synchronization & `PMCCNTR_EL0` (`H1`, `H2`, Open Item #6):** `hv_jit_finish()` now cleans the dirty `rw` `memfd` alias before invalidating `rx`; `hv_tlbi_drop_remote_va()` checks low-56-bit tag matches (avoiding false conflict evictions on remote vCPUs) and returns `-1` when a target vCPU holds `va` in its live TLB or active IJC so `hv_cpu_post_tlbi` + `hv_cpu_sync` retires any in-flight inline TLB access before `TLBI ... IS` completes; inline `PMCCNTR_EL0` now subtracts `hv_cnt_base`.
5. **Lifecycle memory/FD leaks, framebuffer safety, SMBIOS & `test_devices` (`H3`, `H4`, `M1`–`M4`, `M8`, Open Items #3–#5):** Freed `HvRam.whint` (~75 MB on Linux / ~37 MB on Android per VM stop/start), `HvNvme.io_buf`, `HvFwCfg` files/directory, `HvHda` rings, `HvVirtioSnd` ring, and `prepare_cpu_thread` `sigaltstack`; made `prefault_thread` and SLIRP IPv6 loopback bridges joinable on teardown; initialized `wake_pipe = {-1, -1}`; serialized HTTP/VNC/PNG framebuffer snapshots via `hv_winvm_copy_frame_bgrx()` and enforced `stride >= width * 4` in `ramfb`; switched `share.c` clipboard wait to `CLOCK_MONOTONIC` + `MSG_NOSIGNAL`; added SMBIOS Type 17 `Extended Size` (`>= 32 GB` RAM); and ported `tests/test_devices.c` to `HvWinVirt` as part of `make test-core`.

### Remaining Hardware Validation & Future Optimization
- **Physical Pixel 9 run:** Verify cold `.hvsparse` unpack, UEFI $\to$ Windows 11 desktop boot, GLES 120 Hz touch/IME input, and AAudio speaker/mic on device.
- **DGX Spark stress verification:** Re-run `./scripts/trace_freeze.sh run` under multi-tab Edge memory pressure to confirm the SMP `TLBI` fix eliminates the fault livelock in long sessions.
- **Performance:** Move NVMe `pread`/`pwrite`/`fsync` off vCPU threads (`dev_mu`) and shard or reduce `tr_mu` contention.

---

## Verification

- `make -j16`: clean, no warnings outside `third_party/`.
- `make test-core`: all 7 suites pass (classify 43, operands 36, jit 10×2, mem 12×3+mmio, mmu 76, exc 53, jit_region).
- `make build/test_bench`: builds.
- `make android-lib`: `libwinvm.{a,so}` and `libwinvm_jni.so` build cleanly with NDK API 34.
- `./gradlew --offline assembleDebug -PusePrebuiltJni=true`: BUILD SUCCESSFUL, so the Kotlin changes compile.
- **Runtime smoke test**, UEFI only, using a copy of `vars.fd` and no Windows disk:
  - the listeners are on `127.0.0.1`;
  - HTTP index and frame requests work while a stalled client is connected;
  - VNC works at 32 bpp and 16 bpp, with pacing;
  - an unknown VNC message closes the connection;
  - Ctrl-C stops the VM cleanly ("stopped (interrupted)").
- **WebDAV guard:** tested over the share's Unix socket, and the data-loss bug was reproduced on the original code in a temporary worktree.
- **Not tested on a device:** none of the Android runtime changes have run on a Pixel. They are only compile-checked.

---

## Freeze Investigation (DGX Spark, 2026-11-13)

**Symptom:** Windows intermittently stops responding after boot, or during Start → Shut down. It doesn't happen every run, which is what makes it hard to reproduce. It has been seen at both 4 and 8 vCPUs.

### What the evidence rules out

| Hypothesis | Test | Result |
|---|---|---|
| **vCPU count** (`--cpus 8` → translation-lock contention, from an earlier note) | Freeze reproduced at **4 vCPUs** (`work/winvm_stats.log`, `work/winvm_stats3.log`) | **Not the cause.** More vCPUs add overhead, but the freeze also happens at 4. Note that waiting on the translation lock already averages 23.5% of one CPU at 4 vCPUs, peaking at 54%; that's a performance issue in its own right. |
| **Synchronous `fsync` on NVMe FLUSH under the device lock** (a vCPU blocked in I/O stalls every device access and timer tick) | Whole session run under `strace -f --seccomp-bpf -T -e trace=fsync,fdatasync` via `scripts/trace_freeze.sh run`, including a freeze | **Not the cause.** All 955 flushes completed in **≤ 16 ms**, and none took more than 0.5 s. |
| **A lock or I/O deadlock in the engine** | Per-thread state from `/proc/<pid>/task/*/stat` and `wchan`, sampled twice 2 s apart during the freeze | **Not the cause.** All 4 vCPU threads were in state `R` at **100% user CPU** (about 200 ticks per 2 s each). The service threads (SLIRP poll, HTTP/VNC accept, kicker, signal thread) were idle as normal. |
| **RAM too small** (94% guest memory use seen before a freeze) | Reasoning, plus the fault signature below | **A likely trigger, not the cause.** Memory pressure makes Windows trim and re-fault pages and rewrite page tables far more often, which exercises the failing path. But a correctly emulated CPU slows down under pressure; it doesn't livelock. Running with 8 GB is a reasonable workaround until it's fixed. |

### What the freeze actually is: a page-fault livelock

During the freeze (`work/winvm_stats3.log`, t ≈ 280–313 s, 4 vCPUs):

- **About 2.45 million exceptions per second**, against a normal 20–130K/s. That's the "~2M exc/s" loop noted before.
- **The same exception every time:**

  ```
  last exc: esr=96000005 (ec=25) far=150f204 elr=fffff803ffa2e058 spsr=80000044
  ```

  - `EC 0x25`: a data abort taken from EL1 (kernel mode).
  - `ISS`: a read, `DFSC 0x05` (translation fault, level 1); one sample showed `0x06` (level 2).
  - `FAR 0x150f204`: a **user-space address**, read by kernel code at `fffff803ffa2e050`/`…058`.
  - vCPUs 1 and 3 were parked on that exact instruction; the others were nearby.
- **The engine counters match a fault-and-retry loop:**
  - EL1 software-TLB misses: 2,477K/s, the same as the exception rate;
  - slow-path loads where a TLB entry exists but the tag doesn't match: 1,181K/s;
  - SVC/ERET resumed in place: 4.2M/s;
  - WFI: 0/s, with every vCPU 100% busy.

**Interpretation:** Windows' page-fault handler runs, appears to resolve the fault, and returns with ERET. The same instruction re-executes and faults again at the same address, millions of times a second. Because the reported fault level moves between L1 and L2, the guest's page tables are being filled in, yet the engine's walk still fails. That points to the engine walking or caching something different from what the guest just fixed. The candidates are:

1. **Stale translation state.** A software-TLB entry, an ASID-keyed `HvTlbStash` entry, or a stash filter survives an update the guest made, perhaps because a TLBI was raced or missed, or a restore came from the wrong process's stash.
2. **The wrong address space.** The walk uses a TTBR0/ASID different from the one the handler fixed, for example during a temporary process attach. The low address (`0x150f204`) is typical of a 32-bit or emulated x86 process.
3. **A genuine guest-side loop.** Unlikely for Windows, but not ruled out until the guest's page tables have been read at the moment of the loop.

### Diagnostics added

- **A fault-loop detector** in `core/exception.c` (`hv_raise_abort` → `fault_loop_track`):
  - It has no effect on normal runs, and its per-vCPU state lives in a static array, so the fixed `HvCpu` layout is unchanged.
  - When one vCPU takes **20,000 identical aborts in a row** (same PC and address, same fault class, any level), it prints, once per loop:
    - the faulting instruction word, DAIF/PAN, and ELR/SPSR;
    - SCTLR, TCR, both TTBRs, and the active ASID;
    - **every page-table descriptor for the faulting address, read directly from guest RAM**, level by level;
    - the result of a **fresh `hv_mmu_walk`** as EL1 and as EL0.
  - It prints again after 80,000 repeats, and once more when the loop ends.
  - With `HV_FAULT_LOOP_FLUSH=1`, on detection it also performs a local `TLBI VMALLE1` (the guest's own full-flush operation) and reports whether the loop then stops.
  - Unit-checked with a synthetic 20,000-abort sequence.
  - **How to read the result:**
    - the guest's descriptors are valid but the fresh walk fails → a walker bug;
    - the descriptors and the fresh walk are both valid, and the flush stops the loop → stale TLB or stash state;
    - the descriptors themselves are invalid → the guest really lacks the mapping, so look at what Windows expected (a fault the engine reports wrongly, or a missed TLBI).
- **`scripts/trace_freeze.sh`:**
  - `run`: starts the VM (4 vCPUs, 4608 MB) under `strace` recording flush durations, plus `--stats 5`;
  - `bt`: per-thread state from `/proc`, plus `gdb` backtraces when no tracer is attached (only one tracer can attach, and under `run` that's `strace`);
  - `report`: the slowest flushes and any gaps in the stats output.

### A false alarm during testing

One "freeze" during testing (a dark screen with a moving pointer) turned out **not** to be the livelock. It happened after the user started a Restart and then cancelled it at the "apps are preventing restart" screen. Windows had already closed `explorer.exe` and didn't relaunch it.

- **Engine state was healthy:** all vCPU threads asleep in WFI waits, no fault loop, and the exception rate down to about 1K/s.
- **The guest framebuffer** showed Windows' own empty session screen with its pointer.
- **Recovery:**
  - Ctrl+Shift+Esc did nothing, because the desktop shell handles that shortcut and it was gone;
  - an injected Ctrl+Alt+Del (`/input?t=a&a=cad`) brought up the security screen, so Windows was responsive;
  - the restart then completed cleanly ("guest requested PSCI SYSTEM_RESET — rebooting...") and Windows booted normally.
- **Lesson:** "screen dark, mouse moves" is not a reliable freeze signal. Check whether the vCPU threads are spinning (the `bt` state dump) and the exception rate in the stats.

### Root-cause analysis & fixes applied (2026-11-27)

Closer inspection of the counters from `work/winvm_stats3.log` against `core/mmu.c` and `core/sysreg.c` pinpoints how the livelock occurs:

1. **What the 2:1 `exceptions` vs `slow_why[5]` ratio proves:**
   - During the freeze, `exceptions` ran at **2,450K/s**, `SVC/ERET resumed in place` at **4.2M/s** (~800 ns per full fault + `MmAccessFault` + `ERET` round-trip), and `slow_why[5]` (`entry-but-tag`) ran at **1,181K/s** — almost Move-for-Move **half** of `2,450K/s` (`96.4%`).
   - In `hv_tlb_refill()` (`core/mmu.c`), `slow_why[5]` is incremented via `last_refill_key` when the **exact same `(va, flags)`** misses twice in a row with **zero intervening `hv_tlb_refill()` calls** (`Fault #1` sets `last_refill_key = rkey`; `Fault #2` sees `last_refill_key == rkey`, resets it to `0`, and bumps `slow_why[5]++`).
   - That 2:1 ping-pong proves two things simultaneously:
     1. Inside `MmAccessFault(0x150f204)`, Windows took **zero software-TLB misses** on 96%+ of iterations: every read `MmAccessFault` made to inspect its self-referential page tables (`MmPteBase`, `0xFFFFF680...` in `TTBR1_EL1`) **hit in `cpu->tlb[1]`**.
     2. At ~800 ns per round-trip, `MmAccessFault` was taking its lock-free *"PTE/PDE/PPE already valid $\to$ spurious TLB fault $\to$ `return STATUS_SUCCESS`"* early exit after reading `MmPteBase` through `cpu->tlb[1]`, while `hv_mmu_walk()` walking guest physical RAM from `cpu->ttbr0_el1` saw `L1` (`DFSC 0x05`) or `L2` (`DFSC 0x06`) invalid.

2. **Two engine bugs that produce that exact mismatch (both now fixed):**
   - **Bug A — Unsynchronized SMP remote `TLBI` shootdown (`hv_tlbi_drop_remote_va` in `core/mmu.c`, fixed in `1dfbb4b`):**
     Under heavy RAM pressure (94% of 4.5 GB) or during Start $\to$ Shut down (mass process teardown across 4 vCPUs), Windows's working-set trimmer constantly unmaps user pages and **reclaims L3/L2 page-table pages**, unmapping both the user VA and the self-map `MmPteBase` kernel VA (`0xFFFFF680...`) and issuing `TLBI ... IS` + `DSB ISH`.
     Previously, `hv_tlbi_drop_remote_va()` wrote `HV_TLB_INVALID` to `target->tlb` and **returned `1` immediately without waiting for `target`**, even while `target->pc_state->tlb_live == 1` (and even when `fetched == 1`, it only called `hv_cpu_kick()` without `hv_cpu_sync()`). A remote vCPU that had *just* passed its inline `CBNZ U` TLB tag check in JIT code (especially if preempted by the Linux host scheduler for a few microseconds before the host load/store/atomic) completed its memory operation against the **old physical page** *after* `TLBI ... IS` + `DSB ISH` had returned and Windows recycled/reallocated that page as a page table or MM structure.
     **Fix:** `hv_tlbi_drop_remote_va()` now checks low-56-bit tag matches (avoiding false conflict evictions on remote vCPUs) and returns `-1` whenever `target` holds `va` in its live `cpu->tlb` or active IJC, forcing `hv_cpu_post_tlbi` + `hv_cpu_sync` so `target` exits its translated block and retires all in-flight inline TLB accesses before `TLBI ... IS` completes.
   - **Bug B — `va + len` wraparound at $2^{64}$ / $2^{56}$ on top-of-`TTBR1` range `TLBI`s (`core/mmu.c`, `core/sysreg.c`):**
     In both `hv_tlbi_apply()` (`p < va + len`) and `hv_tlb_flush_kernel_range()` (`p < va + len` for $\le 16\text{ MB}$ ranges, and `hi = (va + len) & 0x00ffffffffffffffull` for $> 16\text{ MB}$ ranges), any kernel `TLBI RVAE1IS` reaching the top of the 64-bit address space (`va + len == 0` modulo $2^{64}$) wrapped the upper bound to **`0`**, turning `p < 0` / `pg < 0` into a **silent no-op** and leaving stale `TTBR1` kernel TLB entries behind in `cpu->tlb[1]`.
     **Fix:** Rewrote both range loops and the big-range `[lo, hi)` bounds in terms of `span = (va - first) + len` and `hi = lo + span` (`lo < 2^{56}`, `span <= 2^{37}`), eliminating 64-bit and 56-bit wraparound completely (covered by unit tests in `tests/test_mmu.c`).

### Verification steps on DGX Spark

1. Run `./scripts/trace_freeze.sh run` (or `HV_FAULT_LOOP_FLUSH=1 ./scripts/trace_freeze.sh run`) under heavy memory pressure (Edge with multiple tabs, Store, Settings at 4.5 GB, followed by Start $\to$ Shut down) to confirm the livelock no longer reproduces with both fixes in place.
2. If `FAULT LOOP` ever triggers again, the built-in detector in `core/exception.c` logs the exact faulting instruction word, `TTBR0`/`TTBR1`, L0–L3 physical descriptors, and whether `HV_FAULT_LOOP_FLUSH=1` (`TLBI VMALLE1`) clears it.
3. Independent of correctness, moving NVMe `pread`/`pwrite`/`fsync` off vCPU threads (`dev_mu`) and reducing `tr_mu` contention remain worthwhile throughput optimizations.

---

## Appendix: How the Repo Works

### What it is

WindowsPixel runs **unmodified Windows 11 24H2 ARM64** as a guest on an ARM64 host that has **no hardware virtualization available to apps**. Neither iOS, a non-rooted Android phone, nor a Linux box without KVM lets an app run guest code at EL1, so `winvm` does not use a hypervisor at all. It is a **system emulator built around an ARM64→ARM64 dynamic binary translator (JIT)**:

- The guest and host share an instruction set, so most guest instructions are **copied verbatim** into host code.
- The translator rewrites only what can't run as-is in a user process:
  - memory accesses, which need guest-VA → host-address translation;
  - branches;
  - PC-relative instructions;
  - privileged and system instructions;
  - the few registers the engine reserves for itself.
- Everything a real PC would supply is emulated in C: CPU state at EL0/EL1, a software MMU, the interrupt controller, timers, firmware interfaces and PCIe devices.

The code descends from **WindowsiOS**, which itself builds on the **HengeVM** engine (originally a macOS-guest VM for iOS). The `Hv*` and `hv_*` names come from HengeVM.

```
 Windows 11 ARM64 (ntoskrnl, drivers, apps)     ← guest, believes it runs on a Cortex-A76 PC
 EDK2 ArmVirtQemu UEFI (code.fd + vars.fd)      ← unmodified QEMU "virt" firmware
 ───────────────────────────────────────────────────────────────────────────────
 devices/   virt board: GICv3+ITS, timers, PL011/PL031/PL061, fw_cfg+ramfb, pflash,
            PCIe ECAM: NVMe, xHCI (kbd+tablet), Intel HDA, virtio-net; ACPI+SMBIOS
 core/      JIT translator, dispatcher, software MMU/TLB, exceptions, sysregs, PSCI
 host/      thread/JIT-memory/disk/network/audio/UI glue per OS (iOS, macOS, Linux, Android)
 ───────────────────────────────────────────────────────────────────────────────
 Host OS process (EL0), one host thread per vCPU
```

### Repository map

| Directory | Contents |
|---|---|
| `core/` | The engine. `classify.c` sorts A64 instructions into kinds (copy, memory, branch, sysreg, …). `operands.c` finds register fields. `translate.c` (2.2K lines) emits host code. `dispatch.c` is the run loop, block cache, chaining, WFI/WFE, stats. `mmu.c` holds the stage-1 walker and software TLB. `memop.c` decodes loads/stores and runs the slow path. `exception.c` covers exception entry/ERET and PSCI. `sysreg.c` covers system registers, the counter and TLBI. `machine.c` has the guest physical map, per-CPU state, cross-CPU requests and locks. |
| `host/` | `enter.S` (entry/exit/helper thunks into JIT code), `jit_region.c` (executable memory), `winvm.c` (VM lifecycle API shared by all front ends), `disk.c`/`hvsparse.c` (raw and sparse disk images), `net_slirp.c` (user-mode NAT), `share.c` (WebDAV share and clipboard/OAuth bridge), the audio backends, and the front ends `main_mac.m` and `main_linux.c`. |
| `devices/` | The virtual board (`winvirt.c`) and every device model, plus the display backends `fb_metal.m` (Apple) and `fb_android.c` (GLES). |
| `ios/` | SwiftUI app, the Objective-C bridge, and the TXM/debugger JIT bootstrap. |
| `android/` | Kotlin/Compose app, foreground service, and the JNI bridge. |
| `tools/` | `hvsparse` packer, `WinIOSBrowser.exe` (an ARM64 Win32 OAuth relay inside the guest), an offline registry hive tool. |
| `scripts/` | Windows provisioning (WinPE + DISM), image slimming, iOS deploy/LLDB launchers, `launch_spark.sh`. |
| `tests/` | Assembly guest programs run through the real engine (`test_jit`, `test_mem`, `test_mmu`, `test_exc`, …). |

---

### 1. The CPU: an ARM64→ARM64 dynamic binary translator

#### 1.1 Guest state and register mapping (`core/cpu.h`, `host/enter.S`)

- Each vCPU is an `HvCpu` struct with a fixed layout, so JIT code and `enter.S` address fields by constant offset:
  - x0–x30, SP, PC, NZCV, V0–V31, FPCR/FPSR;
  - EL1 system registers;
  - the software TLB, which is placed inside the struct, plus return tables and cache tags.
- While a block runs, **host x28 points to the `HvCpu`**. Most guest general registers live in the *same-numbered host register*, so a copied instruction like `add x3, x4, x5` needs no change at all.
- **Reserved registers:**
  - Guest **x18** (reserved by Darwin), **x28** (the `HvCpu` pointer) and **SP** (the host stack switches under it) live in the struct. Instructions that name them are rewritten to use stand-in scratch registers.
  - Guest **x6, x7 and x15** also live in the struct. They are the least-used registers in compiled code, so host x6/x7/x15 become free scratch that the translator never needs to spill.
- `hv_enter` saves the host's callee-saved registers, loads the guest registers and branches into a block.
- Blocks leave through `cpu->exit_routine` (`hv_exit`), or call C *without leaving* through `hv_helper_entry`/`hv_helper_thunk`. Those save the rest of the state, run `hv_helper(cpu, info)` on the host stack, then either resume the block or exit to the dispatcher.

#### 1.2 Translation (`core/translate.c`)

A **block** is guest code from an entry PC up to a branch, a system instruction, or a page boundary. For each instruction, `classify.c` decides how it is handled:

| Guest instruction kind | Host code emitted |
|---|---|
| ALU and SIMD/FP | **Copied verbatim**, with register fields remapped only when they name x18/x28/SP |
| `ADR`/`ADRP`/`LDR literal` | Replaced by a constant load: the guest PC is known at translation time |
| Loads/stores | Inline software-TLB check, then the host access; out-of-line slow path (§1.4) |
| `LDXR`/`STXR` exclusives | QEMU-style: LDXR records address and value, STXR becomes a host `CASAL` against it. LDXP/STXP pairs use native `CASPAL`. |
| LSE atomics (`CAS`, `LDADD`, …) | The host's own atomics on the translated address |
| `B`/`BL`/`B.cond`/`CBZ`/`TBZ` | Chainable exits (§1.3) |
| `BR`/`BLR`/`RET` | Inline lookup in a per-CPU **indirect-jump cache (IJC)**; on a miss, exit to the dispatcher |
| `BL` + `RET` pairs | Host `BL`/`RET` through a return table keyed by guest return address, so the host's return-address predictor works for guest code |
| `MRS`/`MSR` of plain-storage sysregs, NZCV/FPCR/FPSR, DAIF/SPSel/PAN | Inline |
| Counter reads (`CNTVCT`/`CNTPCT`) | Inline host counter read, scaled (§3.1) |
| `DC ZVA`, `IC IVAU` | Inline fast paths (both run millions of times per second in Windows) |
| `SVC`, `HVC`, `ERET`, `WFI`/`WFE`, TLBI, MMU/timer/GIC sysregs | Call into the C helper |
| PAC instructions | Executed as no-ops or pointer strips: the guest is told there is no PAuth |

Layout details:
- Each block has a **hot part**, written from the bottom of a 1 MB chunk, and a **cold part** (unchained exits, slow paths, literal pool) written from the top. Code that actually runs therefore sits together in the instruction cache and branch predictors.
- Code is built in a static buffer and copied once into the JIT region.
- **Group optimization:** several loads/stores off the same base register do one TLB check for the range they cover, then reuse the host addend.

#### 1.3 Dispatch, block cache and chaining (`core/dispatch.c`)

- `run_loop()` looks up the block for `(VA, PA, flags)`, where the flags encode EL1 vs EL0, FP trap, SPSel and PAN, in a global hash.
  - The lookup is lock-free, because blocks are only ever added.
  - Translation itself runs without the lock; only publishing takes `tr_mu`.
- **Chaining:**
  - When a block leaves through a direct-branch exit, the dispatcher stores the target block's entry in that exit's **slot**. The exit is plain data, so code is never rewritten.
  - Next time, the exit jumps straight to the target if `cpu->exit_req` is clear.
  - Same-page chains are always valid. Cross-page chains first check that the target page's TLB entry still has the same host addend.
- `exit_req` is the asynchronous-event mechanism. A **kicker thread** sets it on every vCPU every 1 ms (2.5 ms on mobile), and interrupts and TLBIs set it on their target. Chained loops always contain at least one checked edge, so a CPU that is asked to leave does so promptly.
- **Invalidation:**
  - When guest code writes a page that contains translated code (tracked per page in `HvRam.code_pages`), that page's generation (`page_gen`) is bumped, and stale blocks fail the lookup check.
  - When the JIT region fills, a **stop-the-world flush** parks every CPU and drops all translations.
- `fast_resume()` lets SVC and ERET continue directly into the cached next block without a dispatcher round trip.

#### 1.4 Software MMU (`core/mmu.c`, `core/memop.c`)

- There is **no stage 2**: the engine plays hypervisor. `hv_mmu_walk` implements VMSAv8-64 stage-1 for EL1&0:
  - 4K, 16K and 64K granules, 48-bit VA/PA;
  - TBI, PAN, HPD, ASIDs.
- **Software TLB:** 16,384 entries for each of EL0, EL1 and EL1+PAN. Each entry holds `read_tag`, `write_tag`, `exec_tag` and `addend` (host = guest VA + addend).
- **The inline JIT check** is roughly: `idx = (va >> 12) & mask; if ((va - tag) >> 12 == 0) host = va + addend`. Tags are biased by 15 bytes, so a single subtract-and-shift also proves an access of up to 16 bytes doesn't cross the page.
- **On a miss:**
  1. `hv_helper_thunk` tries a **light refill**, `hv_tlb_refill`. It is called straight from JIT code with guest SIMD registers still live, which is why `mmu.c` is built with `-mgeneral-regs-only`.
  2. If that fails, `hv_mem_emulate` performs the whole instruction in C. That covers MMIO, faults, page-crossing accesses and code-page writes.
- **Process switches:** an ASID-keyed **TLB stash** (`HvTlbStash`) saves and restores the TLB contents for each address space, with 2 MB/1 GB filters, so Windows' frequent context switches don't start from an empty TLB.
- **TLBIs** are broadcast to the other vCPUs as requests acknowledged by sequence number (`hv_cpu_sync`). A parked CPU (asleep, off) doesn't need to acknowledge.

#### 1.5 Exceptions, interrupts, EL model (`core/exception.c`, `core/sysreg.c`)

- Only EL0 and EL1 exist. `hv_take_exception` writes `ESR`/`FAR`/`ELR`/`SPSR_EL1`, switches EL and SP bank, and jumps to `VBAR_EL1` plus the architectural offset. `ERET` restores the saved state.
- The engine acts as EL2 for **PSCI over HVC**: CPU_ON/OFF, SYSTEM_RESET/OFF.
- System registers present a **Cortex-A76 r4p1** (MIDR) with a curated set of ID registers:
  - included: LSE, LRCPC2, DotProd, AES/SHA, and so on;
  - excluded: PAuth and SVE.
  Every advertised feature exists on the host, so copied instructions always run.
- **Interrupts:** the GIC model raises a per-CPU `irq_line`/`fiq_line`. Checked exits and the dispatcher deliver the IRQ when `PSTATE.I` allows.
- **Timers** (`devices/timer.c`): `CNTV`/`CNTP` compare values are checked against `hv_counter()`. The next deadline sets `cpu->timer_due`, which the run loop polls.

#### 1.6 SMP model

- **Each vCPU is a host pthread** running `hv_run()` concurrently. There is an optional big lock (`HV_BIG_LOCK=1`), but by default execution is truly parallel. Guest memory is shared host memory, and guest atomics are host atomics.
- **CPU 0** also polls devices through `m->tick`.
- **Device MMIO** is serialized by `dev_mu`, a recursive mutex; the GIC takes its own lock instead.
- **WFI** sleeps on a per-CPU condition variable for up to 10 ms, or until the next timer deadline or a wake.
- **WFE** in a spin loop sleeps on the host's own `WFE`, watching the host copy of the lock word, instead of returning to the dispatcher.

---

### 2. The virtual machine (`devices/`, `host/winvm.c`)

`hv_winvirt_create` builds a board compatible with QEMU's `virt` machine, so the stock **EDK2 ArmVirtQemu** firmware boots unmodified:

| Component | Implementation |
|---|---|
| Firmware | `pflash0` = `code.fd` (mapped read-only as RAM), `pflash1` = `vars.fd` (CFI01 flash, persistent NVRAM) |
| `fw_cfg` | DMA interface, used by EDK2 to fetch the ACPI tables, SMBIOS and the `ramfb` config |
| ACPI (`acpi.c`) | Generated in-process and delivered through QEMU's `etc/table-loader` protocol: RSDP/XSDT, FADT (PSCI), MADT, GTDT, MCFG, IORT (ITS), PPTT, SPCR, DBG2, DSDT (GPEX PCIe host, GED power button) |
| Display | `ramfb`: the guest writes a framebuffer config through `fw_cfg`, and the host reads BGRX pixels straight from guest RAM. Windows uses it through its basic display driver. |
| Interrupts | GICv3 distributor and per-CPU redistributors, plus an **ITS for LPIs**, so PCIe MSI-X works |
| UART/RTC/GPIO | PL011, PL031, PL061 |
| PCIe | ECAM + GPEX: **NVMe** (Windows `StorNVMe`, inbox), **xHCI** with a HID keyboard and absolute tablet (inbox), **Intel HDA** with a codec (inbox), **virtio-net** (needs the Red Hat NetKVM driver) |

Every device on this list except virtio-net has an inbox driver in Windows 11 ARM64.

**`host/winvm.c`** is the lifecycle API every front end uses: `hv_winvm_create`/`start`/`run_cpu0`/`stop`/`destroy`, input injection (`key_hid`, `tablet`), the display and PNG, and the power button. It opens the disks, sets up the machine and JIT, builds the board, attaches SLIRP, audio and the share, and runs the kicker thread and the vCPU threads. PSCI `SYSTEM_RESET` performs a warm reboot inside `run_cpu0` without tearing down RAM or the JIT.

Host services:
- **`net_slirp.c`:** the virtio-net frames go to libslirp (user-mode NAT at 10.0.2.0/24) on its own thread. Two special cases:
  - Guest connections to **10.0.2.4:80** go to `share.c` through a Unix socket, not a TCP port.
  - Dynamic host-to-guest port forwards serve OAuth loopback callbacks.
- **`share.c`:** a small WebDAV server for the shared folder. Under `/.henge/` it carries the clipboard, `open-url` (the guest asks the host to open a URL natively), `expose-port` and `oauth-done`. The guest-side partner is `tools/winios_browser_arm64.c`, registered as Windows' HTTP handler.
- **Disks:** raw images or **hvsparse**. hvsparse is a simple sparse container ("HVSPARSE", a size, then data runs) that makes it practical to ship a 32+ GB Windows image to a phone.

---

### 3. How it was implemented for iOS (upstream WindowsiOS)

On iOS, code signing forbids writable-executable memory, and on iOS 26+ the **TXM (Trusted Execution Monitor)** enforces this even more strictly. Running a JIT there takes these steps:

1. **JIT memory from a debugger** (`ios/Sources/Native/winios_jit.c`, `winios_brk.S`, `Resources/henge-jit.js`, `scripts/_lldb_txm_jit.py`):
   - The app is launched under a debugger: StikDebug's script on device, or `scripts/launch_on_duo.sh`, which runs `devicectl --start-stopped` then LLDB.
   - It talks to the debugger through `brk` traps:
     - `brk #0x69` probes whether a debugger is answering;
     - `brk #0xf00d` with `x16=1` (`CMD_PREPARE_REGION`) asks debugserver to allocate an **RX region** (`_M<size>,rx`), which code signing then permits because the process is being debugged (`get-task-allow`);
     - `x16=0` detaches once the region exists.
   - The app makes a **writable alias** of the same pages with `vm_remap`. The JIT writes through `base + rw_diff` and executes at `base`. This is the "split" shape in `host/jit_region.c`.
   - iOS 26/27 TXM hangs on executable pages above `0x7000000000`. The app therefore reserves a 384 MiB zero-fill array in `__DATA` at launch and frees it just before debugserver allocates, so the RX region lands in that low hole.
2. **Guest RAM outside the jetsam footprint** (`hv_alloc_no_footprint` in `winvirt.c`): RAM is a Mach memory entry with `mach_memory_entry_ownership`. That keeps 4–4.5 GB of guest RAM from counting against the app's memory limit. It is pre-allocated before the JIT region because of the 36-bit user VA ceiling.
3. **Time:** the guest counter is `mach_absolute_time`, which already ticks at 24 MHz on Apple silicon. The JIT reads it inline the way libsystem does, using the counter register named by the commpage plus the commpage timebase offset.
4. **Host integration:**
   - display: Metal (`devices/fb_metal.m`, a `CAMetalLayer`);
   - audio: CoreAudio/AudioToolbox (`host/audio_mac.c`);
   - scheduling: QoS classes for vCPU threads (below UI priority), at most 4 vCPUs on iPhone;
   - deferred NVMe flushes on mobile flash.
5. **App** (`ios/Sources/*.swift`, `winios_bridge.m`): SwiftUI with a Surface-style laptop-deck UI and a hinge sensor for the foldable "iPhone Duo", the Metal display view and touch/keyboard → HID. OAuth URLs from the guest open in `SFSafariViewController`. libslirp and glib are cross-built by `scripts/build_ios_deps.sh`.
6. **macOS** (`host/main_mac.m`): the same engine as a desktop app, with `MAP_JIT` plus `pthread_jit_write_protect_np` instead of the debugger trick.

---

### 4. What was done to make it work on DGX Spark (Linux ARM64) — Stage A

The engine was written for Darwin. The port replaced every Apple dependency while keeping the Apple paths behind `#ifdef __APPLE__`:

| Area | Darwin (upstream) | Linux (this fork) |
|---|---|---|
| **Guest counter** (`core/sysreg.c`, `core/translate.c`) | `mach_absolute_time` (24 MHz), commpage offset in JIT | The host `CNTVCT_EL0` runs at **1 GHz** on GB10, but the guest is told 24 MHz. `counter()` computes `((vct - base) * mult) >> 60`, with `mult = 24 MHz·2^60 / host_freq`. The JIT emits the same thing inline: `mrs` → `sub base` → `mul` + `umulh` + `extr #60`. All dispatcher timing went from `mach_absolute_time()` to `hv_counter()`. |
| **JIT memory** (`host/jit_region.c`) | `MAP_JIT` / debugger RX + `vm_remap` | A `memfd_create` file mapped twice, **RX + RW** (W^X dual mapping), falling back to anonymous RWX when policy forbids exec on memfd. Cache maintenance uses `__builtin___clear_cache` instead of `sys_icache_invalidate`. |
| **Assembly** (`host/enter.S`, `tests/*.S`) | Mach-O `_symbol` names | A `SYM()` macro for ELF names, plus `.note.GNU-stack` |
| **Threads and timing** (`core/dispatch.c`) | `pthread_cond_timedwait_relative_np`, `thread_info` | Absolute-deadline `pthread_cond_timedwait` (monotonic since this review), and `CLOCK_THREAD_CPUTIME_ID` for the `--stats` CPU-busy figure |
| **Guest RAM** (`devices/winvirt.c`) | Ledger-exempt Mach memory | `mmap(MAP_NORESERVE)` + `madvise(MADV_HUGEPAGE)`; `hv_winvirt_destroy` for teardown |
| **Disk** (`host/disk.c`) | `F_BARRIERFSYNC` | `fdatasync`, plus an exclusive `flock` so two VMs can't open the same image |
| **Network** | libslirp from Homebrew/iOS deps | **Vendored libslirp 4.8.0** (`third_party/libslirp`, with a minimal `glib.h` shim) built into the binary; `hv_net_detach` for teardown |
| **Share** (`host/share.c`) | `copyfile`, `removefile`, `st_birthtimespec` | Recursive copy/remove written in C; `st_mtime`/`st_ctime` |
| **SMBIOS** (`devices/acpi.c`) | Hard-coded "Apple / iPhone" | Read from host DMI (`/sys/devices/virtual/dmi/id`) and `/proc/cpuinfo` (MIDR part number → core name) |
| **Front end** | Cocoa app (`main_mac.m`) | **`host/main_linux.c`**: a headless CLI with an HTML5 canvas viewer (`--web`), an RFB 3.8 VNC server (`--vnc`, DesktopSize), pure-C PNG export (`--screenshot`), a `--cmd-file` input script, `--timeout`, and `--stats` (extended with per-CPU PCs and the last exception, for freeze debugging) |
| **Display** | Metal | Nothing renders on the host: the viewers read the ramfb directly. `fb_display.c` gained the PNG encoder. |
| **Audio** | CoreAudio | None on Linux (HDA is present; audio is not wired to a host backend) |

The result is verified booting Windows 11 ARM64 on 4 vCPUs on the Spark. `scripts/launch_spark.sh` wraps the standard invocation.

### 5. What was done to make it work on Android (Pixel 9) — Stage B

Android is Linux underneath, so it reuses all of Stage A and adds:

- **Build:**
  - `make android-lib` cross-compiles the engine, devices and libslirp with the NDK at API 34 into `libwinvm.{a,so}` and `libwinvm_jni.so`;
  - `android/app/src/main/cpp/CMakeLists.txt` mirrors the Makefile for Gradle;
  - the Gradle wrapper can package the prebuilt JNI library (`-PusePrebuiltJni=true`), with a native ARM64 `aapt2`, all on the Spark.
  - `-fno-sanitize=shadow-call-stack` keeps the NDK's ShadowCallStack from claiming x18 in host code that runs alongside JIT code; `mmu.c` keeps `-mgeneral-regs-only`.
- **SELinux:** untrusted apps may be denied `execmem` on memfd, which is why `jit_region.c` falls back to anonymous RWX.
- **Scheduling:** `prepare_cpu_thread()` pins vCPU threads to cores **4–7** (Cortex-X4 + A720) with `sched_setaffinity`, keeping them off the in-order A520 efficiency cores. `HV_NO_PIN` disables this. `HV_MAX_CPUS` is 4, the kicker runs at 2.5 ms, and NVMe FLUSH fsyncs are deferred, as on iOS.
- **Display** (`devices/fb_android.c`):
  - A render thread owns an EGL/GLES 3.0 context on the app's `ANativeWindow`.
  - It uploads the ramfb as a texture, using `GL_TEXTURE_SWIZZLE` for BGRX→RGBA so no CPU conversion is needed, and draws an aspect-correct quad.
  - `surfaceDestroyed` is synchronous, as `SurfaceHolder` requires: the UI thread waits until the EGL surface is released.
- **Audio** (`host/audio_android.c`): **AAudio** low-latency 16-bit streams in both directions behind the same `HvHdaBackend` interface CoreAudio uses: guest HDA playback → speaker, microphone → guest capture. Since this review it also reconnects after route changes and is teardown-safe.
- **SMBIOS:** `ro.product.manufacturer`/`ro.product.model`/`ro.soc.model` from system properties.
- **Lifecycle:** `hv_winvm_destroy()` (new in the fork) joins every thread and frees RAM, the JIT, devices, SLIRP and disks, so the app can stop and restart a VM without killing the process. The JNI bridge serializes this since this review.
- **App** (`android/app/src/main/java/com/winpixel/app/`):
  - `MainActivity`: Jetpack Compose UI with a vCPU/RAM/sound picker and a toolbar (Win, Ctrl+Alt+Del, Power, Keyboard, Stop);
  - a `SurfaceView` display with touch → absolute tablet;
  - hardware and IME keys → HID usages;
  - guest `open-url` requests (OAuth) opened in **Chrome Custom Tabs**;
  - `VmForegroundService` (`specialUse`) keeps the process alive while Windows runs;
  - `WinVmBridge` declares the JNI functions.
- **Status:** the native libraries and debug APK build. Boot on a physical Pixel 9 has not been verified yet.

### 6. Life of a guest instruction (putting it together)

1. A vCPU thread in `run_loop()` needs the block at guest PC `0xfffff801…`. It takes the TLB/MMU walk for the fetch, the hash lookup and, on a miss, translation. That produces copied ALU instructions, inline TLB-checked loads/stores and chainable exits.
2. `hv_enter` jumps into the host code. Loads like `ldr x0, [x1, #8]` become a TLB tag check plus a host `ldr` on `x1 + addend`, all inside JIT code.
3. A direct branch exit jumps to the chained next block. An indirect branch hits the IJC. A return hits the return table.
4. When Windows writes `TTBR0_EL1` (a context switch), the block calls `hv_helper`. The helper stashes and restores the TLB for that ASID and continues.
5. A timer comparison or the kicker sets `exit_req`. At the next checked exit the block leaves. The dispatcher runs `m->tick` (timers, devices), sees `irq_line`, and `hv_take_exception` enters the guest's IRQ vector.
6. When the NVMe driver rings a doorbell, the store misses the TLB (MMIO). `hv_mem_emulate` calls the NVMe model under `dev_mu`, which does `pread`/`pwrite` on the image and raises an MSI-X LPI through the ITS.
7. Windows draws to its framebuffer in guest RAM. The web, VNC, Metal or GLES front end reads those pixels directly from the `ramfb` host pointer.
