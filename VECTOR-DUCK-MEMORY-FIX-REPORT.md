# Vector Duck Memory fix — full report

**Date of this report:** 21 August 2026  
**Checked again while writing:** Pixel 7a serial `32231JEHN18513`, Duck pid `10593`, live `module.prop` `vcanary-remap (3103-libart-memfd)` / `versionCode=3103`  
**Git branch:** `duck-bypass` at commit `6c71a30b` plus **uncommitted** remap and Windows string-header work  
**What this file is:** a plain-English record of how Vector stopped leaking hook memory to Duck Detector, what we changed, what we tried and threw away, and what we verified on the phone.

This report was written from:

- the real git diffs in this tree (not from memory)
- Duck Detector’s own C++ (local clone under `docs/duck/Duck-Detector-Refactoring/`, gitignored)
- LSPlant and Dobby source in this tree
- a live `adb` re-check of `/proc/10593/maps` and `/proc/10593/smaps` while writing

If a sentence here disagrees with git or with those maps, git and maps win.

---

## 1. Short answer

Duck Memory was red for two different physical reasons, both caused by Vector after it injects into a scoped app:

1. **Finding A.** Dobby put trampoline code on an **anonymous executable** page (empty path in `/proc/self/maps`). Duck calls that “Anonymous executable code” and scores it High.
2. **Finding B.** LSPlant Init uses Dobby to **patch ART C++ functions inside `libart.so`**. Writing those bytes on a file-backed `/apex/.../libart.so` page makes Linux copy-on-write (COW) that page. Duck then sees `Shared_Dirty` (High) and `Anonymous` (Review) on a **system** executable mapping.

We did **not** hide this with a Duck allowlist, a fake JIT name, BRENE, or SUSFS. We changed how Vector/Dobby **places and writes executable bytes**.

**On this Pixel 7a, with Duck in gpssetter’s Vector scope, that Memory card is clear.** Live maps still show the five old dirty `libart.so` pages replaced by unnamed `/memfd: (deleted)` at the **same virtual addresses**. Remaining `/apex/.../libart.so` executable VMAs have `Shared_Dirty=0` and `Anonymous=0`. There is no empty-path anonymous RX and no W+X mapping in Duck’s process.

That is the Memory problem. It is **not** a claim that the phone looks unrooted on every Duck card (Custom ROM and similar Info findings can still appear). gpssetter / DeviceMasker were **not** re-tested after the remap flash.

---

## 2. Locked product rules (why we fixed it this way)

These rules were already decided before the remap. The code follows them:

- Do **not** special-case Duck’s package name, UID, or process. Any in-process scanner in a module’s scope should see the same physics.
- Do **not** name the memfd like ART JIT (`memfd:jit-cache` and friends). Duck already allowlists those names; copying them is spoofing and will break if Duck changes.
- Do **not** use anonymous RX (`pathname` empty, `[anon]`, `[anon:]`, `/dev/zero`) as the **intended** design. Anonymous RX is only a last-chance fallback so `system_server` does not crash when executable memfd is denied.
- Do **not** leave pages writable and executable at the same time as the intended design (`W+X`). Dual-map writes go through a **separate RW alias**.
- Do **not** `mprotect` the Vector Magisk/KSU `.so` to plant trampolines. That caused SELinux `execmod` and leaked `/data/adb` in the AVC.
- Dirty executable pages under `/apex` or `/system` **are** Finding B. Dirty pages inside Vector’s own module `.so` were accepted as a worse trade-off, then abandoned because of `execmod`.
- Dobby `FATAL_LOG` / `CHECK_NOT_NULL` **only logs**. It does **not** abort. Returning a null exec page still becomes `memcpy` into address 0 and kills the process. Never hand Dobby a null exec page.
- Keep original log strings. Verbose logging and inline log traces default **off**.
- Kernel hide-maps (SUSFS `sus_map`, BRENE) do **not** hide anonymous RX or inline patches inside `libart.so`. We researched that and did not use it for Memory.

---

## 3. Device, Duck, and Vector install

| Item | Value |
|---|---|
| Phone | Pixel 7a, serial `32231JEHN18513` |
| OS | Android 16 |
| Root | KernelSU (`/data/adb/ksud`), ReZygisk |
| Vector manager host | parasitic in `com.android.shell` (`org.matrix.vector.manager`) |
| Root UI used to flash | ResuKisu (`com.resukisu.resukisu`) |
| Duck | `com.eltavine.duckdetector`, build `2026.08.20-10f67170a933` / 521 (fork by RENE) |
| Duck’s Vector scope | gpssetter (`com.android1500.gpssetter`) — Duck is **in** a module scope, so Vector injects into Duck |
| Live module id | `zygisk_vector` |
| Live `module.prop` | `version=vcanary-remap (3103-libart-memfd)`, `versionCode=3103` |
| Manager APK stamp | still `2.2 (1)` on this PC (local git has no `origin/master` count / tags). The **native `.so`** is the remap build. Do not use the manager version string to identify this flash. |

