#include "core/trampoline_pool.h"

#include <cstring>

namespace vector::native {

TrampolinePool::TrampolinePool(std::span<std::byte> pool, MemoryProtect &protect)
    : pool_(pool), protect_(protect) {}

void *TrampolinePool::Allocate(std::string_view code) {
    if (code.size() > kPageSize) return nullptr;
    if (static_cast<size_t>(pages_used_) >= pool_.size() / kPageSize) return nullptr;
    auto *page = pool_.data() + static_cast<size_t>(pages_used_) * kPageSize;
    if (!protect_.Change(page, kPageSize, MemoryProtect::Prot::kReadWrite)) return nullptr;
    std::memcpy(page, code.data(), code.size());
    protect_.SyncICache(page, page + code.size());
    if (!protect_.Change(page, kPageSize, MemoryProtect::Prot::kReadExec)) return nullptr;
    ++pages_used_;
    return page;
}

void TrampolinePool::Recycle(void *) {}

}  // namespace vector::native
