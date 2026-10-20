#include "save_transaction.h"
#include "host_diagnostics.h"
#ifdef RECOMP_FULL_PROGRAM
#include "cri_service_adapter.h"
#include "audio_output.h"
#include "custom_music.h"
#include "soundtrack_adapter.h"
#include "d3d_presenter.h"
#include "d3d_frame_adapter.h"
#include "fiber_adapter.h"
#include "input_adapter.h"
#include "free_camera_adapter.h"
#include "viewer_menu_adapter.h"
#include "scene_freeze_adapter.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#endif
#include "runtime.h"
#include "stop_report.h"
#include "xbox_memory_layout.h"
#include "xbe_parser.h"

#ifndef RECOMP_FULL_PROGRAM
#include "runner_imports.h"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

extern "C" const char *recomp_disc_root_path;

#if defined(_WIN32) && defined(RECOMP_FULL_PROGRAM)
BOOL WINAPI handleConsoleControl(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    recomp_d3d_frame_adapter_request_console_break();
    return TRUE;
}
#endif

constexpr std::size_t kRamSize = 64u * 1024u * 1024u;
constexpr std::uint32_t kStackTop = 0x00f7fff0u;
constexpr std::uint32_t kKernelDataBase = 0x00740000u;
std::uint64_t inputHostAfterPoll = 0u;
std::uint64_t inputWaitAfterPoll = 0u;
std::string inputResumeFile;
std::string inputPausedAnalogFile;

bool inputResumeFileExists()
{
    std::error_code error;
    const bool exists = std::filesystem::exists(inputResumeFile, error);
    if (error || (exists &&
            !std::filesystem::is_regular_file(inputResumeFile, error))) {
        std::cerr << "recomp input: invalid resume file: " << inputResumeFile
                  << " (" << error.message() << ")\n";
        std::exit(64);
    }
    return exists;
}

extern "C" void xbe_entry_point(void);
#ifdef RECOMP_FULL_PROGRAM
extern "C" RecompFunction recomp_program_lookup(
    std::uint32_t guest_address);

RecompInputPulseSource inputPulseSource;

void samplePausedAnalogFile()
{
    if (inputPausedAnalogFile.empty() || !inputPulseSource.paused ||
        inputPulseSource.pending_samples != 0u) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point retry_deadline;
    static const char *retry_operation = nullptr;
    static std::error_code retry_error;
    const auto fail = [](const char *operation, const std::error_code &error) {
        std::cerr << "recomp input: invalid/unreadable paused analog file: "
                  << inputPausedAnalogFile << " operation=" << operation
                  << " error=" << error.category().name() << ':' << error.value()
                  << " (" << error.message() << ")\n";
        std::exit(64);
    };
    const auto io_error = [&](const char *operation, const std::error_code &error) {
#ifdef _WIN32
        if (error.category() == std::system_category() &&
            (error.value() == ERROR_SHARING_VIOLATION ||
             error.value() == ERROR_LOCK_VIOLATION)) {
            const auto now = Clock::now();
            if (retry_operation == nullptr) {
                retry_deadline = now + std::chrono::milliseconds(250);
            }
            retry_operation = operation;
            retry_error = error;
            if (now < retry_deadline) { return; }
        }
#endif
        fail(operation, error);
    };
    const auto crt_error = []() {
        const int value = errno;
#ifdef _WIN32
        unsigned long os_error = 0u;
        _get_doserrno(&os_error);
        if (os_error != 0u) {
            return std::error_code(static_cast<int>(os_error), std::system_category());
        }
#endif
        return std::error_code(value, std::generic_category());
    };
    std::error_code error;
    const bool exists = std::filesystem::exists(inputPausedAnalogFile, error);
    if (error) { io_error("exists", error); return; }
    if (!exists) { retry_operation = nullptr; return; }
    if (retry_operation != nullptr && Clock::now() >= retry_deadline) {
        fail(retry_operation, retry_error);
    }
    const bool regular = std::filesystem::is_regular_file(inputPausedAnalogFile, error);
    if (error) { io_error("file-type", error); return; }
    if (!regular) { fail("file-type", {}); }
#ifdef _WIN32
    _set_doserrno(0u);
#endif
    FILE *file = std::fopen(inputPausedAnalogFile.c_str(), "rb");
    if (file == nullptr) { io_error("open", crt_error()); return; }
    char command[8]{};
#ifdef _WIN32
    _set_doserrno(0u);
#endif
    const std::size_t size = std::fread(command, 1u, sizeof command, file);
    const std::error_code read_error = crt_error();
    const bool read_failed = std::ferror(file) != 0;
    std::fclose(file);
    if (read_failed) { io_error("read", read_error); return; }
    const bool digital = command[0] == 'd';
    const std::size_t length = digital ? 5u : 1u;
    if (!(size == length || (size == length + 1u && command[length] == '\n') ||
          (size == length + 2u && command[length] == '\r' && command[length + 1u] == '\n'))) {
        fail("command-format", {});
    }
    std::uint16_t mask = 0u;
    std::uint8_t index = 0u;
    if (digital) {
        for (std::size_t i = 1u; i < length; ++i) {
            const char digit = command[i];
            unsigned value = 0u;
            if (digit >= '0' && digit <= '9') { value = digit - '0'; }
            else if (digit >= 'a' && digit <= 'f') { value = digit - 'a' + 10u; }
            else if (digit >= 'A' && digit <= 'F') { value = digit - 'A' + 10u; }
            else { fail("command-format", {}); }
            mask = static_cast<std::uint16_t>((mask << 4u) | value);
        }
        if (mask == 0u || (mask & 0xff00u) != 0u) { fail("command-format", {}); }
    } else {
        if (command[0] < '0' || command[0] > '7') { fail("command-format", {}); }
        index = static_cast<std::uint8_t>(command[0] - '0');
    }
    const bool removed = std::filesystem::remove(inputPausedAnalogFile, error);
    if (error) { io_error("remove", error); return; }
    if (!removed) { fail("remove", {}); }
    retry_operation = nullptr;
    const bool queued = digital
        ? recomp_input_pulse_source_press_buttons(&inputPulseSource, mask)
        : recomp_input_pulse_source_press_analog(&inputPulseSource, index);
    if (!queued) { fail("queue-pulse", {}); }
    if (digital) {
        std::cerr << "recomp input: paused digital pulse mask=0x" << std::hex
                  << static_cast<unsigned>(mask) << std::dec;
    } else {
        std::cerr << "recomp input: paused analog pulse index="
                  << static_cast<unsigned>(index);
    }
    std::cerr << " script_poll=" << inputPulseSource.sample_count << '\n';
}