How we flashed the remap zip (this path worked):

ResuKisu → Modules → light-blue FAB → DocumentsUI Download → `Vector-vcanary-3103-remap-Release.zip` → Install → “Flash success” / “Module installed successfully!” → Reboot.

KSU CLI also works (`adb shell su -c '/data/adb/ksud module install <zip>'` then reboot). On this phone ResuKisu was enough.

The zip on the PC was first produced as `Vector-v2.2-1-Release.zip` because this clone’s `versionCode` is `1` (`git rev-list --count refs/remotes/origin/master` fails). Before flashing, `module.prop` inside the zip was set to `versionCode=3103` and the file was renamed `Vector-vcanary-3103-remap-Release.zip` so KernelSU would **replace** the previous `3102` module. That stamp is **not** produced by current Gradle on this machine. A clean `zipRelease` here still names the zip `Vector-v2.2-1-Release.zip`.

---

## 4. The problem in plain words

Vector is a Zygisk module. When an app is in a module’s scope, ReZygisk loads Vector’s `.so` into that app. Vector then:

1. Starts **LSPlant**, which uses **Dobby** to inline-hook ART C++ functions in `libart.so` (this is `lsplant::Init`, not the later Java `XposedBridge.hookMethod` trampolines).
2. Allocates small **executable stubs** (Dobby trampolines) so the original instructions can keep running.
3. Later, for each Java hook, LSPlant may allocate more executable trampolines.

Duck Detector, when it is itself in scope, is just another injected app. It reads `/proc/self/maps`, `/proc/self/smaps`, and `/proc/self/fd`. It does **not** try to understand ART. It scores **memory physics**.

So Vector can hide package names, binder names, and `XposedBridge` fields and still fail Memory, because the pages themselves look like a hook framework.

---

## 5. What Duck Memory actually scores

Source: `docs/duck/Duck-Detector-Refactoring/app/src/main/cpp/memory/` (local clone, not in Vector git).

### 5.1 Anonymous executable code (Finding A) — High

In `maps_anomaly_detector.cpp`, every executable mapping whose path is “anonymous” is High.

Anonymous path (`maps_reader.cpp` `is_anonymous_path`) means only:

- empty pathname
- `[anon]`
- starts with `[anon:`
- starts with `/dev/zero`

**Unnamed `/memfd: (deleted)` is not anonymous.** That is why empty-name memfd is allowed.

Duck also skips mappings it already treats as ART JIT / code-cache (`is_benign_art_code_cache_path`). We do **not** use those names.

### 5.2 Shared-dirty system code (Finding B, High) and anonymous pages on a system mapping (Review / Medium)

Same detector, on **smaps** rows that are executable and not JIT:

- If `Shared_Dirty > 0` **and** the path is a system path → High (“Shared-dirty system code”).
- If `Anonymous > 0` **and** the path is a system path → Medium (“Anonymous executable pages on system mapping”).

System path (`is_system_path`) is exactly:

- `/system/`
- `/system_ext/`
- `/vendor/`
- `/product/`
- `/odm/`
- `/apex/`

Duck’s Memory detector does **not** use `Private_Dirty`. (A **different** Duck probe, Zygisk `smaps_probe.cpp`, does look at Private_Dirty + Shared_Dirty, but only on `/system/`, `/vendor/`, `/apex/`, and only if the sum is at least 8 kB. After remap, remaining `libart.so` exec VMAs are clean, so that probe is also quiet.)

### 5.3 Writable + executable — High

If a mapping is both writable and executable, Duck scores W+X High. Our dual-map design keeps RX and RW as **two mappings** of the same memfd. Live Duck process: **0** W+X mappings.

### 5.4 Suspicious memfd / deleted `.so` (FD card, not Memory)

`fd_detector.cpp` flags a memfd only if the path contains `memfd:` **and** looks like a loader (`frida`, `zygisk`, `magisk`, `/data/adb/`, …). Empty `/memfd: (deleted)` does **not** match.

A `(deleted)` executable is flagged only if the path is suspicious **or** contains `.so`. `/memfd: (deleted)` contains neither.

That is why unnamed memfd did not light the FD card.

### 5.5 What Duck does not look at (important)

- Duck does **not** walk ART method objects or LSPlant Java trampoline bytes to decide Memory High.
- GOT/PLT checks are for libc/linker symbols (`open`, `mmap`, `dlopen`, …), not for `libart.so` C++ hooks. Init inline hooks therefore show up as **dirty `libart.so` pages**, not as GOT findings.

