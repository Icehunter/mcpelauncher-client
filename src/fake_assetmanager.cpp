#include <cstdio>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <FileUtil.h>
#include <sys/types.h>
#include <dirent.h>
#include <log.h>
#include <libc_shim.h>
#include <android/compat.h>
#include <jni.h>
#include <cstdlib>
#include <algorithm>
#include <regex>
#include <vector>
#include <string>
#include "fake_assetmanager.h"
#include <mcpelauncher/path_helper.h>

namespace {

std::mutex g_worldMutex;
std::string g_lastWorldPacks;
std::string g_lastLoggedWorld;

void observePath(const char *path) {
    if(!path)
        return;
    const std::string p = path;
    if(p.find("world_resource_packs") != std::string::npos &&
       p.size() >= 4 && p.compare(p.size() - 4, 4, ".bak") != 0 &&
       strstr(path, "premium_cache") == nullptr &&
       strstr(path, "world_templates") == nullptr) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(g_worldMutex);
            changed = g_lastWorldPacks != p;
            g_lastWorldPacks = p;
        }
        if(changed)
            Log::info("PathObserver", "WORLDPACKS %s", path);
    }
}

std::string ensureTrailingSlash(std::string p) {
    if(!p.empty() && p.back() != '/')
        p += '/';
    return p;
}

bool firstUuidMatches(const std::string &manifest, const std::string &packId) {
    static const std::regex re("\"uuid\"\\s*:\\s*\"([^\"]+)\"", std::regex::icase);
    auto begin = std::sregex_iterator(manifest.begin(), manifest.end(), re);
    if(begin == std::sregex_iterator())
        return false;
    return begin->str(1) == packId;
}

std::vector<std::string> packIdsFromJsonFile(const std::string &file) {
    static const std::regex re("\"pack_id\"\\s*:\\s*\"([^\"]+)\"");
    std::vector<std::string> result;
    std::string json;
    if(!FileUtil::readFile(file, json))
        return result;
    for(std::sregex_iterator it(json.begin(), json.end(), re), end; it != end; ++it)
        result.push_back(it->str(1));
    return result;
}

std::vector<std::string> resolvePackDirs(const std::vector<std::string> &packIds) {
    std::string dataDir = ensureTrailingSlash(PathHelper::getPrimaryDataDirectory());
    std::string gameData = dataDir + "games/com.mojang/";
    const std::string bases[] = {gameData + "resource_packs/", gameData + "development_resource_packs/",
                                 dataDir + "minecraftpe/packcache/resource/"};

    std::vector<std::pair<std::string, std::string>> found;
    for(const auto &base : bases) {
        DIR *d = opendir(base.c_str());
        if(!d)
            continue;
        while(auto *ent = readdir(d)) {
            if(ent->d_name[0] == '.')
                continue;
            std::string dir = ensureTrailingSlash(base + ent->d_name);
            std::string manifest;
            if(FileUtil::readFile(dir + "manifest.json", manifest))
                found.emplace_back(dir, std::move(manifest));
        }
        closedir(d);
    }

    std::vector<std::string> result;
    for(const auto &packId : packIds) {
        for(const auto &f : found) {
            if(firstUuidMatches(f.second, packId)) {
                if(std::find(result.begin(), result.end(), f.first) == result.end())
                    result.push_back(f.first);
                break;
            }
        }
    }
    return result;
}

