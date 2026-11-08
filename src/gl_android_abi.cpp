#include "gl_android_abi.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {
using Word = std::uint64_t;
using UInt = std::uint32_t;
using Int = std::int32_t;
std::atomic<void *> originals[11] = {};

template <unsigned Slot, class Function>
Function nativeFunction() {
    return reinterpret_cast<Function>(originals[Slot].load(std::memory_order_acquire));
}

// All incoming arguments use eight-byte slots. The typed native call below
// lets Clang repack arguments 9+ according to the Darwin arm64 ABI. Pointer
// arguments retain their complete 64-bit value; GL integers use their low 32 bits.
template <unsigned Slot>
void copyImage(Word a, Word b, Word c, Word d, Word e, Word f, Word g, Word h,
               Word i, Word j, Word k, Word l, Word m, Word n, Word o) {
    using Function = void (*)(UInt, UInt, Int, Int, Int, Int, UInt, UInt,
                              Int, Int, Int, Int, Int, Int, Int);
    nativeFunction<Slot, Function>()(UInt(a), UInt(b), Int(c), Int(d), Int(e), Int(f),
                                     UInt(g), UInt(h), Int(i), Int(j), Int(k), Int(l),
                                     Int(m), Int(n), Int(o));
}

template <unsigned Slot>
void blit(Word a, Word b, Word c, Word d, Word e, Word f, Word g, Word h, Word i, Word j) {
    using Function = void (*)(Int, Int, Int, Int, Int, Int, Int, Int, UInt, UInt);
    nativeFunction<Slot, Function>()(Int(a), Int(b), Int(c), Int(d), Int(e), Int(f),
                                     Int(g), Int(h), UInt(i), UInt(j));
}

template <unsigned Slot>
void subImage3D(Word a, Word b, Word c, Word d, Word e, Word f, Word g, Word h,
                Word i, Word j, Word k) {
    using Function = void (*)(UInt, Int, Int, Int, Int, Int, Int, Int, UInt, UInt, const void *);
    nativeFunction<Slot, Function>()(UInt(a), Int(b), Int(c), Int(d), Int(e), Int(f),
                                     Int(g), Int(h), UInt(i), UInt(j),
                                     reinterpret_cast<const void *>(std::uintptr_t(k)));
}

template <unsigned Slot>
void compressedSubImage3D(Word a, Word b, Word c, Word d, Word e, Word f, Word g, Word h,
                          Word i, Word j, Word k) {
    using Function = void (*)(UInt, Int, Int, Int, Int, Int, Int, Int, UInt, Int, const void *);
    nativeFunction<Slot, Function>()(UInt(a), Int(b), Int(c), Int(d), Int(e), Int(f),
                                     Int(g), Int(h), UInt(i), Int(j),
                                     reinterpret_cast<const void *>(std::uintptr_t(k)));
}

struct Entry { const char *name; void *adapter; };
const Entry entries[] = {
    {"glCopyImageSubData", reinterpret_cast<void *>(copyImage<0>)},
    {"glCopyImageSubDataEXT", reinterpret_cast<void *>(copyImage<1>)},
    {"glCopyImageSubDataOES", reinterpret_cast<void *>(copyImage<2>)},
    {"glBlitFramebuffer", reinterpret_cast<void *>(blit<3>)},
    {"glBlitFramebufferANGLE", reinterpret_cast<void *>(blit<4>)},
    {"glBlitFramebufferEXT", reinterpret_cast<void *>(blit<5>)},
    {"glBlitFramebufferNV", reinterpret_cast<void *>(blit<6>)},
    {"glTexSubImage3D", reinterpret_cast<void *>(subImage3D<7>)},
    {"glTexSubImage3DOES", reinterpret_cast<void *>(subImage3D<8>)},
    {"glCompressedTexSubImage3D", reinterpret_cast<void *>(compressedSubImage3D<9>)},
    {"glCompressedTexSubImage3DOES", reinterpret_cast<void *>(compressedSubImage3D<10>)},
};
static_assert(sizeof(entries) / sizeof(entries[0]) == sizeof(originals) / sizeof(originals[0]));
}
#endif

void *gl_android_abi::wrap(const char *name, void *native) {
#if defined(__APPLE__) && defined(__aarch64__)
    if (!native) return nullptr;
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (std::strcmp(name, entries[i].name) == 0) {
            if (native == entries[i].adapter) return native;
            originals[i].store(native, std::memory_order_release);
            return entries[i].adapter;
        }
    }
#else
    (void)name;
#endif
    return native;
}