---

## 6. Architecture: how a hook byte gets into the process

### 6.1 Injection path (unchanged)

```
ReZygisk
  → /data/adb/modules/zygisk_vector/zygisk/arm64-v8a.so  (VectorModule)
    → fetch binder / framework DEX from the daemon
    → lsplant::Init (ART C++ inline hooks via Dobby)
    → Context::InitHooks (DEX trust + JNI bridges)
    → Java forkCommon (Xposed / libxposed modules)
```

LSPlant is configured in `zygisk/src/main/cpp/module.cpp`:

- `inline_hooker` / `inline_unhooker` → Vector `HookInline` → `DobbyHook` / `DobbyDestroy`
- ART symbols from Vector’s `ElfSymbolCache` on `libart.so`
- `generated_class_name = "Vector_"`
- `generated_source_name = ""` (empty on purpose; a `"Dobby"` source name was on Duck’s tag list)
- **`executable_memory_allocator` is not set** any more (see §8). Java ArtMethod trampolines use LSPlant’s own allocator.

### 6.2 Two different executable heaps

This split is the whole Memory story.

| Who | What it allocates | Where it used to live | Where it lives now |
|---|---|---|---|
| **Dobby** | Inline-hook stubs, relocated original instructions | Anonymous `mmap` RX (Finding A), or later a cave inside Vector’s `.so` (`execmod`) | Unnamed memfd, RX map + RW map of the same fd |
| **LSPlant** | Java `ArtMethod` trampolines (after Init) | Same as Dobby if Vector supplied a cave allocator; otherwise LSPlant’s own memfd/ashmem/anon | LSPlant default: unnamed memfd dual-map, then ashmem, then anonymous RWX last |
| **Dobby `DobbyCodePatch` on the target** | The actual 4–16 byte jump written **on top of** `Class::SetStatus` etc. in `libart.so` | `mprotect` RWX → `memcpy` → `mprotect` RX on the **file-backed** page → COW → Finding B | Remap that **one page** to unnamed memfd at the **same address**, then write through the RW alias |

LSPlant Init **must** patch ART `.text`. You cannot “just not hook `libart.so`” and still have a working LSPosed-compatible framework. The remap keeps the hooks and changes only the backing of the dirty page.

### 6.3 Overlay, not a Dobby fork commit

Dobby lives in `external/dobby` (submodule). Vector’s real source of truth is:

`native/patches/dobby/`

`external/CMakeLists.txt` **copies** those four files onto the submodule **before** `add_subdirectory(dobby)`:

- `platform.h` → `dobby/source/PlatformUnifiedInterface/`
- `MemoryAllocator.cc` → `dobby/source/MemoryAllocator/`
- `platform-posix.cc` → `dobby/source/Backend/UserMode/UnifiedInterface/`
- `code-patch-tool-posix.cc` → `dobby/source/Backend/UserMode/ExecMemory/`

That is why CI with a clean Dobby SHA still builds Vector’s behaviour. After a local CMake configure, `git status` shows `external/dobby` dirty (`m`). **Do not commit that gitlink / dirty submodule.** Commit only `native/patches/dobby/*` and `external/CMakeLists.txt`.

### 6.4 The write path (current)

`DobbyHook` (and `DobbyInstrument`) on Android first `mprotect` the target page to RX. That does not dirty a page that is already RX.

The real write is `InterceptRouting::Active` → `DobbyCodePatch(patched_addr, trampoline_bytes)`.

`DobbyCodePatch` (`native/patches/dobby/code-patch-tool-posix.cc`) now:

1. Calls `OSMemory::WriteExecutable`.
2. If that returns true, only flushes the instruction cache and returns.
3. **If that returns false**, it still does the old path: `mprotect` RWX, `memcpy`, `mprotect` RX. That old path **is** Finding B (and is briefly W+X). It remains as a last fallback if remap fails.

`WriteExecutable` (`platform-posix.cc`, Android only):

1. If the address is already in the dual-map table, copy through the RW alias. Done.
2. Else, if the address sits on a file-backed **system** RX page (`/apex/`, `/system/`, …), remap that whole 4 kB page onto unnamed memfd at the same VA, register it, then write through the new RW alias.
3. Else return false (caller may COW).

Dobby **new** trampoline pages go through `OSMemory::Allocate(..., kReadExecute)` → unnamed memfd dual-map first, anonymous RX only if memfd exec is denied.

---

## 7. Finding A and Finding B, with numbers from this phone

Pixel 7a `libart.so` RX load is 1:1 (file offset equals VMA offset). After Init, five 4 kB pages were dirty. File offsets:

