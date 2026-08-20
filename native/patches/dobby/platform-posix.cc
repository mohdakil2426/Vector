#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <assert.h>
#include <cstdint>

#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h> // for pthread_set_name_np
#endif

#include <sched.h> // for sched_yield
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>

#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h> // NOLINT, for sysctl
#endif

#include "logging/logging.h"
#include "logging/check_logging.h"
#include "PlatformUnifiedInterface/platform.h"

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif

#if defined(ANDROID) && !defined(ANDROID_LOG_STDOUT)
#define ANDROID_LOG_TAG "Dobby"

#include <android/log.h>

#endif

#include <string.h>

#if defined(__APPLE__)
const int kMmapFd = VM_MAKE_TAG(255);
#else
const int kMmapFd = -1;
#endif

const int kMmapFdOffset = 0;

using namespace base;

typedef struct thread_handle_t {
  pthread_t thread;
} thread_handle_t;

void ThreadInterface::SetName(const char *name) {
#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  pthread_set_name_np(pthread_self(), name);
#elif defined(__APPLE__)
  pthread_setname_np(name);
#endif
}

int ThreadInterface::CurrentId() {
#if defined(__APPLE__)
  mach_port_t port = mach_thread_self();
  mach_port_deallocate(mach_task_self(), port);
  return port;
#elif defined(_POSIX_VERSION)
  return syscall(__NR_gettid);
#endif
}

static void *thread_handler_wrapper(void *ctx) {
  ThreadInterface::Delegate *d = (ThreadInterface::Delegate *)ctx;
  d->ThreadMain();
  return nullptr;
}

bool ThreadInterface::Create(ThreadInterface::Delegate *delegate, ThreadHandle *handle) {
  thread_handle_t *handle_impl = new thread_handle_t;

  int err = 0;
  err = pthread_create(&(handle_impl->thread), nullptr, thread_handler_wrapper, delegate);
  if (err != 0) {
    ERROR_LOG("pthread create failed");
    return false;
  }
  return true;
}

OSThread::OSThread(const char *name) {
  strncpy(name_, name, sizeof(name_) -1);
}

bool OSThread::Start() {
  if (ThreadInterface::Create(this, &handle_) == false) {
    return false;
  }
  return true;
}

static int GetProtectionFromMemoryPermission(MemoryPermission access) {
  switch (access) {
  case MemoryPermission::kNoAccess:
    return PROT_NONE;
  case MemoryPermission::kRead:
    return PROT_READ;
  case MemoryPermission::kReadWrite:
    return PROT_READ | PROT_WRITE;
  case MemoryPermission::kReadWriteExecute:
    return PROT_READ | PROT_WRITE | PROT_EXEC;
  case MemoryPermission::kReadExecute:
    return PROT_READ | PROT_EXEC;
  }
  UNREACHABLE();
}

int OSMemory::PageSize() {
  return static_cast<int>(sysconf(_SC_PAGESIZE));
}

void *OSMemory::Allocate(size_t size, MemoryPermission access) {
  return OSMemory::Allocate(size, access, nullptr);
}

#if defined(__ANDROID__)
#include <fcntl.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <string.h>
#include <sys/syscall.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace {
struct DualExecRegion {
  uint8_t *rx;
  uint8_t *rw;
  size_t size;
};

pthread_mutex_t g_dual_lock = PTHREAD_MUTEX_INITIALIZER;
constexpr int kMaxDuals = 96;
DualExecRegion g_duals[kMaxDuals];
int g_dual_count = 0;

bool DualContainsLocked(uint8_t *ptr, uint32_t size) {
  for (int i = 0; i < g_dual_count; i++) {
    if (ptr >= g_duals[i].rx && ptr + size <= g_duals[i].rx + g_duals[i].size)
      return true;
  }
  return false;
}

bool PathIsSystemExecFile(const char *path) {
  if (!path || path[0] != '/')
    return false;
  static const char *kPrefixes[] = {"/apex/",     "/system/", "/system_ext/",
                                    "/vendor/",   "/product/", "/odm/"};
  for (const char *prefix : kPrefixes) {
    if (strncmp(path, prefix, strlen(prefix)) == 0)
      return true;
  }
  return false;
}

