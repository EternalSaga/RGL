module;
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include "GFSDK_Aftermath.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include "GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <fstream>
#include <map>
export module aftermath;

import rlstd;
import rllogger;

namespace RGL {

struct ShaderDebugInfoIdCompare {
    bool operator()(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& a,
                    const GFSDK_Aftermath_ShaderDebugInfoIdentifier& b) const {
        return (a.id[0] < b.id[0]) || (a.id[0] == b.id[0] && a.id[1] < b.id[1]);
    }
};

static std::map<GFSDK_Aftermath_ShaderDebugInfoIdentifier, std::vector<uint8_t>, ShaderDebugInfoIdCompare> s_shaderDebugInfoCache;
static std::mutex s_shaderDebugInfoMutex;

static constexpr uint32_t MAX_DUMP_SIZE = 64 * 1024 * 1024;

struct AftermathState {
    bool initialized = false;
    bool crashDumpPending = false;
    std::vector<uint8_t> crashDumpData;
    std::mutex dumpMutex;
    std::condition_variable dumpCv;
};

static inline AftermathState& getAftermathState() {
    static AftermathState state;
    return state;
}

static void GFSDK_AFTERMATH_CALL gpuCrashDumpCallback(
    const void* pGpuCrashDump,
    const uint32_t gpuCrashDumpSize,
    void* pUserData)
{
    (void)pUserData;
    auto& state = getAftermathState();
    auto logger = RLLogger::getInstance();

    {
        std::lock_guard<std::mutex> lock(state.dumpMutex);
        state.crashDumpData.assign(
            static_cast<const uint8_t*>(pGpuCrashDump),
            static_cast<const uint8_t*>(pGpuCrashDump) + gpuCrashDumpSize);
        state.crashDumpPending = true;
    }

    if (logger) {
        logger->error("Aftermath: GPU crash dump received ({} bytes)", gpuCrashDumpSize);
    }

    state.dumpCv.notify_all();
}

static void GFSDK_AFTERMATH_CALL descriptionCallback(
    PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription,
    void* pUserData)
{
    (void)pUserData;
    addDescription(
        GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName,
        "RGL Vulkan Practice");
    addDescription(
        GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion,
        "1.0");
    addDescription(
        GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined,
        "Mesh Shader + Shader Object + Descriptor Heap diagnostic test");
    addDescription(
        GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined + 1,
        "Testing NVIDIA RTX 5090 mesh shader + shader object Xid 13 issue");
}

static void decodeCrashDump();

static void GFSDK_AFTERMATH_CALL shaderDebugInfoCallback(
    const void* pShaderDebugInfo,
    const uint32_t shaderDebugInfoSize,
    void* pUserData)
{
    (void)pUserData;
    auto logger = RLLogger::getInstance();

    GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier{};
    auto result = GFSDK_Aftermath_GetShaderDebugInfoIdentifier(
        GFSDK_Aftermath_Version_API, pShaderDebugInfo, shaderDebugInfoSize, &identifier);

    if (!GFSDK_Aftermath_SUCCEED(result)) {
        if (logger) logger->warn("Aftermath: failed to get shader debug info identifier");
        return;
    }

    std::vector<uint8_t> data(static_cast<const uint8_t*>(pShaderDebugInfo),
                              static_cast<const uint8_t*>(pShaderDebugInfo) + shaderDebugInfoSize);

    {
        std::lock_guard<std::mutex> lock(s_shaderDebugInfoMutex);
        s_shaderDebugInfoCache[identifier] = std::move(data);
    }

    if (logger) {
        logger->info("Aftermath: shader debug info cached (uid=0x{:x}_{:x}, {} bytes)",
                     identifier.id[0], identifier.id[1], shaderDebugInfoSize);
    }
}

static void GFSDK_AFTERMATH_CALL shaderDebugInfoLookupCallback(
    const GFSDK_Aftermath_ShaderDebugInfoIdentifier* pIdentifier,
    PFN_GFSDK_Aftermath_SetData setShaderDebugInfo,
    void* pUserData)
{
    (void)pUserData;
    std::lock_guard<std::mutex> lock(s_shaderDebugInfoMutex);
    auto it = s_shaderDebugInfoCache.find(*pIdentifier);
    if (it != s_shaderDebugInfoCache.end()) {
        setShaderDebugInfo(it->second.data(), static_cast<uint32_t>(it->second.size()));
    }
}

export bool initAftermath() {
    auto logger = RLLogger::getInstance();
    if (!logger) return false;

    GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
        gpuCrashDumpCallback,
        shaderDebugInfoCallback,
        descriptionCallback,
        nullptr,
        nullptr
    );

    if (result == GFSDK_Aftermath_Result_Success) {
        getAftermathState().initialized = true;
        logger->info("Aftermath: GPU crash dump enabled (SDK v2.27)");
        return true;
    }