- `0x24a000`
- `0x383000`
- `0x3d7000`
- `0x80a000`
- `0x88c000`

Confirmed Init sites on those pages (Android 16, this ART):

- `0x3d7000` — `art::mirror::Class::SetStatus` (`art/mirror/class.cxx`, Oreo+). **Hook** (not resolve-only).
- `0x88c000` — `art::jit::JitCodeCache::DoCollection` (`jit_code_cache.cxx`). **Hook**.
- `0x80a000` — ClassLinker JNI / `RegisterNative` neighbourhood. **Hook**.
- `0x24a000` — `PrettyMethod` lives here but LSPlant uses it **resolve-only** (`.as`, not `.hook`). `GetQuickFrameInfo` is hooked only on API 23, not this device. Something else on that page was still patched (another Init hook in the same 4 kB).
- `0x383000` — no dynsym (stripped). Still a real dirty page from an Init write.

We listed every LSPlant Init hooker and tried to drop “optional” ones. The try-drop inventory was **empty**. Do **not** drop `SetStatus`, `DoCollection`, `RegisterNative`, `FixupStaticTrampolines`, or visibility callbacks. Finding B is not optional hook cost; it is the cost of Init.

LSPlant also **resolves** many symbols without patching them (`PrettyMethod`, `GetClassDef`, …). Resolve-only does not dirty pages.

---

## 8. What we tried that failed (do not repeat)

### 8.1 File-backed trampoline cave inside Vector’s `.so` (commit `5f2ab69b`, later removed)

Idea: put Dobby/LSPlant stubs in unused RX space of `zygisk/arm64-v8a.so` so maps show a file path, not `[anonymous]`.

What happened:

- Dobby’s **own** arena is still `OSMemory::Allocate`. The cave never owned Dobby stubs. Finding A stayed.
- `mprotect` on the Magisk/KSU module `.so` produced SELinux `execmod` AVCs with path `/data/adb/modules/zygisk_vector/zygisk/arm64-v8a.so`. Duck SELinux Warning. The `comm` on those AVCs was the app (`ne.duckdetector`), not `system_server`.

Vector `.so` COW was an allowed trade-off on paper. `execmod` + `/data/adb` in the AVC was not.

### 8.2 Fail-closed dual-map with no anonymous fallback (commit `d8eaf979` as first flashed)

Idea: Dobby exec pages **must** be unnamed memfd. If `memfd`+`PROT_EXEC` fails, return nullptr.

What happened:

- `system_server` (and some other domains) **cannot** map executable memfd.
- Dobby `CHECK_NOT_NULL` logs `FATAL` and **continues**.
- `CodeMemoryArena(0)` → later `memcpy` 0x20 bytes to address 0 → SIGSEGV, `fault addr 0x0`, LR in Vector’s `.so`.
- `system_server` died in a loop. The manager looked like it “would not open”. That was the crash, not Compose.

First dual-map also reserved a 2-page anonymous region and `MAP_FIXED` the RX and RW halves next to each other. That extra `MAP_FIXED` pair failed closed on those domains.

### 8.3 JIT-named memfd

Duck allowlists `/memfd:jit-cache` and `/memfd:jit-zygote-cache`. Using those names for Vector stubs is spoofing. Locked out.

### 8.4 BRENE / SUSFS hide-maps

Official SUSFS: it does **not** hide anonymous memory and does **not** hide inline hooks created by the injected library. BRENE `add_sus_map` marks module `.so` inodes; it does not spoof `Shared_Dirty` / `Anonymous` on `libart.so`. Older SUSFS fake-anon tricks were removed because `PR_SET_VMA` unmasked them, and fake-anon would make Finding A worse.

### 8.5 Dropping Init hooks

See §7. Empty inventory. Would break LSPlant.

### 8.6 Windows `-DVERSION_NAME="2.2"` on the compiler command line

Not a Duck detector issue. It blocked **building** the remap zip on this Windows PC.

Ninja / `CreateProcess` strips the quotes. Clang then parses `2.2` as a **double**, or `'2.2'` as a multi-character constant. Fix: CMake `configure_file` so the quotes live in a header template. Details in §12.

---

## 9. The working design (what is on the phone)

Ranked, in the order the code actually tries:

1. **Unnamed memfd dual-map for new Dobby exec pages**  
   `memfd_create("")` → `ftruncate` → `mmap` RX `MAP_SHARED` and `mmap` RW `MAP_SHARED` as **two independent maps** (not adjacent `MAP_FIXED`). Never W+X. Maps pathname: `/memfd: (deleted)`.

2. **Anonymous RX only if step 1 fails**  
   Keeps `system_server` alive. This **is** Finding A in that process. Duck is not `system_server`, so Duck Memory can stay clear while `system_server` uses the fallback.