// Duck High/Review for Finding B is file-backed /apex|/system RX with
// Shared_Dirty or Anonymous > 0. Unnamed memfd is not anonymous and is not a
// system path. Replace the dirty page in place (same VA) so PC-relative code
// and Init hooks keep working.
bool PageIsSystemRxFile(uintptr_t page) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (!fp)
    return false;
  char line[768];
  bool hit = false;
  while (fgets(line, sizeof(line), fp)) {
    unsigned long start = 0, end = 0;
    char perms[8] = {};
    if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
      continue;
    if (page < start || page >= end)
      continue;
    if (perms[0] != 'r' || perms[2] != 'x')
      break;
    char *path = strchr(line, '/');
    if (!path)
      break;
    size_t len = strlen(path);
    while (len && (path[len - 1] == '\n' || path[len - 1] == '\r'))
      path[--len] = 0;
    hit = PathIsSystemExecFile(path);
    break;
  }
  fclose(fp);
  return hit;
}

bool RemapSystemPage(uintptr_t page) {
  const size_t ps = static_cast<size_t>(OSMemory::PageSize());
  auto *page_ptr = reinterpret_cast<uint8_t *>(page);

  if (!PageIsSystemRxFile(page))
    return false;

  pthread_mutex_lock(&g_dual_lock);
  if (DualContainsLocked(page_ptr, 1)) {
    pthread_mutex_unlock(&g_dual_lock);
    return true;
  }

  int fd = static_cast<int>(syscall(__NR_memfd_create, "", MFD_CLOEXEC));
  if (fd < 0) {
    pthread_mutex_unlock(&g_dual_lock);
    return false;
  }
  if (ftruncate(fd, static_cast<off_t>(ps)) < 0) {
    close(fd);
    pthread_mutex_unlock(&g_dual_lock);
    return false;
  }
  void *rw = mmap(nullptr, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (rw == MAP_FAILED) {
    close(fd);
    pthread_mutex_unlock(&g_dual_lock);
    return false;
  }
  memcpy(rw, page_ptr, ps);
  void *rx = mmap(page_ptr, ps, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, fd, 0);
  close(fd);
  if (rx != page_ptr) {
    munmap(rw, ps);
    pthread_mutex_unlock(&g_dual_lock);
    return false;
  }
#if defined(__GNUC__)
  __builtin___clear_cache(reinterpret_cast<char *>(page_ptr),
                          reinterpret_cast<char *>(page_ptr) + ps);
#endif

  if (g_dual_count >= kMaxDuals) {
    pthread_mutex_unlock(&g_dual_lock);
    munmap(rw, ps);
    return true;
  }
  g_duals[g_dual_count++] = {static_cast<uint8_t *>(rx), static_cast<uint8_t *>(rw), ps};
  pthread_mutex_unlock(&g_dual_lock);
  return true;
}

bool RemapSystemExecPages(void *address, uint32_t size) {
  if (!address || size == 0)
    return false;
  const uintptr_t ps = static_cast<uintptr_t>(OSMemory::PageSize());
  uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(ps - 1);
  uintptr_t end = (reinterpret_cast<uintptr_t>(address) + size + ps - 1) & ~(ps - 1);
  for (uintptr_t page = start; page < end; page += ps) {
    if (!RemapSystemPage(page))
      return false;
  }
  return true;
}

void *AllocDualExec(size_t size) {
  const size_t page = static_cast<size_t>(OSMemory::PageSize());
  size = (size + page - 1) / page * page;

  int fd = static_cast<int>(syscall(__NR_memfd_create, "", MFD_CLOEXEC));
  if (fd < 0)
    return nullptr;
  if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
    close(fd);
    return nullptr;
  }

  // Independent RX/RW aliases of the same memfd. Adjacent MAP_FIXED pairs are
  // not required, and they fail closed on domains that reject execmem.
  void *rx = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  void *rw = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (rx == MAP_FAILED || rw == MAP_FAILED) {
    if (rx != MAP_FAILED)
      munmap(rx, size);
    if (rw != MAP_FAILED)
      munmap(rw, size);
    return nullptr;
  }

  pthread_mutex_lock(&g_dual_lock);
  if (g_dual_count >= kMaxDuals) {
    pthread_mutex_unlock(&g_dual_lock);
    munmap(rx, size);
    munmap(rw, size);
    return nullptr;
  }
  g_duals[g_dual_count++] = {static_cast<uint8_t *>(rx), static_cast<uint8_t *>(rw), size};
  pthread_mutex_unlock(&g_dual_lock);
  return rx;
}

