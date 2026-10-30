#include "sifa/vm.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if __has_include(<WinHvPlatform.h>)
#include <WinHvPlatform.h>
#define SIFA_HAS_WHPX_SDK 1
#elif __has_include(<winhvplatform.h>)
#include <winhvplatform.h>
#define SIFA_HAS_WHPX_SDK 1
#endif

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace sifa {
#if defined(SIFA_HAS_WHPX_SDK)
namespace {

std::string hresult_message(const char* operation, HRESULT value) {
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex
           << static_cast<unsigned long>(value) << ").";
    return stream.str();
}

// Load only the Windows system copy; never search the working directory for a DLL.
// Delayed loading allows the same executable to run on systems without WHPX.
struct Api {
    HMODULE module = nullptr;
    std::string error;
    decltype(&WHvGetCapability) capability = nullptr;
    decltype(&WHvCreatePartition) create_partition = nullptr;
    decltype(&WHvSetPartitionProperty) set_property = nullptr;
    decltype(&WHvSetupPartition) setup_partition = nullptr;
    decltype(&WHvDeletePartition) delete_partition = nullptr;
    decltype(&WHvMapGpaRange) map_memory = nullptr;
    decltype(&WHvUnmapGpaRange) unmap_memory = nullptr;
    decltype(&WHvCreateVirtualProcessor) create_vp = nullptr;
    decltype(&WHvDeleteVirtualProcessor) delete_vp = nullptr;
    decltype(&WHvSetVirtualProcessorRegisters) set_registers = nullptr;
    decltype(&WHvRunVirtualProcessor) run = nullptr;
    decltype(&WHvCancelRunVirtualProcessor) cancel = nullptr;

    template<class Function>
    bool bind(Function& target, const char* name) {
        const auto address = GetProcAddress(module, name);
        static_assert(sizeof(target) == sizeof(address));
        // Win32 guarantees compatible function-pointer representations. Copying
        // the representation avoids compiler warnings about FARPROC's generic
        // signature while retaining the exact declared WHPX calling convention.
        std::memcpy(&target, &address, sizeof(target));
        if (!target) error = std::string("Windows Hypervisor Platform is missing API ") + name + ".";
        return target != nullptr;
    }

    Api() {
        module = LoadLibraryExW(L"WinHvPlatform.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            error = "Windows Hypervisor Platform is unavailable. Enable the Windows Hypervisor Platform optional feature and hardware virtualization, then restart Windows.";
            return;
        }
        if (!bind(capability, "WHvGetCapability") ||
            !bind(create_partition, "WHvCreatePartition") ||
            !bind(set_property, "WHvSetPartitionProperty") ||
            !bind(setup_partition, "WHvSetupPartition") ||
            !bind(delete_partition, "WHvDeletePartition") ||
            !bind(map_memory, "WHvMapGpaRange") ||
            !bind(unmap_memory, "WHvUnmapGpaRange") ||
            !bind(create_vp, "WHvCreateVirtualProcessor") ||
            !bind(delete_vp, "WHvDeleteVirtualProcessor") ||
            !bind(set_registers, "WHvSetVirtualProcessorRegisters") ||
            !bind(run, "WHvRunVirtualProcessor") ||
            !bind(cancel, "WHvCancelRunVirtualProcessor")) return;

        WHV_CAPABILITY capabilities{};
        UINT32 written = 0;
        const HRESULT status = capability(WHvCapabilityCodeHypervisorPresent,
                                          &capabilities, sizeof(capabilities), &written);
        if (FAILED(status)) {
            error = hresult_message("WHvGetCapability", status);
        } else if (!capabilities.HypervisorPresent) {
            error = "The Windows hypervisor is not running. Enable firmware virtualization and Windows Hypervisor Platform, then restart Windows.";
        }
    }
    ~Api() { if (module) FreeLibrary(module); }
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;
};

struct Machine {
    Api& api;
    WHV_PARTITION_HANDLE partition = nullptr;
    bool vp_created = false;
    bool mapped = false;
    void* memory = nullptr;
    static constexpr SIZE_T memory_size = 64 * 1024;

    explicit Machine(Api& functions) : api(functions) {}
    ~Machine() {
        if (vp_created) api.delete_vp(partition, 0);
        if (mapped) api.unmap_memory(partition, 0, memory_size);
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
        if (partition) api.delete_partition(partition);
    }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    bool initialize(std::string& error) {
        HRESULT status = api.create_partition(&partition);
        if (FAILED(status)) {
            error = hresult_message("WHvCreatePartition", status);
            return false;
        }
        WHV_PARTITION_PROPERTY property{};
        property.ProcessorCount = 1;
        status = api.set_property(partition, WHvPartitionPropertyCodeProcessorCount,
                                  &property, sizeof(property));
        if (FAILED(status)) {
            error = hresult_message("WHvSetPartitionProperty", status);
            return false;
        }
        status = api.setup_partition(partition);
        if (FAILED(status)) {
            error = hresult_message("WHvSetupPartition", status);
            return false;
        }
        status = api.create_vp(partition, 0, 0);
        if (FAILED(status)) {
            error = hresult_message("WHvCreateVirtualProcessor", status);
            return false;
        }
        vp_created = true;
        return true;
    }
};

// WHvRunVirtualProcessor is blocking. Cancellation is retried after the deadline
// because a single cancellation can race with a transition between VP runs.
class Deadline {
    Api& api_;
    WHV_PARTITION_HANDLE partition_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool finished_ = false;
    std::thread thread_;
public:
    Deadline(Api& api, WHV_PARTITION_HANDLE partition)
        : api_(api), partition_(partition), thread_([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_for(lock, std::chrono::seconds(2), [this] { return finished_; })) return;
            while (!finished_) {
                lock.unlock();
                api_.cancel(partition_, 0, 0);
                lock.lock();
                wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return finished_; });
            }
        }) {}
    ~Deadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }
    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;
};

} // namespace

