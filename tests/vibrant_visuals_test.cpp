#include "../src/vibrant_visuals.h"
#include <base64.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <iostream>

using nlohmann::json;
using namespace vibrant_visuals;
static int checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if(!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
}
static Capabilities supported() {
    Capabilities c;
    c.appleArm64 = c.supportedVersion = true;
    c.renderer = "ANGLE (Apple, Vulkan 1.1.357 (Apple M5 Pro (0x1B02020A)), MoltenVK-0.2.2211)";
    c.major = 3; c.minor = 1; c.maxComputeInvocations = 1024;
    c.extensions = {"GL_EXT_shader_framebuffer_fetch", "GL_EXT_texture_cube_map_array"};
    return c;
}
static std::string encode(const json& j) { return Base64::encode(j.dump()); }
static int integerQueries = 0;
static int indexedQueries = 0;
static std::string mockVersion = "OpenGL ES 3.1 ANGLE";
static std::string mockRenderer = supported().renderer;
static int mockExtensionCount = 2;
static const unsigned char* mockGetString(unsigned int name) {
    const auto& text = name == 0x1F01 ? mockRenderer : mockVersion;
    return reinterpret_cast<const unsigned char*>(text.c_str());
}
static const unsigned char* mockGetStringi(unsigned int, unsigned int index) {
    ++indexedQueries;
    return reinterpret_cast<const unsigned char*>(index == 0 ? "GL_EXT_shader_framebuffer_fetch" : "GL_EXT_texture_cube_map_array");
}
static void mockGetInteger(unsigned int name, int* value) {
    ++integerQueries;
    if(name == 0x821B) *value = 3;
    else if(name == 0x821C) *value = 1;
    else if(name == 0x90EB) *value = 1024;
    else if(name == 0x821D) *value = mockExtensionCount;
}
static void* mockResolve(const char* name) {
    if(std::strcmp(name, "glGetString") == 0) return reinterpret_cast<void*>(mockGetString);
    if(std::strcmp(name, "glGetStringi") == 0) return reinterpret_cast<void*>(mockGetStringi);
    if(std::strcmp(name, "glGetIntegerv") == 0) return reinterpret_cast<void*>(mockGetInteger);
    return nullptr;
}
int main() {
    const json table = {{"gpu", {{"Adreno (TM) 104", 1}, {"Unrelated", 5}}}, {"other", {1,2,3}}};
    const std::string original = encode(table);
    auto c = supported();
    auto r = transform(original, c);
    check(r.changed && r.gpu == "Apple M5 Pro", "supported host gets a stable model rule");
    auto output = json::parse(Base64::decode(r.bytes));
    auto expected = table; expected["gpu"]["Apple M5 Pro"] = 4;
    check(output == expected && original == encode(table), "only model rule changes and input stays intact");
    auto same = table; same["gpu"]["Apple M5 Pro"] = 5;
    const auto higher = encode(same);
    check(transform(higher, c).bytes == higher, "higher tier stays byte exact");
    same["gpu"]["Apple M5 Pro"] = 4;
    const auto equal = encode(same);
    check(transform(equal, c).bytes == equal, "tier four stays byte exact");
    same["gpu"]["Apple M5 Pro"] = 2;
    check(json::parse(Base64::decode(transform(encode(same), c).bytes))["gpu"]["Apple M5 Pro"] == 4, "lower tier raised");
    same = table; same["gpu"]["Apple M"] = 5;
    check(json::parse(Base64::decode(transform(encode(same), c).bytes))["gpu"]["Apple M5 Pro"] == 5, "broader matching higher tier retained");
    auto reject = [&](Capabilities bad, const char* what) { check(transform(original, bad).bytes == original, what); };
    auto bad = c; bad.appleArm64 = false; reject(bad, "other platform unchanged");
    bad = c; bad.supportedVersion = false; reject(bad, "old game unchanged");
    bad = c; bad.major = 3; bad.minor = 0; reject(bad, "ES 3.0 unchanged");
    bad = c; bad.maxComputeInvocations = 255; reject(bad, "low compute limit unchanged");
    bad = c; bad.extensions = {"GL_EXT_texture_cube_map_array"}; reject(bad, "missing fetch unchanged");
    bad = c; bad.extensions = {"GL_EXT_shader_framebuffer_fetch"}; reject(bad, "missing cube array unchanged");
    bad = c; bad.extensions = {"GL_EXT_shader_framebuffer_fetch", "GL_OES_texture_cube_map_array"};
    reject(bad, "OES-only cube arrays do not pass game gate");
    bad = c; bad.extensions = {"GL_EXT_shader_framebuffer_fetch", "GL_ARB_texture_cube_map_array"};
    check(transform(original, bad).changed, "ARB cube arrays pass game gate");
    bad = c; bad.extensions[0] += "_invalid"; reject(bad, "extension token must match");
    for(const char* name : {"Apple M1", "Apple M2 Pro", "Apple M3 Max", "Apple M4 Ultra", "Apple M6", "Apple M12 Pro"}) {
        bad = c; bad.renderer = std::string("ANGLE (Apple, Vulkan 1.3.0 (") + name + " (0x1234)), MoltenVK-1.0)";
        check(transform(original, bad).changed, "numbered Apple M model accepted");
    }
    for(const char* name : {"Apple M0", "Apple M01", "Apple M5Pro", "Apple M5 Pro Max", "Apple M5 Prototyp", "Apple M5 Ultra X", "Apple A18", "Apple M", "Apple M-1"}) {
        bad = c; bad.renderer = std::string("ANGLE (Apple, Vulkan 1.3.0 (") + name + " (0x1234)), MoltenVK-1.0)";
        reject(bad, "unknown model unchanged");
    }
    for(const char* renderer : {"Apple M5 Pro", "ANGLE (Apple, ANGLE Metal Renderer: Apple M5 Pro, Unspecified Version)", "ANGLE (NVIDIA, Vulkan 1.3.0 (Apple M5 Pro (0x1234)), Driver)"}) {
        bad = c; bad.renderer = renderer; reject(bad, "other backend unchanged");
    }
    bad = c; bad.minor = 2; bad.extensions = {"GL_EXT_shader_framebuffer_fetch_non_coherent"};
    check(transform(original, bad).changed, "ES 3.2 cube arrays and noncoherent fetch accepted");
    for(const std::string& bytes : {std::string("!invalid!"), std::string(1, static_cast<char>(0xff)), std::string("e30=garbage"), std::string("e31="), encode(json::array()), encode(json{{"gpu", json::array()}}), encode(json{{"other", 3}}), Base64::encode("{bad json"), std::string(1024*1024+1, 'A')}) {
        check(transform(bytes, c).bytes == bytes, "invalid asset stays byte exact");
    }
    same = table; same["gpu"]["Apple M5 Pro"] = "4";
    const auto invalidTier = encode(same);
    check(transform(invalidTier, c).bytes == invalidTier, "unknown tier schema unchanged");
    capture(nullptr, true);
    check(snapshot().renderer.empty() && snapshot().extensions.empty(), "missing resolver gives empty capabilities");
    capture(mockResolve, false);
    check(integerQueries == 0 && indexedQueries == 0, "unsupported version makes no GL queries");
    mockVersion = "OpenGL ES 2.0 ANGLE";
    capture(mockResolve, true);
    check(integerQueries == 0 && indexedQueries == 0, "ES2 makes no unsupported enum queries");
    mockVersion = "OpenGL ES 3.1 ANGLE";
    mockRenderer = "ANGLE (Other, Vulkan 1.3.0 (Unknown), Other)";
    capture(mockResolve, true);
    check(integerQueries == 0 && indexedQueries == 0, "unsupported renderer makes no integer queries");
#if defined(__APPLE__) && defined(__aarch64__)
    mockRenderer = supported().renderer;
    capture(mockResolve, true);
    auto captured = snapshot();
    check(captured.major == 3 && captured.minor == 1 && captured.maxComputeInvocations == 1024 && captured.extensions.size() == 2,
          "native query snapshot contains host capabilities");
    check(transform(original, captured).changed, "captured host passes policy");
    captured.renderer.clear();
    check(!snapshot().renderer.empty(), "snapshot is an owned copy");
    mockExtensionCount = 16385;
    const auto previousIndexedQueries = indexedQueries;
    capture(mockResolve, true);
    check(snapshot().extensions.empty() && indexedQueries == previousIndexedQueries, "extension count bound avoids reads");
    check(!transform(original, snapshot()).changed, "invalid extension count fails closed");
#endif
    std::cout << "PASS " << checks << " checks\n";
}