bool materialOverride(const std::string &filename, std::string &content) {
    const std::string prefix = "assets/renderer/materials/";
    const std::string suffix = ".material.bin";
    if(filename.compare(0, prefix.size(), prefix) != 0)
        return false;
    if(filename.size() <= prefix.size() + suffix.size())
        return false;
    if(filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    std::string name = filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
    if(name.find('/') != std::string::npos)
        return false;

    auto tryRead = [&](const std::string &path) -> bool {
        return FileUtil::readFile(path, content);
    };

    const char *bedrockDir = std::getenv("BEDROCK_MATERIAL_DIR");
    if(bedrockDir != nullptr && bedrockDir[0] != '\0') {
        std::string candidate = ensureTrailingSlash(bedrockDir) + name + suffix;
        if(tryRead(candidate)) {
            Log::info("AAssetManager", "OVERRIDE '%s' from '%s'", filename.c_str(), candidate.c_str());
            return true;
        }
    }

    std::vector<std::string> packDirs;

    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        std::vector<std::string> worldPackIds = packIdsFromJsonFile(g_lastWorldPacks);
        if(!worldPackIds.empty())
        {
            auto worldDirs = resolvePackDirs(worldPackIds);
            packDirs.insert(packDirs.end(), worldDirs.begin(), worldDirs.end());
        }
    }

    static const std::vector<std::string> globalPackDirs = []() {
        auto dirs = resolvePackDirs(packIdsFromJsonFile(ensureTrailingSlash(PathHelper::getPrimaryDataDirectory()) + "games/com.mojang/minecraftpe/global_resource_packs.json"));
        Log::info("AAssetManager", "material override: %zu active pack(s)", dirs.size());
        return dirs;
    }();

    packDirs.insert(packDirs.end(), globalPackDirs.begin(), globalPackDirs.end());

    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        if(g_lastLoggedWorld != g_lastWorldPacks) {
            g_lastLoggedWorld = g_lastWorldPacks;
            Log::info("AAssetManager", "material override: world list '%s' -> %zu pack(s)", g_lastWorldPacks.c_str(), packDirs.size());
        }
    }

    std::vector<std::string> candidates;
    for(const auto &dir : packDirs) {
        candidates.push_back(dir + "renderer/materials/" + name + suffix);
        candidates.push_back(dir + "renderer/" + name + suffix);
    }

    for(const auto &candidate : candidates) {
        if(tryRead(candidate)) {
            Log::info("AAssetManager", "OVERRIDE '%s' from '%s'", filename.c_str(), candidate.c_str());
            return true;
        }
    }

    return false;
}

}  // namespace

struct AAsset {
    std::string buffer;
    off64_t offset = 0;
};
struct AAssetDir {
    DIR *dir;
    dirent *ent;
    std::string dirname;
    std::string currentFileName;
};

static bool shouldLogAsset(const std::string &path) {
    static const std::string suffix = ".material.bin";
    return path.find("renderer/") != std::string::npos ||
           (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0);
}

FakeAssetManager::FakeAssetManager(std::string rootDir) {
    if(!rootDir.empty() && *rootDir.rbegin() != '/')
        rootDir += '/';
    this->rootDir = std::move(rootDir);
}

