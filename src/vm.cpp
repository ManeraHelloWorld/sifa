#include "sifa/vm.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace sifa {
namespace {
constexpr std::size_t code_limit = 65536;
constexpr std::size_t output_limit = 65536;
constexpr std::uint64_t step_limit = 10000000;

std::string hex_byte(std::uint8_t byte) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<unsigned>(byte);
    return text.str();
}
} // namespace

std::vector<std::uint8_t> demo_guest_bytes() {
    // MOV AL, character; OUT 0xe9, AL; ...; HLT
    return {0xb0, 'S', 0xe6, 0xe9, 0xb0, 'I', 0xe6, 0xe9,
            0xb0, 'F', 0xe6, 0xe9, 0xb0, 'A', 0xe6, 0xe9,
            0xb0, '\n', 0xe6, 0xe9, 0xf4};
}

VmResult run_software_guest(std::span<const std::uint8_t> program,
                            std::uint64_t max_steps) {
    const auto started = std::chrono::steady_clock::now();
    VmResult result;
    result.backend = "software-x86";
    const auto finish = [&](std::string error = {}) {
        result.error = std::move(error);
        result.ok = result.error.empty();
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    };
    if (program.empty()) return finish("Guest program is empty.");
    if (program.size() > code_limit) return finish("Guest exceeds the 64 KiB code address space.");
    if (max_steps == 0 || max_steps > step_limit)
        return finish("Instruction budget must be between 1 and 10000000.");

    std::size_t ip = 0;
    std::uint16_t ax = 0;
    std::uint16_t dx = 0;
    const auto has_bytes = [&](std::size_t count) {
        return ip <= program.size() && count <= program.size() - ip;
    };
    const auto emit = [&](std::uint16_t port) -> std::string {
        if (port != 0xe9) return "Unsupported I/O port; only debug port 0xe9 is implemented.";
        if (result.output.size() >= output_limit) return "Guest debug output limit exceeded.";
        result.output.push_back(static_cast<char>(ax & 0xff));
        return {};
    };

    while (result.instructions < max_steps) {
        if (!has_bytes(1)) return finish("Instruction pointer left the loaded guest program.");
        const auto opcode = program[ip++];
        ++result.instructions;
        switch (opcode) {
        case 0x90: // NOP
            break;
        case 0xb0: // MOV AL, imm8
            if (!has_bytes(1)) return finish("Truncated MOV AL immediate.");
            ax = static_cast<std::uint16_t>((ax & 0xff00) | program[ip++]);
            break;
        case 0xb8: // MOV AX, imm16
        case 0xba: { // MOV DX, imm16
            if (!has_bytes(2)) return finish("Truncated MOV 16-bit immediate.");
            const auto value = static_cast<std::uint16_t>(program[ip] |
                static_cast<std::uint16_t>(program[ip + 1]) << 8);
            ip += 2;
            if (opcode == 0xb8) ax = value;
            else dx = value;
            break;
        }
        case 0xe6: { // OUT imm8, AL
            if (!has_bytes(1)) return finish("Truncated OUT immediate.");
            const auto error = emit(program[ip++]);
            if (!error.empty()) return finish(error);
            break;
        }
        case 0xee: { // OUT DX, AL
            const auto error = emit(dx);
            if (!error.empty()) return finish(error);
            break;
        }
        case 0xeb: { // JMP rel8
            if (!has_bytes(1)) return finish("Truncated JMP displacement.");
            const auto byte = program[ip++];
            const auto displacement = byte < 128 ? static_cast<int>(byte) : static_cast<int>(byte) - 256;
            const auto target = static_cast<std::int64_t>(ip) + displacement;
            if (target < 0 || target >= static_cast<std::int64_t>(program.size()))
                return finish("Jump target is outside the loaded guest program.");
            ip = static_cast<std::size_t>(target);
            break;
        }
        case 0xf4: // HLT
            return finish();
        default:
            return finish("Unsupported x86 opcode " + hex_byte(opcode) + ".");
        }
    }
    return finish("Guest instruction budget exhausted before HLT.");
}

VmResult run_guest_demo(const std::string& backend) {
    if (backend == "software") return run_software_guest(demo_guest_bytes());
    if (backend == "hardware") return run_hardware_guest_demo();
    if (backend == "auto") {
        if (probe_hypervisor().available) return run_hardware_guest_demo();
        return run_software_guest(demo_guest_bytes());
    }
    VmResult result;
    result.backend = backend;
    result.error = "Unknown VM backend. Use auto, software, or hardware.";
    return result;
}

} // namespace sifa