bool sampleInputPulse(std::uint32_t port, RecompInputGamepad *gamepad)
{
    /* Scripts drive port 0 only; other ports appear after host handover. */
    if (port != 0u) {
        return inputPulseSource.base != nullptr &&
            inputPulseSource.base(port, gamepad);
    }
    if (inputWaitAfterPoll != 0u &&
        inputPulseSource.sample_count >= inputWaitAfterPoll) {
        if (!inputPulseSource.paused) {
            inputPulseSource.paused = true;
            std::cerr << "recomp input: script waiting after poll="
                      << inputPulseSource.sample_count << '\n';
        }
        if (inputPulseSource.pending_samples == 0u &&
            inputResumeFileExists()) {
            inputPulseSource.paused = false;
            inputWaitAfterPoll = 0u;
            std::cerr << "recomp input: script resumed after poll="
                      << inputPulseSource.sample_count << '\n';
        }
    }
    if (inputHostAfterPoll != 0u &&
        inputPulseSource.sample_count >= inputHostAfterPoll &&
        inputPulseSource.base == nullptr) {
        inputPulseSource.base = recomp_input_host_sample;
        std::cerr << "recomp input: host enabled after poll="
                  << inputPulseSource.sample_count << '\n';
    }
    samplePausedAnalogFile();
    return recomp_input_pulse_source_sample(&inputPulseSource, gamepad);
}
#endif

bool parsePositiveU64(const std::string& value, std::uint64_t& parsed)
{
    if (value.empty()) {
        return false;
    }

    std::uint64_t result = 0u;
    for (char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) /
                10u) {
            return false;
        }
        result = result * 10u + digit;
    }
    if (result == 0u) {
        return false;
    }

    parsed = result;
    return true;
}