3. **Do not return nullptr to Dobby** for `kReadExecute`. `MemoryAllocator::allocateCodeMemoryArena` returns nullptr instead of `CHECK_NOT_NULL` then constructing an arena at 0. Callers that cannot get an arena fail the allocation instead of crashing.

4. **When Dobby must write into existing `/apex` or `/system` RX**  
   Copy that 4 kB page into a new unnamed memfd, `mmap` it RX with `MAP_SHARED | MAP_FIXED` **over the same virtual address**, keep an RW alias, write the hook bytes through the alias, flush I-cache. The CPU still executes at the old PC. PC-relative code in the rest of the page still works because the **whole page** was copied first.

5. **If remap fails**, `DobbyCodePatch` still COWs with `mprotect` RWX. That process would show Finding B again. On this Pixel 7a remap succeeded (maps below).

6. **LSPlant Java trampolines** use LSPlant’s own unnamed memfd dual-map (`AllocateMemoryFromMemfd` in `lsplant.cc`). Vector no longer passes a cave allocator.

---

## 10. Finding A — unnamed memfd trampolines (committed)

### Commits

- `d8eaf979` — `fix: dual-map Dobby exec pages instead of Vector .so cave`
- `6c71a30b` — `fix: never hand Dobby a null exec page`

### What changed

**Added** Vector-owned Dobby overlays (copied at CMake time):

- `native/patches/dobby/platform.h` — adds `OSMemory::WriteExecutable`.
- `native/patches/dobby/platform-posix.cc` — Android memfd dual-map table, `AllocDualExec`, `WriteDualExec`, `Allocate` prefers dual-map for RX.
- `native/patches/dobby/code-patch-tool-posix.cc` — `DobbyCodePatch` writes via `WriteExecutable` first.
- `native/patches/dobby/MemoryAllocator.cc` — null-safe arena allocate (`6c71a30b`).
- `external/CMakeLists.txt` — `file(COPY)` of the four overlays.

**Removed** the cave:

- `native/include/core/trampoline_pool.h`
- `native/src/core/trampoline_pool.cpp`
- `native/src/core/trampoline_pool_android.cpp`
- `zygisk/src/main/cpp/module.cpp` no longer sets `executable_memory_allocator` / `executable_memory_recycler`.

**`6c71a30b` extra:** adjacent `MAP_FIXED` pair replaced by two independent `mmap`s; if dual-map fails, fall back to anonymous RX for `kReadExecute` only (`kReadWriteExecute` still returns nullptr). `.gitignore` gained `/memory-bank/`.

### Why Duck Memory stopped reporting Finding A

Duck’s anonymous check needs an empty / `[anon…]` / `/dev/zero` path. `/memfd: (deleted)` is a real path starting with `/memfd:`. It is not a system path, so Shared_Dirty on it is ignored by Finding B rules.

Live Duck process still has two unnamed memfd **trampoline** pairs (RX + RW, same inode), plus five remapped libart pages (next section).

---

## 11. Finding B — remap dirty `libart.so` pages (uncommitted)

This is the last Memory red after `6c71a30b`. Duck showed **5 Critical / 5 Review**, Hooks Clean, SELinux Warning 0, no anonymous RX. The five `libart.so` r-xp splits were the whole remaining Memory Danger.

### The one behavioural change

File: `native/patches/dobby/platform-posix.cc` (working tree vs `6c71a30b`).

1. Dual-map table size **64 → 96** (`kMaxDuals`), shared by trampoline dual-maps **and** remapped system pages.
2. New helpers (Android only):
   - `PathIsSystemExecFile` — same prefixes Duck uses for `is_system_path`.
   - `PageIsSystemRxFile` — reads `/proc/self/maps`, finds the VMA for that page, requires `r*x`, requires a `/…` path under those prefixes.
   - `RemapSystemPage` — if that page is not already in the dual-map table: `memfd_create("")`, copy 4 kB from the old page, `mmap` RX `MAP_FIXED` at the old address, keep RW alias, `__builtin___clear_cache`, store `{rx, rw, page_size}`. Same-thread mutex is held for the whole remap so another write cannot race.
   - `RemapSystemExecPages` — page-align the write range and remap every covered page (a short Dobby trampoline is still one page).
3. `WriteExecutable` now: `WriteDualExec` **or** (`RemapSystemExecPages` then `WriteDualExec`).

`code-patch-tool-posix.cc` was **not** changed in the uncommitted diff. It already called `WriteExecutable` first. Remap is inside `WriteExecutable`, so every `DobbyCodePatch` of a system RX page goes through remap automatically. We did not special-case `libart.so` by name. Any `/apex` or `/system` RX page Dobby writes will remap. That matches Duck’s rule (system path, not “libart” string).

