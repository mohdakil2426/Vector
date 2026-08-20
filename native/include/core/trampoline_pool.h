#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vector::native {

struct MemoryProtect {
    enum class Prot { kReadWrite, kReadExec };
    virtual ~MemoryProtect() = default;
    virtual bool Change(void *addr, size_t len, Prot prot) = 0;
    virtual void SyncICache(void *start, void *end) = 0;
};

class TrampolinePool {
public:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kPoolPages = 2048;
    static constexpr size_t kPoolBytes = kPoolPages * kPageSize;

    TrampolinePool(std::span<std::byte> pool, MemoryProtect &protect);

    // Copies code onto a fresh page and seals it r-x. Returns nullptr on failure.
    // Never maps memory; never requests W+X.
    void *Allocate(std::string_view code);
    void Recycle(void *memory);

    int pages_used() const { return pages_used_; }

private:
    std::span<std::byte> pool_;
    MemoryProtect &protect_;
    int pages_used_ = 0;
};

void *AllocateExecutableTrampoline(std::span<const uint8_t> data);
void RecycleExecutableTrampoline(void *memory);

}  // namespace vector::native