    switch (result) {
        case GFSDK_Aftermath_Result_FAIL_InvalidAdapter:
            logger->warn("Aftermath: not available - non-NVIDIA GPU or no NVIDIA GPU detected");
            break;
        case GFSDK_Aftermath_Result_FAIL_DriverInitFailed:
            logger->warn("Aftermath: driver initialization failed - driver may not support Aftermath on this GPU");
            break;
        case GFSDK_Aftermath_Result_FAIL_DriverVersionNotSupported:
            logger->warn("Aftermath: driver version not supported (need R455+)");
            break;
        case GFSDK_Aftermath_Result_FAIL_NvApiIncompatible:
            logger->warn("Aftermath: NvAPI incompatible - update NVIDIA driver");
            break;
        case GFSDK_Aftermath_Result_FAIL_Disabled:
            logger->warn("Aftermath: disabled by environment (NV_AFTERMATH_FORCE_OFF)");
            break;
        case GFSDK_Aftermath_Result_FAIL_AlreadyInitialized:
            getAftermathState().initialized = true;
            logger->info("Aftermath: already initialized");
            return true;
        default:
            logger->warn("Aftermath: init failed with code 0x{:08X}", static_cast<uint32_t>(result));
            break;
    }
    return false;
}

export void shutdownAftermath() {
    auto& state = getAftermathState();
    if (state.initialized) {
        GFSDK_Aftermath_DisableGpuCrashDumps();
        state.initialized = false;
    }
}

export GFSDK_Aftermath_CrashDump_Status getCrashDumpStatus() {
    GFSDK_Aftermath_CrashDump_Status status = GFSDK_Aftermath_CrashDump_Status_Unknown;
    GFSDK_Aftermath_GetCrashDumpStatus(&status);
    return status;
}

export void waitForCrashDump(uint32_t timeoutMs = 10000) {
    auto& state = getAftermathState();
    auto logger = RLLogger::getInstance();
    if (!logger) return;

    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            logger->warn("Aftermath: crash dump wait timeout after {}ms", timeoutMs);
            break;
        }

        GFSDK_Aftermath_CrashDump_Status status = getCrashDumpStatus();

        if (status == GFSDK_Aftermath_CrashDump_Status_Finished ||
            status == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed) {
            if (status == GFSDK_Aftermath_CrashDump_Status_Finished) {
                logger->info("Aftermath: crash dump processing finished");
            } else {
                logger->warn("Aftermath: crash dump data collection failed");
            }
            break;
        }

        if (status == GFSDK_Aftermath_CrashDump_Status_InvokingCallback) {
            std::unique_lock<std::mutex> lock(state.dumpMutex);
            state.dumpCv.wait_for(lock, std::chrono::milliseconds(500));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (state.crashDumpPending) {
        decodeCrashDump();
    }
}

