#include "sifa/vm.hpp"

#ifndef _WIN32
#include <cerrno>
#include <chrono>
#include <cstring>

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace sifa {
namespace {
class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};
class Mapping {
public:
    Mapping(void* address, std::size_t size) : address_(address), size_(size) {}
    ~Mapping() { if (address_ != MAP_FAILED) ::munmap(address_, size_); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    void* get() const { return address_; }
private:
    void* address_;
    std::size_t size_;
};
std::string system_error(const std::string& operation) {
    const int code = errno;
    return operation + ": " + std::strerror(code);
}
constexpr std::size_t guest_memory_size = 2 * 1024 * 1024;
constexpr std::uint64_t guest_entry = 0x1000;
} // namespace

HypervisorInfo probe_hypervisor() {
    HypervisorInfo info{"kvm", false, {}};
    const FileDescriptor kvm(::open("/dev/kvm", O_RDWR | O_CLOEXEC));
    if (kvm.get() < 0) {
        info.detail = system_error("Cannot open /dev/kvm") +
            ". Enable virtualization and grant this user access to KVM.";
        return info;
    }
    const int api = ::ioctl(kvm.get(), KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) {
        info.detail = api < 0 ? system_error("KVM_GET_API_VERSION") : "Unsupported KVM API version.";
        return info;
    }
    const FileDescriptor vm(::ioctl(kvm.get(), KVM_CREATE_VM, 0));
    if (vm.get() < 0) {
        info.detail = system_error("KVM_CREATE_VM");
        return info;
    }
    info.available = true;
    info.detail = "KVM API 12 is accessible and a test VM was created successfully.";
    return info;
}

VmResult run_hardware_guest_demo() {
    const auto started = std::chrono::steady_clock::now();
    VmResult result;
    result.backend = "kvm";
    const auto finish = [&](std::string error = {}) {
        result.error = std::move(error);
        result.ok = result.error.empty();
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    };

    const FileDescriptor kvm(::open("/dev/kvm", O_RDWR | O_CLOEXEC));
    if (kvm.get() < 0) return finish(system_error("Cannot open /dev/kvm"));
    const int api = ::ioctl(kvm.get(), KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) return finish("KVM API 12 is required.");
    const FileDescriptor vm(::ioctl(kvm.get(), KVM_CREATE_VM, 0));
    if (vm.get() < 0) return finish(system_error("KVM_CREATE_VM"));

    const Mapping memory(::mmap(nullptr, guest_memory_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), guest_memory_size);
    if (memory.get() == MAP_FAILED) return finish(system_error("Guest RAM mmap"));
    const auto guest = demo_guest_bytes();
    std::memcpy(static_cast<std::uint8_t*>(memory.get()) + guest_entry, guest.data(), guest.size());
    kvm_userspace_memory_region region{};
    region.slot = 0;
    region.guest_phys_addr = 0;
    region.memory_size = guest_memory_size;
    region.userspace_addr = reinterpret_cast<std::uintptr_t>(memory.get());
    if (::ioctl(vm.get(), KVM_SET_USER_MEMORY_REGION, &region) < 0)
        return finish(system_error("KVM_SET_USER_MEMORY_REGION"));
    const FileDescriptor vcpu(::ioctl(vm.get(), KVM_CREATE_VCPU, 0));
    if (vcpu.get() < 0) return finish(system_error("KVM_CREATE_VCPU"));
    const int run_size = ::ioctl(kvm.get(), KVM_GET_VCPU_MMAP_SIZE, 0);
    if (run_size < static_cast<int>(sizeof(kvm_run))) return finish("Invalid KVM vCPU mapping size.");
    const Mapping run_mapping(::mmap(nullptr, static_cast<std::size_t>(run_size),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, vcpu.get(), 0),
                              static_cast<std::size_t>(run_size));
    if (run_mapping.get() == MAP_FAILED) return finish(system_error("KVM vCPU mmap"));
    auto* run = static_cast<kvm_run*>(run_mapping.get());

    kvm_sregs special{};
    if (::ioctl(vcpu.get(), KVM_GET_SREGS, &special) < 0) return finish(system_error("KVM_GET_SREGS"));
    special.cs.base = 0;
    special.cs.selector = 0;
    if (::ioctl(vcpu.get(), KVM_SET_SREGS, &special) < 0) return finish(system_error("KVM_SET_SREGS"));
    kvm_regs registers{};
    registers.rip = guest_entry;
    registers.rflags = 2;
    if (::ioctl(vcpu.get(), KVM_SET_REGS, &registers) < 0) return finish(system_error("KVM_SET_REGS"));

    // Only the compiled-in straight-line 11-instruction guest enters hardware.
    // There are no loops, external guest bytes, IRQs, disk or networking devices.
    // KVM returns on each OUT and on HLT; cap exits and interrupted retries.
    for (unsigned exits = 0; exits < 64; ++exits) {
        if (::ioctl(vcpu.get(), KVM_RUN, 0) < 0) {
            if (errno == EINTR) continue;
            return finish(system_error("KVM_RUN"));
        }
        switch (run->exit_reason) {
        case KVM_EXIT_IO: {
            if (run->io.direction != KVM_EXIT_IO_OUT || run->io.port != 0xe9 ||
                run->io.size != 1 || run->io.count != 1)
                return finish("Unexpected I/O operation in the trusted guest.");
            if (run->io.data_offset >= static_cast<std::uint64_t>(run_size))
                return finish("Invalid KVM I/O data offset.");
            const auto* bytes = reinterpret_cast<const char*>(run);
            result.output.push_back(bytes[run->io.data_offset]);
            result.instructions += 2;
            break;
        }
        case KVM_EXIT_HLT:
            ++result.instructions;
            if (result.output != "SIFA\n") return finish("Guest halted with unexpected debug output.");
            return finish();
        case KVM_EXIT_FAIL_ENTRY:
            return finish("KVM could not enter the guest CPU (reason " +
                          std::to_string(run->fail_entry.hardware_entry_failure_reason) + ").");
        case KVM_EXIT_INTERNAL_ERROR:
            return finish("KVM reported an internal CPU emulation error.");
        case KVM_EXIT_SHUTDOWN:
            return finish("Guest CPU shut down unexpectedly.");
        default:
            return finish("Unexpected KVM exit " + std::to_string(run->exit_reason) + ".");
        }
    }
    return finish("Guest exceeded the hardware exit budget.");
}
} // namespace sifa

#else
namespace sifa {
HypervisorInfo probe_hypervisor() {
    return {"unsupported", false, "Hardware guest execution currently requires x86 Linux KVM or Windows WHPX."};
}
VmResult run_hardware_guest_demo() {
    VmResult result;
    result.backend = "unsupported";
    result.error = probe_hypervisor().detail;
    return result;
}
} // namespace sifa
#endif
#endif