HypervisorInfo probe_hypervisor() {
    Api api;
    if (!api.error.empty()) return {"whpx", false, api.error};
    Machine machine(api);
    std::string error;
    if (!machine.initialize(error)) return {"whpx", false, error};
    return {"whpx", true, "Windows Hypervisor Platform created a partition and virtual CPU. Android boot and GPU virtualization are not implemented."};
}

VmResult run_hardware_guest_demo() {
    VmResult result{};
    result.backend = "whpx";
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&] {
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    };
    Api api;
    if (!api.error.empty()) { result.error = api.error; return finish(); }
    Machine machine(api);
    if (!machine.initialize(result.error)) return finish();

    machine.memory = VirtualAlloc(nullptr, Machine::memory_size,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!machine.memory) {
        result.error = "VirtualAlloc failed for the 64 KiB demo guest.";
        return finish();
    }
    // A fixed, trusted real-mode program: MOV AL, byte; OUT 0xe9, AL; ... HLT.
    // No user image or APK is accepted by this proof-of-execution backend.
    const auto& guest = demo_guest_bytes();
    constexpr UINT64 entry = 0x1000;
    std::memcpy(static_cast<unsigned char*>(machine.memory) + entry, guest.data(), guest.size());
    const auto flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
    HRESULT status = api.map_memory(machine.partition, machine.memory, 0,
                                    Machine::memory_size, flags);
    if (FAILED(status)) {
        result.error = hresult_message("WHvMapGpaRange", status);
        return finish();
    }
    machine.mapped = true;

    const WHV_REGISTER_NAME names[] = {
        WHvX64RegisterRip, WHvX64RegisterRflags, WHvX64RegisterCr0,
        WHvX64RegisterCr3, WHvX64RegisterCr4, WHvX64RegisterEfer,
        WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs,
        WHvX64RegisterSs, WHvX64RegisterFs, WHvX64RegisterGs,
        WHvX64RegisterRax, WHvX64RegisterRsp
    };
    WHV_REGISTER_VALUE registers[sizeof(names) / sizeof(names[0])]{};
    registers[0].Reg64 = entry;
    registers[1].Reg64 = 2;
    registers[2].Reg64 = 0x10; // CR0.ET; PE/PG clear: 16-bit real mode.
    for (unsigned i = 6; i <= 11; ++i) {
        registers[i].Segment.Base = 0;
        registers[i].Segment.Limit = 0xffff;
        registers[i].Segment.Selector = 0;
        registers[i].Segment.Attributes = (i == 6) ? 0x009b : 0x0093;
    }
    registers[13].Reg64 = 0xfffe;
    status = api.set_registers(machine.partition, 0, names,
                               static_cast<UINT32>(sizeof(names) / sizeof(names[0])), registers);
    if (FAILED(status)) {
        result.error = hresult_message("WHvSetVirtualProcessorRegisters", status);
        return finish();
    }

    Deadline deadline(api, machine.partition);
    for (unsigned exits = 0; exits < 16; ++exits) {
        WHV_RUN_VP_EXIT_CONTEXT context{};
        status = api.run(machine.partition, 0, &context, sizeof(context));
        if (FAILED(status)) {
            result.error = hresult_message("WHvRunVirtualProcessor", status);
            return finish();
        }
        if (context.ExitReason == WHvRunVpExitReasonX64Halt) {
            result.ok = result.output == "SIFA\n";
            // This fixed, branch-free program has exactly eleven instructions.
            // This is not a hardware performance counter or an Android benchmark.
            if (result.ok) result.instructions = 11;
            else result.error = "The guest halted without producing the expected SIFA greeting.";
            return finish();
        }
        if (context.ExitReason == WHvRunVpExitReasonCanceled) {
            result.error = "WHPX demo exceeded its two-second execution deadline.";
            return finish();
        }
        if (context.ExitReason != WHvRunVpExitReasonX64IoPortAccess) {
            result.error = "WHPX stopped with unsupported exit reason " +
                std::to_string(static_cast<unsigned>(context.ExitReason)) + ".";
            return finish();
        }
        const auto& io = context.IoPortAccess;
        if (!io.AccessInfo.IsWrite || io.AccessInfo.AccessSize != 1 ||
            io.AccessInfo.StringOp || io.AccessInfo.RepPrefix || io.PortNumber != 0xe9 ||
            context.VpContext.InstructionLength != 2 || result.output.size() >= 5) {
            result.error = "The demo guest attempted an unexpected I/O operation.";
            return finish();
        }
        result.output.push_back(static_cast<char>(io.Rax & 0xff));
        const WHV_REGISTER_NAME rip_name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE rip{};
        rip.Reg64 = context.VpContext.Rip + context.VpContext.InstructionLength;
        status = api.set_registers(machine.partition, 0, &rip_name, 1, &rip);
        if (FAILED(status)) {
            result.error = hresult_message("Advancing guest RIP", status);
            return finish();
        }
    }
    result.error = "WHPX demo exceeded its VM-exit budget.";
    return finish();
}

#else

HypervisorInfo probe_hypervisor() {
    return {"whpx", false, "This executable was built without Windows Hypervisor Platform SDK headers. Rebuild with the Windows 10/11 SDK to enable the WHPX backend."};
}
VmResult run_hardware_guest_demo() {
    VmResult result{};
    result.backend = "whpx";
    result.error = probe_hypervisor().detail;
    return result;
}

#endif
} // namespace sifa
#endif // _WIN32
