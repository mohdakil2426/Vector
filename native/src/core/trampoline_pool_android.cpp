#include "core/trampoline_pool.h"

#include <sys/mman.h>

#include <mutex>

namespace vector::native {
namespace {

class AndroidProtect final : public MemoryProtect {
public:
    bool Change(void *addr, size_t len, Prot prot) override {
        const int bits = (prot == Prot::kReadWrite) ? (PROT_READ | PROT_WRITE)
                                                    : (PROT_READ | PROT_EXEC);
        return mprotect(addr, len, bits) == 0;
    }
    void SyncICache(void *start, void *end) override {
        __builtin___clear_cache(static_cast<char *>(start), static_cast<char *>(end));
    }
};

alignas(4096) __attribute__((section(".text.vector_stub_pool"), used, visibility("hidden")))
std::byte g_vector_stub_pool[TrampolinePool::kPoolBytes]{};

AndroidProtect g_protect;
std::once_flag g_once;
TrampolinePool *g_pool = nullptr;

void InitUnlocked() {
    static TrampolinePool pool({g_vector_stub_pool, TrampolinePool::kPoolBytes}, g_protect);
    g_pool = &pool;
}

}  // namespace

void *AllocateExecutableTrampoline(std::span<const uint8_t> data) {
    std::call_once(g_once, InitUnlocked);
    if (g_pool == nullptr) return nullptr;
    return g_pool->Allocate(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
}

void RecycleExecutableTrampoline(void *memory) {
    if (g_pool == nullptr) return;
    g_pool->Recycle(memory);
}

}  // namespace vector::native