void writeU16(
    std::vector<std::uint8_t>& memory,
    std::uint32_t address,
    std::uint16_t value)
{
    memory[address] = static_cast<std::uint8_t>(value);
    memory[address + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void writeU32(
    std::vector<std::uint8_t>& memory,
    std::uint32_t address,
    std::uint32_t value)
{
    writeU16(memory, address, static_cast<std::uint16_t>(value));
    writeU16(memory, address + 2u, static_cast<std::uint16_t>(value >> 16u));
}

/* The eleven data exports this XBE imports. Every other imported ordinal is a
   function and must stay callable so an unimplemented one fails by name. */
std::uint32_t kernelDataAddress(std::uint32_t ordinal)
{
    switch (ordinal) {
    case 322u: return kKernelDataBase;          // XboxHardwareInfo
    case 324u: return kKernelDataBase + 0x10u;  // XboxKrnlVersion
    case 16u: return kKernelDataBase + 0x20u;   // ExEventObjectType
    case 40u: return kKernelDataBase + 0x30u;   // HalDiskCachePartitionCount
    case 156u: return kKernelDataBase + 0x40u;  // KeTickCount
    case 164u: return kKernelDataBase + 0x50u;  // LaunchDataPage
    case 259u: return kKernelDataBase + 0x60u;  // PsThreadObjectType
    case 323u: return kKernelDataBase + 0x70u;  // XboxHDKey
    case 325u: return kKernelDataBase + 0x80u;  // XboxSignatureKey
    case 354u: return kKernelDataBase + 0x90u;  // XboxAlternateSignatureKeys
    case 356u: return kKernelDataBase + 0xa0u;  // HalBootSMCVideoMode
    default: return 0u;
    }
}

bool materializeKernelDataExports(
    const doaxbv::XbeMetadata& metadata,
    std::vector<std::uint8_t>& memory,
    std::string& error)
{
    if (kKernelDataBase + 0x420u > memory.size()) {
        error = "kernel data area leaves guest RAM";
        return false;
    }

    writeU32(memory, kKernelDataBase, 0u);
    memory[kKernelDataBase + 4u] = 0xa1u;
    memory[kKernelDataBase + 5u] = 0xb1u;
    writeU16(memory, kKernelDataBase + 0x10u, 1u);
    writeU16(memory, kKernelDataBase + 0x12u, 0u);
    writeU16(memory, kKernelDataBase + 0x14u, 5849u);
    writeU16(memory, kKernelDataBase + 0x16u, 0u);
    writeU32(memory, kKernelDataBase + 0x30u, 3u);

    for (const auto& thunk : metadata.kernelThunks) {
        const std::uint32_t dataAddress = kernelDataAddress(thunk.ordinal);

        if (dataAddress == 0u) {
            continue;
        }
        const std::uint64_t slot =
            static_cast<std::uint64_t>(metadata.selectedKernelThunkAddress) +
            static_cast<std::uint64_t>(thunk.index) * 4u;
        if (slot + 4u > memory.size()) {
            error = "kernel data thunk slot leaves guest RAM";
            return false;
        }
        writeU32(memory, static_cast<std::uint32_t>(slot), dataAddress);
    }
    return true;
}

bool materializeStartupThread(
    const doaxbv::XbeMetadata& metadata,
    std::vector<std::uint8_t>& memory,
    std::string& error)
{
    if (!metadata.tls.has_value()) {
        return true;
    }

    const auto& tls = *metadata.tls;
    if (tls.dataEndAddress < tls.dataStartAddress) {
        error = "TLS initialized-data range is reversed";
        return false;
    }
    const std::uint64_t tlsSize =
        static_cast<std::uint64_t>(tls.dataEndAddress) -
        tls.dataStartAddress + tls.sizeOfZeroFill;
    const std::uint64_t tlsBlock =
        static_cast<std::uint64_t>(XBOX_STARTUP_THREAD_STACK_SLOT) + 4u;
    const std::uint64_t stackBase = tlsBlock + tlsSize;
    if (stackBase > memory.size() ||
        static_cast<std::uint64_t>(XBOX_STARTUP_THREAD_OBJECT) + 0x2cu >
            memory.size()) {
        error = "startup thread state leaves guest RAM";
        return false;
    }

    std::memset(
        memory.data() + XBOX_STARTUP_THREAD_STACK_SLOT,
        0,
        static_cast<std::size_t>(4u + tlsSize));
    std::memset(memory.data() + XBOX_STARTUP_THREAD_OBJECT, 0, 0x2cu);

    // The lifter flattens FS-relative PCR reads to guest address zero.
    writeU32(memory, 0x00u, 0xffffffffu); // ExceptionList
    writeU32(memory, 0x04u, static_cast<std::uint32_t>(stackBase));
    writeU32(memory, 0x1cu, 0u);          // SelfPcr
    writeU32(memory, 0x20u, 0x28u);      // Prcb
    writeU32(memory, 0x24u, 0u);         // Irql
    writeU32(memory, 0x28u, XBOX_STARTUP_THREAD_OBJECT);
    writeU32(
        memory,
        XBOX_STARTUP_THREAD_OBJECT + 0x28u,
        XBOX_STARTUP_THREAD_STACK_SLOT);
    return true;
}

bool mapXbe(
    const doaxbv::XbeParseResult& parsed,
    std::vector<std::uint8_t>& memory,
    std::string& error)
{
    const auto& metadata = parsed.metadata;
    const std::uint64_t image_end =
        static_cast<std::uint64_t>(metadata.header.baseAddress) +
        metadata.header.sizeImage;
    const std::uint64_t header_end =
        static_cast<std::uint64_t>(metadata.header.baseAddress) +
        metadata.header.sizeHeaders;

    if (image_end > memory.size() || header_end > memory.size() ||
        metadata.header.sizeHeaders > parsed.fileBytes.size()) {
        error = "XBE image or headers leave guest RAM";
        return false;
    }

    std::memcpy(
        memory.data() + metadata.header.baseAddress,
        parsed.fileBytes.data(),
        metadata.header.sizeHeaders);

    for (const auto& section : metadata.sections) {
        const std::uint64_t virtual_end =
            static_cast<std::uint64_t>(section.virtualAddress) +
            std::max(section.virtualSize, section.rawSize);
        const std::uint64_t raw_end =
            static_cast<std::uint64_t>(section.rawAddress) + section.rawSize;

        if (virtual_end > memory.size() || raw_end > parsed.fileBytes.size()) {
            error = "XBE section leaves its source or guest-RAM range";
            return false;
        }
        if (!section.digestMatches) {
            error = "XBE section digest mismatch: " + section.name;
            return false;
        }

        std::memset(
            memory.data() + section.virtualAddress,
            0,
            std::max(section.virtualSize, section.rawSize));
        std::memcpy(
            memory.data() + section.virtualAddress,
            parsed.fileBytes.data() + section.rawAddress,
            section.rawSize);
    }

    if (!materializeKernelDataExports(metadata, memory, error)) {
        return false;
    }
    if (!materializeStartupThread(metadata, memory, error)) {
        return false;
    }

    return true;
}

#ifndef RECOMP_FULL_PROGRAM
const char *importName(RecompImportKind kind)
{
    switch (kind) {
    case RECOMP_IMPORT_CREATE_THREAD:
        return "CreateThread";
    case RECOMP_IMPORT_BOOT_TO_DASH:
        return "XapiBootToDash";
    case RECOMP_IMPORT_EXIT_THREAD:
        return "ExitThread";
    default:
        return "none";
    }
}
#endif

} // namespace

int main(int argc, char **argv)
{
    recomp_install_host_diagnostics();
#ifdef _WIN32
    /* Every guest thread is a fiber on this one. At Normal priority, busy
       desktop apps left it ready but unscheduled for 20-50 ms. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif

    std::string xbePath;
    std::string expectStop;
    std::string stopAt;
    std::string milestoneLog;
    bool inputStartPulseEnabled = false;
    bool vsyncPresent = false;
    std::vector<std::uint64_t> inputStartPulsePolls;
    std::vector<std::uint64_t> inputAPulsePolls;
std::vector<std::pair<std::uint64_t, std::uint16_t>> inputButtonsPulses;
struct AnalogPulse {
    std::uint64_t poll;
    std::uint8_t index;
    std::uint8_t value;
};
std::vector<AnalogPulse> inputAnalogPulses;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool wantsValue = arg == "--xbe" || arg == "--expect-stop" ||
            arg == "--stop-at" || arg == "--milestone-log" ||
            arg == "--input-start-pulse-at" ||
            arg == "--input-a-pulse-at" || arg == "--input-buttons-at" ||
            arg == "--input-analog-at" || arg == "--input-host-after-poll" ||
            arg == "--input-wait-after-poll" || arg == "--input-resume-file" ||
            arg == "--input-paused-analog-file";

        if (wantsValue && i + 1 >= argc) {
            std::cerr << "recomp runner: " << arg << " needs a value\n";
            return 64;
        }
        if (arg == "--xbe") {
            xbePath = argv[++i];
        } else if (arg == "--vsync") {
            vsyncPresent = true;
        } else if (arg == "--expect-stop") {
            expectStop = argv[++i];
        } else if (arg == "--stop-at") {
            stopAt = argv[++i];
        } else if (arg == "--milestone-log") {
            milestoneLog = argv[++i];
        } else if (arg == "--input-wait-after-poll") {
            const std::string value = argv[++i];
            if (!parsePositiveU64(value, inputWaitAfterPoll) ||
                inputWaitAfterPoll == std::numeric_limits<std::uint64_t>::max()) {
                std::cerr << "recomp runner: invalid input wait poll '"
                          << value << "'\n";
                return 64;
            }
            inputStartPulseEnabled = true;
        } else if (arg == "--input-resume-file") {
            inputResumeFile = argv[++i];
            if (inputResumeFile.empty()) {
                std::cerr << "recomp runner: empty input resume file\n";
                return 64;
            }
        } else if (arg == "--input-paused-analog-file") {
            inputPausedAnalogFile = argv[++i];
            if (inputPausedAnalogFile.empty()) {
                std::cerr << "recomp runner: empty paused analog file\n";
                return 64;
            }
        } else if (arg == "--input-host-after-poll") {
            const std::string value = argv[++i];
            if (!parsePositiveU64(value, inputHostAfterPoll)) {
                std::cerr << "recomp runner: invalid host handover poll '"
                          << value << "'\n";
                return 64;
            }
            inputStartPulseEnabled = true;
        } else if (arg == "--input-start-pulse-at") {
            const std::string value = argv[++i];
            std::uint64_t parsedPoll = 0u;
            if (!parsePositiveU64(value, parsedPoll)) {
                std::cerr << "recomp runner: invalid START pulse poll '"
                          << value << "'\n";
                return 64;
            }
            if (inputStartPulsePolls.size() >=
                    RECOMP_INPUT_PULSE_POLL_CAPACITY) {
                std::cerr << "recomp runner: at most "
                          << RECOMP_INPUT_PULSE_POLL_CAPACITY
                          << " START pulse polls\n";
                return 64;
            }
            inputStartPulsePolls.push_back(parsedPoll);
            inputStartPulseEnabled = true;
        } else if (arg == "--input-a-pulse-at") {
            const std::string value = argv[++i];
            std::uint64_t parsedPoll = 0u;
            if (!parsePositiveU64(value, parsedPoll)) {
                std::cerr << "recomp runner: invalid A pulse poll '"
                          << value << "'\n";
                return 64;
            }
            if (inputAPulsePolls.size() >=
                    RECOMP_INPUT_A_PULSE_POLL_CAPACITY) {
                std::cerr << "recomp runner: at most "
                          << RECOMP_INPUT_A_PULSE_POLL_CAPACITY
                          << " A pulse polls\n";
                return 64;
            }
            inputAPulsePolls.push_back(parsedPoll);
            inputStartPulseEnabled = true;
        } else if (arg == "--input-buttons-at") {
            const std::string value = argv[++i];
            const std::size_t separator = value.find(':');
            if (separator == std::string::npos) {
                std::cerr << "recomp runner: invalid button pulse '"
                          << value << "' (expected <poll>:<hexmask>)\n";
                return 64;
            }
            std::uint64_t parsedPoll = 0u;
            if (!parsePositiveU64(value.substr(0u, separator), parsedPoll)) {
                std::cerr << "recomp runner: invalid button pulse poll '"
                          << value.substr(0u, separator) << "'\n";
                return 64;
            }
            std::uint16_t parsedMask = 0u;
            try {
                const unsigned long mask =
                    std::stoul(value.substr(separator + 1u), nullptr, 16);
                if (mask == 0u || mask > 0xfffful) {
                    std::cerr << "recomp runner: invalid button pulse mask '"
                              << value.substr(separator + 1u) << "'\n";
                    return 64;
                }
                /* The Xbox digital word has eight bits, 0x0001-0x0080. The
                   face buttons, Black/White and the triggers are analog and
                   travel in a separate byte array, so a high bit here is set,
                   copied to the guest, and correctly ignored -- silently. Six
                   rounds of mask sweeps were partly measuring nothing before
                   this was caught. Fail closed instead. */
                if ((mask & ~0x00fful) != 0u) {
                    std::cerr << "recomp runner: button mask '"
                              << value.substr(separator + 1u)
                              << "' has bits above 0x00ff, which the digital"
                                 " button word cannot carry.\n"
                              << "  legal: 0x0001-0x0008 dpad, 0x0010 START,"
                                 " 0x0020 BACK, 0x0040/0x0080 thumb clicks\n"
                              << "  for A/B/X/Y, Black/White and the triggers"
                                 " use --input-analog-at <poll>:<index>:<value>"
                                 "\n";
                    return 64;
                }
                parsedMask = static_cast<std::uint16_t>(mask);
            } catch (const std::exception&) {
                std::cerr << "recomp runner: invalid button pulse mask '"
                          << value.substr(separator + 1u) << "'\n";
                return 64;
            }
            if (inputButtonsPulses.size() >=
                    RECOMP_INPUT_BUTTONS_PULSE_CAPACITY) {
                std::cerr << "recomp runner: at most "
                          << RECOMP_INPUT_BUTTONS_PULSE_CAPACITY
                          << " button pulses\n";
                return 64;
            }
            inputButtonsPulses.emplace_back(parsedPoll, parsedMask);
            inputStartPulseEnabled = true;
        } else if (arg == "--input-analog-at") {
            /* <poll>:<index>:<value>. Index is the guest analog ordering from
               xinput_xbox.h: 0=A 1=B 2=X 3=Y 4=Black 5=White 6=LTrig 7=RTrig.
               Value is 0-255; the guest threshold is 30. */
            const std::string value = argv[++i];
            const std::size_t first = value.find(':');
            const std::size_t second =
                first == std::string::npos ? std::string::npos
                                           : value.find(':', first + 1u);
            if (first == std::string::npos || second == std::string::npos) {
                std::cerr << "recomp runner: invalid analog pulse '" << value
                          << "' (expected <poll>:<index>:<value>)\n";
                return 64;
            }
            std::uint64_t parsedPoll = 0u;
            if (!parsePositiveU64(value.substr(0u, first), parsedPoll)) {
                std::cerr << "recomp runner: invalid analog pulse poll '"
                          << value.substr(0u, first) << "'\n";
                return 64;
            }
            std::uint8_t parsedIndex = 0u;
            std::uint8_t parsedValue = 0u;
            try {
                const unsigned long index = std::stoul(
                    value.substr(first + 1u, second - first - 1u), nullptr, 10);
                const unsigned long level =
                    std::stoul(value.substr(second + 1u), nullptr, 10);
                if (index >= RECOMP_INPUT_ANALOG_BUTTON_COUNT) {
                    std::cerr << "recomp runner: analog index " << index
                              << " out of range, expected 0-7"
                                 " (0=A 1=B 2=X 3=Y 4=Black 5=White"
                                 " 6=LTrig 7=RTrig)\n";
                    return 64;
                }
                if (level > 0xfful) {
                    std::cerr << "recomp runner: analog value " << level
                              << " out of range, expected 0-255\n";
                    return 64;
                }
                parsedIndex = static_cast<std::uint8_t>(index);
                parsedValue = static_cast<std::uint8_t>(level);
            } catch (const std::exception&) {
                std::cerr << "recomp runner: invalid analog pulse '" << value
                          << "'\n";
                return 64;
            }
            if (inputAnalogPulses.size() >=
                    RECOMP_INPUT_ANALOG_PULSE_CAPACITY) {
                std::cerr << "recomp runner: at most "
                          << RECOMP_INPUT_ANALOG_PULSE_CAPACITY
                          << " analog pulses\n";
                return 64;
            }
            inputAnalogPulses.push_back(
                AnalogPulse{parsedPoll, parsedIndex, parsedValue});
            inputStartPulseEnabled = true;
        } else {
            std::cerr << "usage: recomp_runner --xbe <path>"
                         " [--expect-stop <id>] [--stop-at <id>]"
                         " [--milestone-log <path>]"
                         " [--vsync]"
                         " [--input-host-after-poll <poll>]"
                         " [--input-wait-after-poll <poll> --input-resume-file <path>]"
                         " [--input-paused-analog-file <path>]"
                         " [--input-start-pulse-at <poll>]"
                         " [--input-buttons-at <poll>:<hexmask>]"
                         " [--input-analog-at <poll>:<index>:<value>]\n";
            return 64;
        }
    }
    if ((inputWaitAfterPoll != 0u) != !inputResumeFile.empty() ||
        (inputWaitAfterPoll != 0u && inputHostAfterPoll != 0u)) {
        std::cerr << "recomp runner: input wait needs a resume file and cannot"
                     " be combined with host handover\n";
        return 64;
    }
    if (inputWaitAfterPoll != 0u && inputResumeFileExists()) {
        std::cerr << "recomp runner: input resume file already exists\n";
        return 64;
    }
    bool sameInputFiles = false;
    if (!inputPausedAnalogFile.empty() && !inputResumeFile.empty()) {
        std::error_code error;
        const auto analog = std::filesystem::weakly_canonical(
            std::filesystem::absolute(inputPausedAnalogFile, error), error);
        if (error) { return 64; }
        const auto resume = std::filesystem::weakly_canonical(
            std::filesystem::absolute(inputResumeFile, error), error);
        if (error) { return 64; }
#ifdef _WIN32
        sameInputFiles = _wcsicmp(analog.c_str(), resume.c_str()) == 0;
#else
        sameInputFiles = analog == resume;
#endif
    }
    if (!inputPausedAnalogFile.empty() &&
        (inputWaitAfterPoll == 0u || sameInputFiles)) {
        std::cerr << "recomp runner: paused analog input needs a wait and a"
                     " distinct file from resume\n";
        return 64;
    }
    if (!inputPausedAnalogFile.empty()) {
        std::error_code error;
        if (std::filesystem::exists(inputPausedAnalogFile, error) || error) {
            std::cerr << "recomp runner: paused analog file already exists"
                         " or cannot be checked\n";
            return 64;
        }
    }
    if (xbePath.empty()) {
        std::cerr << "usage: recomp_runner --xbe <path>"
                     " [--expect-stop <id>] [--stop-at <id>]"
                     " [--milestone-log <path>]"
                     " [--input-host-after-poll <poll>]"
                     " [--input-wait-after-poll <poll> --input-resume-file <path>]"
                     " [--input-paused-analog-file <path>]"
                     " [--input-start-pulse-at <poll>]"
                     " [--input-buttons-at <poll>:<hexmask>]"
                     " [--input-analog-at <poll>:<index>:<value>]\n";
        return 64;
    }
    recomp_stop_configure(
        expectStop.empty() ? nullptr : expectStop.c_str(),
        milestoneLog.empty() ? nullptr : milestoneLog.c_str());
    recomp_stop_configure_boundary(
        stopAt.empty() ? nullptr : stopAt.c_str());

