// Offline ABI test: no EGL context, graphics driver or game is needed.
#include "../src/gl_android_abi.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__) && defined(__aarch64__)
using Word = std::uint64_t;
using UInt = std::uint32_t;
using Int = std::int32_t;
extern "C" void androidCall(void *function, const Word *arguments);
// Android AAPCS64: stack integer arguments each occupy an eight-byte slot.
// Bound the frame to seven stack arguments plus saved FP/LR, aligned to 16 bytes.
asm(".text\n.p2align 2\n.globl _androidCall\n_androidCall:\n"
    "sub sp, sp, #80\n stp x29, x30, [sp, #64]\n mov x29, sp\n"
    "mov x9, x0\n mov x10, x1\n"
    "ldp x11, x12, [x10, #64]\n stp x11, x12, [sp]\n"
    "ldp x11, x12, [x10, #80]\n stp x11, x12, [sp, #16]\n"
    "ldp x11, x12, [x10, #96]\n stp x11, x12, [sp, #32]\n"
    "ldr x11, [x10, #112]\n str x11, [sp, #48]\n"
    "ldp x0, x1, [x10]\n ldp x2, x3, [x10, #16]\n"
    "ldp x4, x5, [x10, #32]\n ldp x6, x7, [x10, #48]\n"
    "blr x9\n ldp x29, x30, [sp, #64]\n add sp, sp, #80\n ret\n");

static Word observed[15];
static unsigned calls;
static void nativeCopy(UInt a, UInt b, Int c, Int d, Int e, Int f, UInt g, UInt h,
                        Int i, Int j, Int k, Int l, Int m, Int n, Int o) {
    const Word values[] = {a,b,UInt(c),UInt(d),UInt(e),UInt(f),g,h,UInt(i),UInt(j),UInt(k),UInt(l),UInt(m),UInt(n),UInt(o)};
    std::memcpy(observed, values, sizeof(values));
    ++calls;
}
static void nativeBlit(Int a, Int b, Int c, Int d, Int e, Int f, Int g, Int h, UInt i, UInt j) {
    const Word values[] = {UInt(a),UInt(b),UInt(c),UInt(d),UInt(e),UInt(f),UInt(g),UInt(h),i,j};
    std::memcpy(observed, values, sizeof(values));
    ++calls;
}
static void nativeSubImage(UInt a, Int b, Int c, Int d, Int e, Int f, Int g, Int h,
                            UInt i, UInt j, const void *k) {
    const Word values[] = {a,UInt(b),UInt(c),UInt(d),UInt(e),UInt(f),UInt(g),UInt(h),i,j,reinterpret_cast<std::uintptr_t>(k)};
    std::memcpy(observed, values, sizeof(values));
    ++calls;
}
static void nativeCompressedSubImage(UInt a, Int b, Int c, Int d, Int e, Int f, Int g, Int h,
                                      UInt i, Int j, const void *k) {
    nativeSubImage(a,b,c,d,e,f,g,h,i,UInt(j),k);
}

int main(int argc, char **argv) {
    const bool raw = argc > 1 && std::strcmp(argv[1], "--raw") == 0;
    struct Case { const char *name; void *native; unsigned count; bool pointer; };
    const Case cases[] = {
        {"glCopyImageSubData", reinterpret_cast<void *>(nativeCopy), 15, false},
        {"glCopyImageSubDataEXT", reinterpret_cast<void *>(nativeCopy), 15, false},
        {"glCopyImageSubDataOES", reinterpret_cast<void *>(nativeCopy), 15, false},
        {"glBlitFramebuffer", reinterpret_cast<void *>(nativeBlit), 10, false},
        {"glBlitFramebufferANGLE", reinterpret_cast<void *>(nativeBlit), 10, false},
        {"glBlitFramebufferEXT", reinterpret_cast<void *>(nativeBlit), 10, false},
        {"glBlitFramebufferNV", reinterpret_cast<void *>(nativeBlit), 10, false},
        {"glTexSubImage3D", reinterpret_cast<void *>(nativeSubImage), 11, true},
        {"glTexSubImage3DOES", reinterpret_cast<void *>(nativeSubImage), 11, true},
        {"glCompressedTexSubImage3D", reinterpret_cast<void *>(nativeCompressedSubImage), 11, true},
        {"glCompressedTexSubImage3DOES", reinterpret_cast<void *>(nativeCompressedSubImage), 11, true},
    };
    // Native callers use the Darwin signature.
    nativeCopy(1,2,-3,4,5,6,7,8,9,10,11,12,13,14,15);
    if (observed[2] != UInt(-3) || observed[14] != 15) return 2;
    if (gl_android_abi::wrap("unknown", cases[0].native) != cases[0].native ||
        gl_android_abi::wrap("glCopyImageSubDataEXT", nullptr) != nullptr) return 3;
    unsigned failures = 0;
    for (const auto &test : cases) {
        Word args[15];
        for (unsigned i = 0; i < 15; ++i)
            args[i] = 0xface123400000000ULL | (i == 2 ? UInt(-37) : UInt(0x101 + i*17));
        if (test.pointer) args[10] = 0x12345678abcdef01ULL; // capture only: never dereferenced
        std::memset(observed, 0, sizeof(observed));
        unsigned before = calls;
        void *function = raw ? test.native : gl_android_abi::wrap(test.name, test.native);
        if (!raw && gl_android_abi::wrap(test.name, function) != function) return 4;
        androidCall(function, args);
        bool ok = calls == before + 1;
        for (unsigned i = 0; i < test.count; ++i) {
            Word expected = test.pointer && i == 10 ? args[i] : UInt(args[i]);
            if (observed[i] != expected) ok = false;
        }
        std::printf("%s %s%s\n", ok ? "PASS" : "FAIL", test.name, raw ? " (raw ABI mismatch)" : "");
        failures += !ok;
    }
    std::printf("%zu families/aliases checked; %u failures\n", sizeof(cases)/sizeof(cases[0]), failures);
    return failures ? 1 : 0;
}
#else
int main() { std::puts("SKIP: ABI bridge applies only to Apple arm64"); }
#endif
