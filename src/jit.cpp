#include "lime/jit.hpp"

namespace lime {

void JITEngine::clear_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_cache_.clear();
}

const BasicBlock* JITEngine::lookup_or_compile(uint64_t pc, const std::vector<uint32_t>& instructions) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = block_cache_.find(pc);
    if (it != block_cache_.end()) {
        it->second.execute_count++;
        return &it->second;
    }

    BasicBlock bb;
    bb.start_pc = pc;

    for (uint32_t inst : instructions) {
        MicroOp op;
        op.raw_inst = inst;
        op.opcode = inst & 0x7F;
        op.rd = (inst >> 7) & 0x1F;
        op.rs1 = (inst >> 15) & 0x1F;
        op.rs2 = (inst >> 20) & 0x1F;
        op.imm = static_cast<int32_t>(inst) >> 20;
        bb.ops.push_back(op);

        if (op.opcode == 0x6F || op.opcode == 0x67 || op.opcode == 0x63 || op.opcode == 0x73) {
            break;
        }
    }

    auto insert_res = block_cache_.emplace(pc, std::move(bb));
    insert_res.first->second.execute_count = 1;
    return &insert_res.first->second;
}

void JITEngine::invalidate_page(uint64_t gpa) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t page_base = gpa & ~0xFFFULL;
    for (auto it = block_cache_.begin(); it != block_cache_.end();) {
        if ((it->first & ~0xFFFULL) == page_base) {
            it = block_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t JITEngine::compiled_blocks_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return block_cache_.size();
}

}