#if defined(_WIN32) && defined(RECOMP_FULL_PROGRAM)
    if (!SetConsoleCtrlHandler(handleConsoleControl, TRUE)) {
        std::cerr << "recomp runner: console handler installation failed: "
                  << GetLastError() << '\n';
        return 1;
    }
#endif

    doaxbv::XbeParser parser;
    doaxbv::XbeParseResult parsed = parser.parse(xbePath.c_str());
    if (!parsed.ok()) {
        std::cerr << "recomp runner: " << parsed.errors.front() << '\n';
        return 1;
    }

    {
        std::filesystem::path xbe_path = std::filesystem::absolute(xbePath);
        std::filesystem::path parent = xbe_path.parent_path();
        if (parent.empty()) {
            parent = std::filesystem::current_path();
        }
        static std::string disc_root = parent.string();
        recomp_disc_root_path = disc_root.c_str();
    }

    if (!recomp_save_initialize(recomp_disc_root_path)) {
        std::cerr << "recomp runner: save recovery failed; guest not started\n";
        return 2;
    }

    std::vector<std::uint8_t> memory(kRamSize, 0u);
    std::string error;
    if (!mapXbe(parsed, memory, error)) {
        std::cerr << "recomp runner: " << error << '\n';
        return 1;
    }
#ifdef _WIN32
    /* Whole-game display passes find and undo their guest writes through
       write tracking, which only VirtualAlloc can provide. */
    auto *ram = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, memory.size(),
        MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    if (ram == nullptr) {
        std::cerr << "recomp runner: guest RAM allocation failed\n";
        return 1;
    }
    std::memcpy(ram, memory.data(), memory.size());
    std::vector<std::uint8_t>().swap(memory);
