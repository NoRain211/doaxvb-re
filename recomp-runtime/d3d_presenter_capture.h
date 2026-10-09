#ifndef DOAXBV_RECOMP_D3D_PRESENTER_CAPTURE_H
#define DOAXBV_RECOMP_D3D_PRESENTER_CAPTURE_H

#include "d3d_presenter.h"

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

class D3dCapturePacket {
public:
    enum Kind : uint32_t { COMMAND, RELEASE, REPORT };
    struct Record { Kind kind; uint32_t base; uint32_t size; };

    D3dCapturePacket() = default;
    D3dCapturePacket(const D3dCapturePacket &) = delete;
    D3dCapturePacket &operator=(const D3dCapturePacket &) = delete;
    D3dCapturePacket(D3dCapturePacket &&) noexcept = default;
    D3dCapturePacket &operator=(D3dCapturePacket &&) noexcept = default;

    RecompD3dPresenterError add(const RecompD3dPresenterCommand &command);
    RecompD3dPresenterError addRelease(uint32_t base, uint32_t size);
    RecompD3dPresenterError addReport();
    void seal(bool own_textures = false);
    size_t count() const;
    const Record &record(size_t index) const;
    const RecompD3dPresenterCommand &command(size_t index) const;
    void clear();
    // Copied commands, records and aligned payload bytes, excluding capacity.
    uint64_t bytes() const;
    double published_ms = 0;
    bool hasPoseReplay() const { return pose_replay_; }

private:
    friend struct D3dCapturePacketTest;
    // Default-initializes instead of zeroing; Debug builds rebind to proxy types.
    template <typename T>
    struct NoInitAllocator : std::allocator<T> {
        using std::allocator<T>::allocator;
        template <typename U>
        struct rebind { using other = NoInitAllocator<U>; };

        template <typename U>
        void construct(U *pointer)
        {
            ::new (static_cast<void *>(pointer)) U;
        }

        template <typename U, typename... Args>
        void construct(U *pointer, Args &&...args)
        {
            ::new (static_cast<void *>(pointer)) U(std::forward<Args>(args)...);
        }
    };

    struct Entry { Record record; size_t command_index; };
    struct CapturedCommand {
        CapturedCommand() {} // Skip zeroing: add() writes every field it uses.
        alignas(8) RecompD3dPresenterCommand value;
        size_t offsets[10];
    };
    struct Span { size_t size; size_t offset; };
    // Open-addressed span index reused by every packet that occupies this slot.
    // A slot is live only in the current generation, so clear() and add()
    // allocate nothing once the table has grown to a frame's span count.
    struct SpanSlot { const void *source; Span span; uint32_t generation; };

    RecompD3dPresenterError addRecord(Kind kind, uint32_t base, uint32_t size);
    size_t findSpan(const void *source, size_t size) const;
    void insertSpan(const void *source, Span span);
    size_t copySpan(const void *source, size_t size);

    std::vector<Entry> entries_;
    std::vector<CapturedCommand> commands_;
    // Word storage keeps indices and vertex data aligned after relocation.
    // resize() appends uninitialized words; add() overwrites the used bytes.
    std::vector<uint64_t, NoInitAllocator<uint64_t>> payload_;
    std::vector<SpanSlot> span_slots_;
    size_t span_count_ = 0;
    uint32_t span_generation_ = 1;
    bool sealed_ = false;
    bool pose_replay_ = false;
};

#endif
