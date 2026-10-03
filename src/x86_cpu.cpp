#include "lime/x86_cpu.hpp"

namespace lime {

X86CPUDecoder::X86CPUDecoder(std::shared_ptr<MemoryManager> mem, std::shared_ptr<DeviceBus> bus)
    : mem_(mem), bus_(bus) {
    gprs_.fill(0);
}

void X86CPUDecoder::reset(uint64_t entry_point) {
    gprs_.fill(0);
    rip_ = entry_point;
    rflags_ = 0x02;
    cr0_ = 0x60000010;
    cr3_ = 0;
    cr4_ = 0;
    mode_ = CPUMode::REAL_16BIT;
}

uint64_t X86CPUDecoder::get_gpr(size_t index) const {
    if (index < 16) return gprs_[index];
    return 0;
}

void X86CPUDecoder::set_gpr(size_t index, uint64_t val) {
    if (index < 16) {
        gprs_[index] = val;
    }
}

uint64_t X86CPUDecoder::get_rip() const {
    return rip_;
}

void X86CPUDecoder::set_rip(uint64_t rip) {
    rip_ = rip;
}

CPUMode X86CPUDecoder::mode() const {
    return mode_;
}

void X86CPUDecoder::set_mode(CPUMode m) {
    mode_ = m;
}

bool X86CPUDecoder::step() {
    if (!mem_) return false;
    uint8_t opcode = mem_->read8(rip_);
    rip_++;

    switch (opcode) {
    case 0x90:
        break;
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB: {
        uint32_t val = mem_->read32(rip_);
        rip_ += 4;
        set_gpr(opcode - 0xB8, val);
        break;
    }
    case 0xEB: {
        int8_t rel = static_cast<int8_t>(mem_->read8(rip_));
        rip_++;
        rip_ += rel;
        break;
    }
    case 0xE9: {
        int32_t rel = static_cast<int32_t>(mem_->read32(rip_));
        rip_ += 4;
        rip_ += rel;
        break;
    }
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53: {
        uint64_t val = get_gpr(opcode - 0x50);
        uint64_t rsp = get_gpr(4) - 8;
        set_gpr(4, rsp);
        mem_->write64(rsp, val);
        break;
    }
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B: {
        uint64_t rsp = get_gpr(4);
        uint64_t val = mem_->read64(rsp);
        set_gpr(opcode - 0x58, val);
        set_gpr(4, rsp + 8);
        break;
    }
    case 0x01: {
        uint8_t modrm = mem_->read8(rip_);
        rip_++;
        uint8_t reg = (modrm >> 3) & 7;
        uint8_t rm = modrm & 7;
        set_gpr(rm, get_gpr(rm) + get_gpr(reg));
        break;
    }
    case 0x29: {
        uint8_t modrm = mem_->read8(rip_);
        rip_++;
        uint8_t reg = (modrm >> 3) & 7;
        uint8_t rm = modrm & 7;
        set_gpr(rm, get_gpr(rm) - get_gpr(reg));
        break;
    }
    case 0xF4:
        return false;
    default:
        break;
    }
    return true;
}

}