### Why the same virtual address matters

ART `.text` uses PC-relative addressing. If we copied the function to a **new** address, every literal pool and nearby helper in that page would break unless we relocated the whole page’s instructions. `MAP_FIXED` over the old VA means:

- the process still jumps to `Class::SetStatus` at the old address
- only the **backing** of that 4 kB changes from the APK/APEX file to a memfd
- Linux splits the `libart.so` VMA around the hole

### What Duck then sees

The hole’s pathname is `/memfd: (deleted)`, not `/apex/.../libart.so`. Finding B’s `is_system_path` is false. Shared_Dirty on the memfd (4 kB) is ignored. The leftover `libart.so` RX pieces are still file-backed and **clean** (no COW).

### Edge case (honest)

If the dual-map table is already full (`kMaxDuals == 96`) after a successful `MAP_FIXED`, the code unmaps the RW alias, returns true from `RemapSystemPage`, then `WriteDualExec` fails, then `DobbyCodePatch` falls back to `mprotect` RWX on the already-remapped page. That is a leftover. 96 slots is far above five libart pages plus a couple of trampoline arenas. We did not hit it on device.

If `memfd` exec or `MAP_FIXED` fails, remap returns false and the old COW path runs. Finding B would come back **in that process**.

---

## 12. Windows build: version strings (uncommitted, needed to compile)

This is **not** a Duck bypass. It is how the remap zip was built on this Windows machine.

### What broke

Root `build.gradle.kts` used to pass:

```text
-DVERSION_NAME='"2.2"'
```

in `cppFlags`. On Windows, ninja drops the quotes. Clang then sees `2.2` as a floating-point token.

Zygisk had the same pattern for `INJECTED_PACKAGE_NAME` and `MANAGER_PACKAGE_NAME`. `INJECTED_PACKAGE_UID` is an integer and stayed on `cppFlags` (`-DINJECTED_PACKAGE_UID=…`). `VERSION_CODE` is also an integer and stayed on `cppFlags` (`-DVERSION_CODE=…`).

### What we did

New template:

`native/include/common/vector_build_strings.h.in`

```c
#define VECTOR_VERSION_NAME_LITERAL "@VECTOR_VERSION_NAME@"
#define INJECTED_PACKAGE_NAME "@VECTOR_INJECTED_PACKAGE_NAME@"
#define MANAGER_PACKAGE_NAME "@VECTOR_MANAGER_PACKAGE_NAME@"
```

Quotes are in the template. CMake `configure_file(... @ONLY)` writes `vector_build_strings.h` into the native build `generated/` directory.

`native/CMakeLists.txt`:

- default `VECTOR_VERSION_NAME` to `unknown`
- default `VECTOR_INJECTED_PACKAGE_NAME` to `com.android.shell`
- default `VECTOR_MANAGER_PACKAGE_NAME` to `org.matrix.vector.manager`
- `target_include_directories(native PUBLIC include generated)`

`native/include/common/config.h` includes that header and sets `kVersionName = VECTOR_VERSION_NAME_LITERAL`.

`zygisk/src/main/cpp/module.cpp` already includes `common/config.h`, so it now gets `INJECTED_PACKAGE_NAME` / `MANAGER_PACKAGE_NAME` from the generated header. Zygisk CMake is `add_subdirectory(native)`, so the same generate step runs. Root Gradle passes `-DVECTOR_VERSION_NAME=…`. Zygisk Gradle passes `-DVECTOR_INJECTED_PACKAGE_NAME=…` and `-DVECTOR_MANAGER_PACKAGE_NAME=…`.

Local `java` on PATH is 17. The project wants bytecode 21. The zip that was flashed was built with Android Studio JBR 25.

---

## 13. Logcat / LSPosed card (already committed, still part of the same campaign)

Duck’s LSPosed card was logcat, not Memory.

Committed in `5f2ab69b` (`fix: file-backed trampolines and quiet log defaults`) and kept after the cave was deleted:

- Verbose log default **off** (`PreferenceStore.isVerboseLogEnabled()` → `enable_verbose_log` default `false`).
- `FrameworkService.isLogMuted()` is the inverse of verbose.
- Manager “show traces inline” default **off** (`log_traces_inline`, `false`).
- **Original log strings were kept.** We mute by default; we did not rename `VectorLegacyBridge` messages to dodge Duck’s `"Loading class"` / `"bridge"` / `"xposed"` patterns.
- `generated_source_name = ""` so generated hooker DEX does not carry a `"Dobby"` source file name.

On the remap boot, Duck Hooks / LSPosed were already Clean before Finding B was fixed. Remap did not change logging.

