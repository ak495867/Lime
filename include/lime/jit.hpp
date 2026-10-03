#ifndef LIME_JIT_HPP
#define LIME_JIT_HPP

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace lime {

struct MicroOp {
    uint32_t raw_inst;
    uint32_t opcode;
    uint32_t rd;
    uint32_t rs1;
    uint32_t rs2;
    int32_t imm;
};

struct BasicBlock {
    uint64_t start_pc;
    std::vector<MicroOp> ops;
    size_t execute_count{0};
};

class JITEngine {
public:
    JITEngine() = default;
    ~JITEngine() = default;

    void clear_cache();
    const BasicBlock* lookup_or_compile(uint64_t pc, const std::vector<uint32_t>& instructions);
    void invalidate_page(uint64_t gpa);

    size_t compiled_blocks_count() const;

private:
    std::unordered_map<uint64_t, BasicBlock> block_cache_;
    mutable std::mutex mutex_;
};

}

#endif