#else
    // Without write tracking, game_rollback.c reports no display passes.
    std::uint8_t *ram = memory.data();
#endif

    const RecompMemoryRegion region = {
        0u,
        kRamSize,
        ram,
    };
#ifndef RECOMP_FULL_PROGRAM
    std::vector<RecompMemoryAccess> accesses(4096u);
    const RecompFunctionEntry functions[] = {
        {parsed.metadata.selectedEntryPoint, xbe_entry_point},
    };
#endif

    recomp_runtime_init(
        &region,
        1u,
#ifdef RECOMP_FULL_PROGRAM
        nullptr,
        0u,
        nullptr,
        0u);
    recomp_runtime_set_lookup(recomp_program_lookup);
#else
        accesses.data(),
        accesses.size(),
        functions,
        1u);
#endif
    recomp_runtime.registers.esp = kStackTop;
#ifdef RECOMP_FULL_PROGRAM
    recomp_cri_service_adapter_reset();
    std::atexit(recomp_audio_output_shutdown);
    recomp_audio_output_initialize();
    {
        // run_game.py points this at the release's private\UserMusic.
        const char *music = std::getenv("RECOMP_USER_MUSIC");
        static const std::string music_folder = music && *music ? std::string(music) :
            (std::filesystem::path(recomp_disc_root_path) / "UserMusic").string();
        recomp_music_initialize(music_folder.c_str());
    }
    std::atexit([] {
        recomp_soundtrack_shutdown(); // closes decoders before Media Foundation stops
        recomp_music_shutdown();
    });
    recomp_fiber_adapter_reset();
    recomp_input_adapter_reset();
    recomp_input_adapter_set_feedback_sink(recomp_input_host_set_vibration);
    std::atexit(recomp_input_host_stop_vibration);
    /* The options defaults table turns Vibration off; default it on. */
    *recomp_memory(0x003a4c35u, 1u) = 1u;
    /* The guest Swap seam paces updates independently of the host
       monitor. --vsync additionally synchronizes presentation to scanout. */
    recomp_d3d_presenter_set_immediate_present(!vsyncPresent);
    if (inputStartPulseEnabled) {
        recomp_input_pulse_source_init(
            &inputPulseSource,
            nullptr,
            inputStartPulsePolls.empty() ? 0u : inputStartPulsePolls.front());
        for (std::size_t i = 1u; i < inputStartPulsePolls.size(); ++i) {
            recomp_input_pulse_source_add_poll(
                &inputPulseSource, inputStartPulsePolls[i]);
        }
        for (std::uint64_t poll : inputAPulsePolls) {
            recomp_input_pulse_source_add_a_poll(&inputPulseSource, poll);
        }
        for (const auto& buttonsPulse : inputButtonsPulses) {
            recomp_input_pulse_source_add_buttons_poll(
                &inputPulseSource, buttonsPulse.first, buttonsPulse.second);
        }
        for (const auto& analogPulse : inputAnalogPulses) {
            recomp_input_pulse_source_add_analog_poll(
                &inputPulseSource, analogPulse.poll, analogPulse.index,
                analogPulse.value);
        }
        recomp_free_camera_set_source(sampleInputPulse);
        recomp_input_adapter_set_source(recomp_free_camera_sample);
        std::cerr << "recomp input: deterministic START pulse poll=";
        for (std::size_t i = 0u; i < inputStartPulsePolls.size(); ++i) {
            std::cerr << (i == 0u ? "" : ",") << inputStartPulsePolls[i];
        }
        std::cerr << " A pulse poll=";
        for (std::size_t i = 0u; i < inputAPulsePolls.size(); ++i) {
            std::cerr << (i == 0u ? "" : ",") << inputAPulsePolls[i];
        }
        std::cerr << " button pulses=";
        for (std::size_t i = 0u; i < inputButtonsPulses.size(); ++i) {
            std::cerr << (i == 0u ? "" : ",") << std::hex
                      << inputButtonsPulses[i].first << ":"
                      << inputButtonsPulses[i].second;
        }
        std::cerr << std::dec;
        std::cerr << " analog pulses=";
        for (std::size_t i = 0u; i < inputAnalogPulses.size(); ++i) {
            std::cerr << (i == 0u ? "" : ",") << inputAnalogPulses[i].poll
                      << ":" << static_cast<unsigned>(inputAnalogPulses[i].index)
                      << ":"
                      << static_cast<unsigned>(inputAnalogPulses[i].value);
        }
        std::cerr << '\n';
    } else {
        recomp_free_camera_set_source(recomp_input_host_sample);
        recomp_input_adapter_set_source(recomp_free_camera_sample);
    }
    /* The scene freeze, the viewer and the suit switch all run behind the
       free camera; off, task switches go unhooked. */
    if (recomp_free_camera_enabled()) {
        recomp_fiber_set_switch_hook(recomp_scene_freeze_switch);
    }
    recomp_viewer_install();
