#pragma once
// Common types shared across the MiniRaytrace core library.

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>

namespace mrt {

enum class Result : int32_t {
    Success = 0,
    ErrorUnknown,
    ErrorVulkanInit,
    ErrorOutOfMemory,
    ErrorInvalidArgument,
    ErrorInvalidHandle,
    ErrorShaderLoad,
    ErrorDeviceLost,
};

const char* toString(Result r);

// Strongly-typed scene handles. 0 is never a valid id.
using MeshId     = uint32_t;
using InstanceId = uint32_t;
using MaterialId = uint32_t;
using TextureId  = uint32_t;
using LightId    = uint32_t;
inline constexpr uint32_t kInvalidId = 0;

enum class LogLevel { Debug, Info, Warn, Error };
using LogFn = std::function<void(LogLevel, const std::string&)>;

void setLogCallback(LogFn fn);
void log(LogLevel level, const std::string& msg);

// Human-readable detail for the most recent ErrorXxx Result returned to a
// caller on this thread (PRD §8 A4 — e.g. "no Vulkan loader found"). Empty
// string if nothing has failed yet. Overwritten by the next failure.
const std::string& lastErrorMessage();
void setLastErrorMessage(const std::string& msg);

// Hard limits for v1 (see PRD §10 R3: conservative texture slot count for MoltenVK).
inline constexpr uint32_t kMaxTextures   = 64;
inline constexpr uint32_t kMaxBouncesCap = 16;

} // namespace mrt
