#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sifa {

struct VmResult {
    bool ok = false;
    std::string backend;
    std::string output;
    std::string error;
    // Software: attempted instructions. Hardware: known instructions in this fixed guest.
    std::uint64_t instructions = 0;
    double elapsed_ms = 0;
};

struct HypervisorInfo {
    std::string backend;
    bool available = false;
    std::string detail;
};

// Checks usable hardware virtualization, not just CPU feature flags.
HypervisorInfo probe_hypervisor();

// This deliberately small x86-16 educational machine is not an Android runtime.
// Supported opcodes: MOV AL/AX/DX immediate, NOP, OUT imm8/DX, JMP rel8, HLT.
// It has a 64 KiB code address space, bounded execution and bounded debug output.
VmResult run_software_guest(std::span<const std::uint8_t> program,
                            std::uint64_t max_steps = 100000);

// Trusted straight-line machine code emits "SIFA\n" on port 0xe9 and halts.
std::vector<std::uint8_t> demo_guest_bytes();

// Executes only the built-in trusted demo, never arbitrary code or a disk image.
VmResult run_hardware_guest_demo();

// backend: "auto", "software", or "hardware". Auto selects hardware only
// when the platform probe succeeds. An attempted hardware failure is reported.
VmResult run_guest_demo(const std::string& backend = "auto");

} // namespace sifa