#endif
#ifndef RECOMP_FULL_PROGRAM
    recomp_imports_reset();
#endif

    if (!recomp_dispatch(parsed.metadata.selectedEntryPoint)) {
        std::cerr << "recomp runner: entry point is not registered\n";
        return 2;
    }

#ifdef RECOMP_FULL_PROGRAM
    std::cout << "entry=" << doaxbv::hex32(parsed.metadata.selectedEntryPoint)
              << " sha256=" << parsed.metadata.fileSha256 << '\n';
#else
    const RecompImportState *imports = recomp_imports_state();
    std::cout << "entry=" << doaxbv::hex32(parsed.metadata.selectedEntryPoint)
              << " sha256=" << parsed.metadata.fileSha256
              << " first_import=" << importName(imports->first_import)
              << " thread_start=" << doaxbv::hex32(imports->thread_start)
              << '\n';

    if (imports->first_import == RECOMP_IMPORT_CREATE_THREAD &&
        imports->thread_start != 0u) {
        std::cerr << "recomp runner: stopped at unimplemented thread start "
                  << doaxbv::hex32(imports->thread_start) << '\n';
        return 2;
    }
#endif

    /* Reaching here means the entry point returned rather than stopping at a
       named boundary. Route it through the same reporter so an expectation of
       "completed" can be asserted and the milestone log stays complete. */
    recomp_stop(0, "completed");
    return 0;
}