---

## 14. File-by-file list, checked against git

### 14.1 Uncommitted (working tree vs `6c71a30b`) — this is the Finding B + Windows build work

| Path | What git shows | Role |
|---|---|---|
| `native/patches/dobby/platform-posix.cc` | modified, +130 lines | Remap + `kMaxDuals=96` |
| `native/include/common/vector_build_strings.h.in` | **untracked new file** | Quoted string macros |
| `native/CMakeLists.txt` | modified | `configure_file` + public `generated/` include |
| `native/include/common/config.h` | modified | `#include "vector_build_strings.h"`, `kVersionName` |
| `build.gradle.kts` | modified | drop `-DVERSION_NAME` cppFlag; add `-DVECTOR_VERSION_NAME` CMake arg |
| `zygisk/build.gradle.kts` | modified | package-name strings via CMake args; UID stays a cppFlag |
| `external/dobby` | dirty submodule (`m`) | CMake `file(COPY)` overlay. **Do not commit.** |

No other tracked files are modified for this remap. `MemoryAllocator.cc`, `code-patch-tool-posix.cc`, and `platform.h` were **not** edited again in the uncommitted diff.

### 14.2 Already on `duck-bypass` (pushed to `origin/duck-bypass` as `6c71a30b`)

| Commit | Files | Role |
|---|---|---|
| `5f2ab69b` | daemon prefs, manager settings, trampoline_pool (later deleted), zygisk `module.cpp` | Quiet logs; first cave attempt |
| `d8eaf979` | overlays + `external/CMakeLists.txt`; deleted trampoline_pool; LSPlant allocator unset | Dual-map; drop cave |
| `6c71a30b` | `platform-posix.cc`, `MemoryAllocator.cc`, `.gitignore` | Null-safe + independent RX/RW maps |

### 14.3 Not changed (on purpose)

- LSPlant submodule source — Java trampolines already had unnamed memfd.
- Init hook list — all required hookers kept.
- Scanner allowlists — none added.
- Manager UI version label — still git tag/count (`2.2` / `1` on this PC).
- `docs/` and `memory-bank/` — gitignored; not part of the product commit.

---

## 15. Live verification while writing this report

Commands run against the phone that is still on the 3103 remap module.

### 15.1 Module identity

```
id=zygisk_vector
name=Vector
version=vcanary-remap (3103-libart-memfd)
versionCode=3103
```

### 15.2 Duck process

`pidof com.eltavine.duckdetector` → **10593** (same pid as the 04:13 Duck UI scan).

### 15.3 `libart.so` executable VMAs (six file-backed pieces, five 4 kB holes)

```
73b1fde000-73b2028000 r-xp 00200000  …/libart.so
73b2028000-73b2029000 r-xs          /memfd: (deleted)     ← was file offset 0x24a000
73b2029000-73b2161000 r-xp 0024b000  …/libart.so
73b2161000-73b2162000 r-xs          /memfd: (deleted)     ← was 0x383000
73b2162000-73b21b5000 r-xp 00384000  …/libart.so
73b21b5000-73b21b6000 r-xs          /memfd: (deleted)     ← was 0x3d7000
73b21b6000-73b25e8000 r-xp 003d8000  …/libart.so
73b25e8000-73b25e9000 r-xs          /memfd: (deleted)     ← was 0x80a000
73b25e9000-73b266a000 r-xp 0080b000  …/libart.so
73b266a000-73b266b000 r-xs          /memfd: (deleted)     ← was 0x88c000
73b266b000-73b2816000 r-xp 0088d000  …/libart.so
```

Each hole is 4 kB (`…8000` to `…9000`, or `…1000` to `…2000`, etc.). The next `libart.so` piece’s file offset is exactly one page after the hole (`0x24b000`, `0x384000`, `0x3d8000`, `0x80b000`, `0x88d000`). That is the kernel splitting a MAP_PRIVATE file VMA around a `MAP_FIXED` memfd.

### 15.4 Unnamed memfd RX aliases (not JIT)

Five remapped pages (inodes 1528–1532) plus two trampoline RX pages (inodes 1533 and 1527). Matching RW aliases exist at `7675708xxx` for those same inodes. That is the dual-map table in real mappings.

ART’s own `/memfd:jit-cache` and `/memfd:jit-zygote-cache` are still present. Those are Android’s, allowlisted by Duck, not Vector.

### 15.5 smaps (parsed from a full `/proc/10593/smaps` dump)