namespace fake_assetmanager {

AAssetManager *AAssetManager_fromJava(JNIEnv *env, jobject javaAssetManager) {
    return (AAssetManager *)javaAssetManager;
}

AAsset *AAssetManager_open(FakeAssetManager *amgr, const char *filename, int mode) {
    std::string fullPath;
    if(filename == NULL) {
#ifndef NDEBUG
        Log::trace("AAssetManager", "Opening file NULLPTR failed.\n");
#endif
        return nullptr;
    }

    if(filename[0] != '/') {
        fullPath = amgr->rootDir + filename;
    } else {
        // Ignore full paths, the game tries to open user data files with the AAssetManager
        return nullptr;
    }

#ifndef NDEBUG
    Log::trace("AAssetManager", "Opening file '%s' as '%s'\n", filename, fullPath.c_str());
#endif

    bool logThis = shouldLogAsset(fullPath);

    std::string content;
    if(!materialOverride(filename, content) && !FileUtil::readFile(fullPath, content)) {
        if(logThis)
            Log::info("AAssetManager", "OPEN miss '%s' as '%s'", filename, fullPath.c_str());
        return nullptr;
    }

    if(logThis)
        Log::info("AAssetManager", "OPEN ok '%s' as '%s'", filename, fullPath.c_str());

    auto ret = new AAsset;
    ret->buffer = content;
    return ret;
}

AAssetDir *AAssetManager_openDir(FakeAssetManager *amgr, const char *dirname) {
    if(dirname == NULL) {
#ifndef NDEBUG
        Log::trace("AAssetManager", "Opening directory NULLPTR failed.\n");
#endif
        return nullptr;
    }

    std::string fullPath;
    if(dirname[0] != '/') {
        fullPath = amgr->rootDir + dirname;
    } else {
        // Ignore full paths, the game tries to open user data files with the AAssetManager
        return nullptr;
    }

#ifndef NDEBUG
    Log::trace("AAssetManager", "Opening directory '%s' as '%s'\n", dirname, fullPath.c_str());
#endif

    bool logThis = shouldLogAsset(fullPath);

    DIR *d = opendir(fullPath.c_str());
    if(!d) {
        if(logThis)
            Log::info("AAssetManager", "OPEN miss '%s' as '%s'", dirname, fullPath.c_str());
        return nullptr;
    }

    if(logThis)
        Log::info("AAssetManager", "OPEN ok '%s' as '%s'", dirname, fullPath.c_str());

    auto ret = new AAssetDir;
    ret->dir = d;
    ret->ent = nullptr;
    ret->dirname = dirname;
    return ret;
}

void AAsset_close(AAsset *asset) {
    delete asset;
}

int AAsset_isAllocated(AAsset *asset) {
    return true;
}

ssize_t AAsset_read(AAsset *asset, void *buf, size_t count) {
    if(asset->offset > asset->buffer.size()) {
        return 0;
    }
    size_t max_len = asset->buffer.size() - asset->offset;
    if(count > max_len) {
        count = max_len;
    }
    if(count == 0) {
        return 0;
    }
    memcpy(buf, &asset->buffer[asset->offset], count);
    asset->offset += count;
    return (ssize_t)count;
}

off64_t AAsset_seek64(AAsset *asset, off64_t offset, int whence) {
    off64_t cur_pos = asset->offset;
    off64_t max_pos = asset->buffer.size();
    off64_t new_offset;

    if(whence == SEEK_SET) {
        new_offset = offset;
    } else if(whence == SEEK_CUR) {
        new_offset = cur_pos + offset;
    } else if(whence == SEEK_END) {
        new_offset = max_pos + offset;
    }
    if(new_offset < 0 || new_offset > max_pos)
        return -1;
    asset->offset = new_offset;
    return new_offset;
}

off_t AAsset_seek(AAsset *asset, off_t offset, int whence) {
    return (off_t)AAsset_seek64(asset, offset, whence);
}

off64_t AAsset_getLength64(AAsset *asset) {
    return (off64_t)asset->buffer.size();
}

off_t AAsset_getLength(AAsset *asset) {
    return (off_t)asset->buffer.size();
}

off64_t AAsset_getRemainingLength64(AAsset *asset) {
    return (off64_t)(asset->buffer.size() - asset->offset);
}

off_t AAsset_getRemainingLength(AAsset *asset) {
    return (off_t)(asset->buffer.size() - asset->offset);
}

const void *AAsset_getBuffer(AAsset *asset) {
    return asset->buffer.c_str();
}

void AAssetDir_close(AAssetDir *assetDir) {
    if(assetDir)
        closedir(assetDir->dir);
    delete assetDir;
}

void AAssetDir_rewind(AAssetDir *assetDir) {
    rewinddir(assetDir->dir);
}

const char *AAssetDir_getNextFileName(AAssetDir *assetDir) {
    if(!assetDir)
        return nullptr;
    assetDir->ent = readdir(assetDir->dir);
    if(!assetDir->ent)
        return nullptr;
    std::string cname = assetDir->ent->d_name;
    if(cname == "." || cname == "..") {
        return AAssetDir_getNextFileName(assetDir);
    }
    assetDir->currentFileName = cname;
#ifndef NDEBUG
    Log::trace("AAssetDir", "'%s' getNextFileName '%s'\n", assetDir->dirname.data(), assetDir->currentFileName.data());
#endif
    return assetDir->currentFileName.data();
}

}  // namespace fake_assetmanager

void FakeAssetManager::initHybrisHooks(std::unordered_map<std::string, void *> &syms) {
    shim::path_observer = observePath;
    using namespace fake_assetmanager;
    syms["AAssetManager_fromJava"] = (void *)AAssetManager_fromJava;
    syms["AAssetManager_open"] = (void *)AAssetManager_open;
    syms["AAssetManager_openDir"] = (void *)AAssetManager_openDir;
    syms["AAsset_close"] = (void *)AAsset_close;
    syms["AAsset_isAllocated"] = (void *)AAsset_isAllocated;
    syms["AAsset_read"] = (void *)AAsset_read;
    syms["AAsset_seek64"] = (void *)AAsset_seek64;
    syms["AAsset_seek"] = (void *)AAsset_seek;
    syms["AAsset_getLength64"] = (void *)AAsset_getLength64;
    syms["AAsset_getLength"] = (void *)AAsset_getLength;
    syms["AAsset_getRemainingLength64"] = (void *)AAsset_getRemainingLength64;
    syms["AAsset_getRemainingLength"] = (void *)AAsset_getRemainingLength;
    syms["AAsset_getBuffer"] = (void *)AAsset_getBuffer;
    syms["AAssetDir_close"] = (void *)AAssetDir_close;
    syms["AAssetDir_rewind"] = (void *)AAssetDir_rewind;
    syms["AAssetDir_getNextFileName"] = (void *)AAssetDir_getNextFileName;
}
