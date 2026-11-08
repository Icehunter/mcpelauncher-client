#include "vibrant_visuals.h"

#include <algorithm>
#include <base64.h>
#include <cstdio>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>

namespace vibrant_visuals {
namespace {
std::mutex capsMutex;
Capabilities hostCaps;
constexpr size_t maxAssetBytes = 1024 * 1024;

bool hasExtension(const Capabilities& caps, const char* extension) {
    return std::find(caps.extensions.begin(), caps.extensions.end(), extension) != caps.extensions.end();
}

std::string model(const Capabilities& caps) {
    static const std::regex renderer(
        R"(^ANGLE \(Apple, Vulkan [0-9]+\.[0-9]+\.[0-9]+ \((Apple M[1-9][0-9]*(?: (?:Pro|Max|Ultra))?)(?: \(0x[0-9a-fA-F]+\))?\), [^\r\n]+\)$)");
    std::smatch match;
    return std::regex_match(caps.renderer, match, renderer) ? match[1].str() : std::string();
}
}

void capture(Resolver resolve, bool supportedVersion) {
    // Base64 shares its decode table with JNI. Build it before game threads start.
    Base64::decode("");
    Capabilities caps;
#if defined(__APPLE__) && defined(__aarch64__)
    caps.appleArm64 = true;
#endif
    caps.supportedVersion = supportedVersion;
    if(caps.appleArm64 && supportedVersion && resolve) {
        using GetString = const unsigned char* (*)(unsigned int);
        using GetStringi = const unsigned char* (*)(unsigned int, unsigned int);
        using GetIntegerv = void (*)(unsigned int, int*);
        auto getString = reinterpret_cast<GetString>(resolve("glGetString"));
        auto getStringi = reinterpret_cast<GetStringi>(resolve("glGetStringi"));
        auto getInteger = reinterpret_cast<GetIntegerv>(resolve("glGetIntegerv"));
        if(getString && getStringi && getInteger) {
            const auto* renderer = getString(0x1F01); // GL_RENDERER
            const auto* version = getString(0x1F02); // GL_VERSION
            if(renderer) caps.renderer = reinterpret_cast<const char*>(renderer);
            int versionMajor = 0;
            if(!model(caps).empty() && version &&
               std::sscanf(reinterpret_cast<const char*>(version), "OpenGL ES %d", &versionMajor) == 1 && versionMajor >= 3) {
                getInteger(0x821B, &caps.major); // GL_MAJOR_VERSION
                getInteger(0x821C, &caps.minor); // GL_MINOR_VERSION
                if(caps.major > 3 || (caps.major == 3 && caps.minor >= 1)) {
                    getInteger(0x90EB, &caps.maxComputeInvocations); // GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS
                    int count = 0;
                    getInteger(0x821D, &count); // GL_NUM_EXTENSIONS
                    if(count >= 0 && count <= 16384) {
                        for(int i = 0; i < count; ++i) {
                            const auto* extension = getStringi(0x1F03, static_cast<unsigned int>(i));
                            if(extension) caps.extensions.emplace_back(reinterpret_cast<const char*>(extension));
                        }
                    }
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(capsMutex);
    hostCaps = std::move(caps);
}

Capabilities snapshot() {
    std::lock_guard<std::mutex> lock(capsMutex);
    return hostCaps;
}

TierResult transform(const std::string& original, const Capabilities& caps) {
    TierResult result{original, {}, "unsupported host", false};
    if(!caps.appleArm64 || !caps.supportedVersion) return result;
    result.gpu = model(caps);
    if(result.gpu.empty() || caps.major < 3 || (caps.major == 3 && caps.minor < 1) ||
       caps.maxComputeInvocations < 256) return result;
    if(!hasExtension(caps, "GL_EXT_shader_framebuffer_fetch") &&
       !hasExtension(caps, "GL_EXT_shader_framebuffer_fetch_non_coherent")) return result;
    if(!(caps.major > 3 || (caps.major == 3 && caps.minor >= 2)) &&
       !hasExtension(caps, "GL_EXT_texture_cube_map_array") &&
       !hasExtension(caps, "GL_ARB_texture_cube_map_array")) return result;
    result.reason = "invalid tier asset";
    if(original.empty() || original.size() > maxAssetBytes) return result;
    try {
        std::string compact;
        compact.reserve(original.size());
        for(unsigned char c : original) {
            if(c == '\r' || c == '\n') continue;
            if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) return result;
            compact += static_cast<char>(c);
        }
        const auto decoded = Base64::decode(compact);
        if(Base64::encode(decoded) != compact) return result;
        auto data = nlohmann::json::parse(decoded, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if(depth > 32) throw std::runtime_error("Tier asset depth exceeds limit");
            return true;
        });
        if(!data.is_object() || !data.contains("gpu") || !data["gpu"].is_object()) return result;
        auto& gpu = data["gpu"];
        int tier = 4;
        for(auto it = gpu.begin(); it != gpu.end(); ++it) {
            if(it.key().empty() || !it.value().is_number_integer() || it.value() < 1 || it.value() > 5) return result;
            if(caps.renderer.find(it.key()) != std::string::npos)
                tier = std::max(tier, it.value().get<int>());
        }
        if(gpu.contains(result.gpu) && gpu[result.gpu].get<int>() >= tier) {
            result.reason = "existing tier retained";
            return result;
        }
        gpu[result.gpu] = tier;
        result.bytes = Base64::encode(data.dump());
        result.changed = true;
        result.reason = "Apple GPU tier applied";
    } catch(const std::exception&) {
        return result;
    }
    return result;
}
}