- Executable mappings under `/apex/`, `/system/`, `/system_ext/`, `/vendor/`, `/product/`, `/odm/` with `Shared_Dirty > 0` or `Anonymous > 0`: **none**.
- Executable mappings with empty / `[anon` / `/dev/zero` path: **none**.
- W+X mappings: **none**.
- The five unnamed remapped pages: `Size=4`, `Shared_Dirty=4`, `Anonymous=0`, `Private_Dirty=0`. Duck ignores that Shared_Dirty because the path is not a system path.

### 15.6 Duck UI (earlier the same boot, same pid)

Scan **04:13:50**, 16 seconds: overview **OK**, Danger 0, Warning 0, Ready 15. Memory card **All Clear**, text “No hook-like memory signals”.

This writing did **not** tap Duck’s Export button again. Maps/smaps on pid 10593 are enough to show the remap is still in that process.

---

## 16. What this does not mean

- The phone is still rooted. Duck can still show **Info** for Custom ROM and similar. That is not Vector Memory.
- We did **not** re-run gpssetter or DeviceMasker hook smoke after remap. Init hooks were kept, so they should still fire, but that was not re-proven on this boot.
- `system_server` may still use anonymous RX if executable memfd is denied there. That is the crash-avoidance fallback. It is Finding A **in system_server**, which Duck does not scan.
- If remap fails in an app process, Finding B returns for that process.
- Local Gradle still stamps `2.2 (1)`. Identifying the flash requires `module.prop` or `libzygisk.so` contents (the remap build contains the `/system_ext/` prefix string from `PathIsSystemExecFile`).
- This work is **not** in JingMatrix/Vector upstream. The fork is `mohdakil2426/Vector`. Do not push this to JingMatrix. The remap commit is **not even on the fork yet** (uncommitted).
- Unsigned canaries: the fork has no `KEY_STORE`.

---

## 17. How to see the same thing yourself

1. Confirm module: `adb shell su -c 'cat /data/adb/modules/zygisk_vector/module.prop'`
2. Open Duck (it must stay in gpssetter scope), let it scan, read the Memory card.
3. `pidof com.eltavine.duckdetector`
4. `grep libart.so /proc/<pid>/maps` — you want r-xp **splits** with 4 kB `/memfd: (deleted)` holes, not five dirty contiguous `libart.so` pages.
5. `grep memfd /proc/<pid>/maps` — unnamed `/memfd: (deleted)` r-xs in those holes, plus trampoline pairs, plus Android JIT names.
6. smaps: no `Shared_Dirty`/`Anonymous` on remaining `/apex/.../libart.so` r-xp rows.

Do not use Duck Compose `snapshot -i` as the oracle (`insufficient foreground app content`). Do not use `--settle` on Duck (hangs ~30 s). Export’s light button sits near **y ≈ 640–664** on 1080×2400; a tap at **540,720** misses it.

---

## 18. Simple picture of the remap

Before (Finding B):

```
[ ........ libart.so r-xp, one big file mapping ........ ]
                      ^
                      write hook bytes here
                      Linux COWs this 4 kB
                      smaps: Shared_Dirty=4, Anonymous=4
                      Duck: High + Review (path is /apex)
```

After:

```
[ libart.so r-xp ][ /memfd: (deleted) r-xs 4k ][ libart.so r-xp ]
                      ^
                      same CPU address as before
                      bytes live in unnamed memfd
                      leftover libart pieces: Shared_Dirty=0
                      Duck: memfd is not a system path → no Finding B
```

Write path never needs W+X: a second mapping of the same memfd is `rw-s` at another address; `memcpy` goes there.

---

## 19. Source of truth checklist (so this report stays honest)

Checked while writing:

- [x] Uncommitted diff is only the six paths in §14.1 plus a dirty Dobby submodule.
- [x] Remap logic exists only in `native/patches/dobby/platform-posix.cc` (and the copied copy under `external/dobby` after CMake).
- [x] `DobbyCodePatch` still has the old `mprotect` RWX fallback if `WriteExecutable` fails.
- [x] Duck `is_system_path` prefixes match `PathIsSystemExecFile`.
- [x] Duck `is_anonymous_path` does not include `/memfd:`.
- [x] Duck FD memfd High requires a suspicious name; empty memfd is not suspicious.
- [x] Live `module.prop` is 3103 remap.
- [x] Live Duck maps show five libart holes filled with unnamed memfd.
- [x] Live smaps: zero system-path exec dirty/anonymous; zero anonymous RX; zero W+X.
- [x] Windows string header is a build fix, not a detector bypass.
- [x] Cave / trampoline_pool is gone from `module.cpp`.
- [x] `generated_source_name` is `""`.
- [x] Verbose log default is false.

If you rebuild without committing, you must still overlay `platform-posix.cc` through CMake (automatic) and flash a zip whose `versionCode` is **higher than** the installed 3103, or uninstall first. A `versionCode=1` zip will **not** replace 3103.
