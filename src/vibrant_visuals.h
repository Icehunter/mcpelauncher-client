#pragma once

#include <string>
#include <vector>

namespace vibrant_visuals {
struct Capabilities {
    bool appleArm64 = false;
    bool supportedVersion = false;
    std::string renderer;
    int major = 0;
    int minor = 0;
    int maxComputeInvocations = 0;
    std::vector<std::string> extensions;
};

struct TierResult {
    std::string bytes;
    std::string gpu;
    std::string reason;
    bool changed = false;
};

using Resolver = void* (*)(const char*);
void capture(Resolver resolve, bool supportedVersion);
Capabilities snapshot();
TierResult transform(const std::string& original, const Capabilities& caps);
}