bool WriteDualExec(void *address, uint8_t *buffer, uint32_t size) {
  auto *ptr = static_cast<uint8_t *>(address);
  pthread_mutex_lock(&g_dual_lock);
  for (int i = 0; i < g_dual_count; i++) {
    if (ptr >= g_duals[i].rx && ptr + size <= g_duals[i].rx + g_duals[i].size) {
      memcpy(g_duals[i].rw + (ptr - g_duals[i].rx), buffer, size);
      pthread_mutex_unlock(&g_dual_lock);
      return true;
    }
  }
  pthread_mutex_unlock(&g_dual_lock);
  return false;
}
} // namespace
#endif

void *OSMemory::Allocate(size_t size, MemoryPermission access, void *fixed_address) {
  int prot = GetProtectionFromMemoryPermission(access);

#if defined(__ANDROID__)
  if (fixed_address == nullptr && (access == kReadExecute || access == kReadWriteExecute)) {
    if (void *dual = AllocDualExec(size))
      return dual;
    // system_server and some domains cannot map executable memfd. Dobby CHECK
    // does not abort, so a nullptr here becomes memcpy(NULL) and kills the
    // process. Fall back to anonymous RX only after dual-map fails.
    if (access == kReadWriteExecute)
      return nullptr;
  }
#endif

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (fixed_address != nullptr) {
    flags = flags | MAP_FIXED;
  }
  void *result = mmap(fixed_address, size, prot, flags, kMmapFd, kMmapFdOffset);
  if (result == MAP_FAILED)
    return nullptr;

  return result;
}

bool OSMemory::WriteExecutable(void *address, uint8_t *buffer, uint32_t size) {
#if defined(__ANDROID__)
  if (WriteDualExec(address, buffer, size))
    return true;
  // Init hooks land in libart .text (file-backed MAP_PRIVATE). mprotect+write
  // COWs those pages and Duck scores Shared_Dirty / Anonymous on /apex.
  if (RemapSystemExecPages(address, size) && WriteDualExec(address, buffer, size))
    return true;
  return false;
#else
  (void)address;
  (void)buffer;
  (void)size;
  return false;
#endif
}

bool OSMemory::Free(void *address, size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % PageSize());
  DCHECK_EQ(0, size % PageSize());

  return munmap(address, size) == 0;
}

bool OSMemory::Release(void *address, size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % PageSize());
  DCHECK_EQ(0, size % PageSize());

  return munmap(address, size) == 0;
}

bool OSMemory::SetPermission(void *address, size_t size, MemoryPermission access) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % PageSize());
  DCHECK_EQ(0, size % PageSize());

  int prot = GetProtectionFromMemoryPermission(access);
  int ret = mprotect(address, size, prot);
  if (ret) {
    ERROR_LOG("OSMemory::SetPermission: %s\n", ((const char *)strerror(errno)));
  }

  return ret == 0;
}

void OSPrint::Print(const char *format, ...) {
  va_list args;
  va_start(args, format);
  VPrint(format, args);
  va_end(args);
}

void OSPrint::VPrint(const char *format, va_list args) {
#if defined(ANDROID) && !defined(ANDROID_LOG_STDOUT)
  __android_log_vprint(ANDROID_LOG_INFO, ANDROID_LOG_TAG, format, args);
#else
  vprintf(format, args);
#endif
}