static void decodeCrashDump() {
    auto& state = getAftermathState();
    auto logger = RLLogger::getInstance();
    if (!logger || state.crashDumpData.empty()) return;

    std::lock_guard<std::mutex> lock(state.dumpMutex);

    GFSDK_Aftermath_GpuCrashDump_Decoder decoder = nullptr;
    auto result = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
        GFSDK_Aftermath_Version_API,
        state.crashDumpData.data(),
        static_cast<uint32_t>(state.crashDumpData.size()),
        &decoder);

    if (!GFSDK_Aftermath_SUCCEED(result)) {
        logger->error("Aftermath: decoder creation failed (0x{:08X})", static_cast<uint32_t>(result));
        return;
    }

    GFSDK_Aftermath_GpuCrashDump_BaseInfo baseInfo{};
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetBaseInfo(decoder, &baseInfo))) {
        logger->error("Aftermath: app={}, date={}, pid={}, api={}",
                      baseInfo.applicationName, baseInfo.creationDate,
                      baseInfo.pid, static_cast<uint32_t>(baseInfo.graphicsApi));
    }

    GFSDK_Aftermath_GpuCrashDump_DeviceInfo deviceInfo{};
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo(decoder, &deviceInfo))) {
        static const char* statusNames[] = {
            "Active", "Timeout", "OutOfMemory", "PageFault",
            "Stopped", "Reset", "Unknown", "DmaFault", "DeviceRemovedNoGpuFault"
        };
        uint32_t statusIdx = static_cast<uint32_t>(deviceInfo.status);
        const char* statusName = (statusIdx < std::size(statusNames))
            ? statusNames[statusIdx] : "Invalid";
        logger->error("Aftermath: device status={}, adapterReset={}, engineReset={}",
                      statusName, deviceInfo.adapterReset, deviceInfo.engineReset);
    }

    GFSDK_Aftermath_GpuCrashDump_PageFaultInfo pageFaultInfo{};
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetPageFaultInfo(decoder, &pageFaultInfo))) {
        logger->error("Aftermath: pageFault VA=0x{:x} type={} access={} engine={} client={}",
                      static_cast<unsigned long long>(pageFaultInfo.faultingGpuVA),
                      static_cast<uint32_t>(pageFaultInfo.faultType),
                      static_cast<uint32_t>(pageFaultInfo.accessType),
                      static_cast<uint32_t>(pageFaultInfo.engine),
                      static_cast<uint32_t>(pageFaultInfo.client));

        if (pageFaultInfo.resourceInfoCount > 0) {
            std::vector<GFSDK_Aftermath_GpuCrashDump_ResourceInfo> resources(pageFaultInfo.resourceInfoCount);
            GFSDK_Aftermath_GpuCrashDump_GetPageFaultResourceInfo(decoder, pageFaultInfo.resourceInfoCount, resources.data());
            for (uint32_t i = 0; i < pageFaultInfo.resourceInfoCount; i++) {
                logger->error("Aftermath:   resource[{}]: VA=0x{:x} size={} format={} destroyed={} residency={}",
                              i, resources[i].gpuVa, resources[i].size,
                              resources[i].format, resources[i].bWasDestroyed,
                              static_cast<uint32_t>(resources[i].residency));
            }
        }
    }

    uint32_t shaderCount = 0;
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(decoder, &shaderCount))) {
        logger->error("Aftermath: active shader count={}", shaderCount);
        if (shaderCount > 0) {
            std::vector<GFSDK_Aftermath_GpuCrashDump_ShaderInfo> shaders(shaderCount);
            if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo(decoder, shaderCount, shaders.data()))) {
                for (const auto& shader : shaders) {
                    static const char* shaderTypeNames[] = {
                        "Unknown", "Vertex", "TessCtrl", "TessEval", "Geometry",
                        "Fragment", "Compute", "RT_RayGen", "RT_Miss", "RT_Intersection",
                        "RT_AnyHit", "RT_ClosestHit", "RT_Callable", "RT_Internal",
                        "Mesh", "Task"
                    };
                    uint32_t typeIdx = static_cast<uint32_t>(shader.shaderType);
                    const char* typeName = (typeIdx < std::size(shaderTypeNames))
                        ? shaderTypeNames[typeIdx] : "Unknown";
                    logger->error("Aftermath:   shader: hash=0x{:x} type={} internal={} debugUid=0x{:x}_{:x}",
                                  shader.shaderHash, typeName, shader.isInternal,
                                  static_cast<uint64_t>(shader.shaderDebugInfoUid >> 32),
                                  static_cast<uint32_t>(shader.shaderDebugInfoUid));
                }
            }
        }
    }

    uint32_t markerCount = 0;
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfoCount(decoder, &markerCount))) {
        logger->error("Aftermath: event marker count={}", markerCount);
        if (markerCount > 0) {
            std::vector<GFSDK_Aftermath_GpuCrashDump_EventMarkerInfo> markers(markerCount);
            if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfo(decoder, markerCount, markers.data()))) {
                for (const auto& marker : markers) {
                    logger->error("Aftermath:   marker: contextId=0x{:x} status={} type={} dataSize={}",
                                  static_cast<uint32_t>(marker.contextId),
                                  static_cast<uint32_t>(marker.contextStatus),
                                  static_cast<uint32_t>(marker.contextType),
                                  marker.markerDataSize);
                }
            }
        }
    }

    uint32_t jsonSize = 0;
    uint32_t decoderFlags =
        GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO;
    uint32_t formatFlags =
        GFSDK_Aftermath_GpuCrashDumpFormatterFlags_UTF8_OUTPUT;
    result = GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
        decoder, decoderFlags, formatFlags, shaderDebugInfoLookupCallback, nullptr, nullptr, nullptr, &jsonSize);

    if (GFSDK_Aftermath_SUCCEED(result) && jsonSize > 0) {
        std::vector<char> jsonBuffer(jsonSize);
        if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetJSON(decoder, jsonSize, jsonBuffer.data()))) {
            std::string dumpFile = "aftermath_crash_dump.json";
            std::ofstream ofs(dumpFile);
            if (ofs) {
                ofs.write(jsonBuffer.data(), jsonSize);
                ofs.close();
                logger->error("Aftermath: JSON crash dump written to {} ({} bytes)", dumpFile, jsonSize);
            }
        }
    }

    GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
    state.crashDumpPending = false;
}

export vk::DeviceDiagnosticsConfigCreateInfoNV
createDiagnosticsConfig() {
    vk::DeviceDiagnosticsConfigCreateInfoNV config{};
    config.flags = vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderErrorReporting |
                   vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableResourceTracking |
                   vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableAutomaticCheckpoints |
                   vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderDebugInfo;
    return config;
}

export vk::DeviceDiagnosticsConfigFlagsNV getDiagnosticsFlags() {
    return vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderErrorReporting |
           vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableResourceTracking |
           vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableAutomaticCheckpoints |
           vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderDebugInfo;
}

export bool isAftermathInitialized() {
    return getAftermathState().initialized;
}

}
