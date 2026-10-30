#include "sifa/vm.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(EXIT_FAILURE);
    }
}
sifa::VmResult run(std::initializer_list<std::uint8_t> bytes, std::uint64_t budget = 100000) {
    return sifa::run_software_guest(std::span<const std::uint8_t>(bytes.begin(), bytes.size()), budget);
}
}

int main() {
    const auto demo = sifa::run_guest_demo("software");
    check(demo.ok, "software guest succeeds");
    check(demo.output == "SIFA\n", "guest I/O matches expected output");
    check(demo.instructions == 11, "guest executes exactly eleven instructions");
    check(demo.backend == "software-x86", "backend is accurately identified");
    check(demo.elapsed_ms >= 0, "execution duration is nonnegative");
    check(!sifa::run_guest_demo("not-a-backend").ok, "unknown backend rejected");
    check(!run({}).ok, "empty guest rejected");
    check(!run({0x0f}).ok, "unsupported instruction rejected");
    check(!run({0xb0}).ok, "truncated MOV AL rejected");
    check(!run({0xb8, 0x12}).ok, "truncated MOV AX rejected");
    check(!run({0xba}).ok, "truncated MOV DX rejected");
    check(!run({0xe6}).ok, "truncated OUT rejected");
    check(!run({0xeb}).ok, "truncated JMP rejected");
    check(!run({0x90}).ok, "fetch past loaded memory rejected");
    check(!run({0xeb, 0xfd}).ok, "negative jump outside program rejected");
    check(!run({0xeb, 0x7f}).ok, "positive jump outside program rejected");
    check(!run({0xe6, 0x80, 0xf4}).ok, "unknown immediate I/O port rejected");
    check(!run({0xee, 0xf4}).ok, "unknown register I/O port rejected");
    check(!run({0xf4}, 0).ok, "zero instruction budget rejected");
    check(!run({0xf4}, 10000001).ok, "excessive instruction budget rejected");
    const auto loop = run({0xeb, 0xfe}, 100);
    check(!loop.ok && loop.instructions == 100, "infinite loop stops exactly at instruction budget");
    check(run({0x90, 0xf4}, 2).ok, "HLT at last budgeted instruction succeeds");
    check(!run({0x90, 0xf4}, 1).ok, "insufficient execution budget fails");
    const auto port = run({0xb8, 'Z', 0x42, 0xba, 0xe9, 0x00, 0xee, 0xf4});
    check(port.ok && port.output == "Z", "16-bit registers and OUT DX execute correctly");
    const auto low_byte = run({0xb8, 0x00, 0x42, 0xb0, 'A', 0xe6, 0xe9, 0xf4});
    check(low_byte.ok && low_byte.output == "A", "MOV AL replaces only low byte");
    const auto jump = run({0xeb, 0x01, 0x0f, 0xf4});
    check(jump.ok && jump.instructions == 2, "relative jump skips invalid instruction");
    const auto output_loop = run({0xb0, 'A', 0xe6, 0xe9, 0xeb, 0xfc}, 200000);
    check(!output_loop.ok && output_loop.output.size() == 65536, "debug output is bounded");
    std::vector<std::uint8_t> oversized(65537, 0xf4);
    check(!sifa::run_software_guest(oversized).ok, "oversized code rejected");
    std::vector<std::uint8_t> maximum(65536, 0x90);
    maximum.back() = 0xf4;
    const auto full = sifa::run_software_guest(maximum);
    check(full.ok && full.instructions == 65536, "full 64 KiB guest executes safely");
    const auto hardware = sifa::probe_hypervisor();
    check(!hardware.backend.empty() && !hardware.detail.empty(), "hardware probe provides an actionable result");
    if (hardware.available) {
        const auto accelerated = sifa::run_guest_demo("hardware");
        check(accelerated.ok && accelerated.output == "SIFA\n", "available hardware executes the guest");
        std::cout << "Hardware VM validated: " << accelerated.backend << '\n';
    } else {
        std::cout << "Hardware VM SKIPPED: " << hardware.detail << '\n';
    }
    std::cout << checks << " VM checks passed.\n";
}
