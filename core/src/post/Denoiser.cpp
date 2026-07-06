#include "Denoiser.hpp"
#include "mrt/Common.hpp"

#ifdef MRT_ENABLE_OIDN
#include <OpenImageDenoise/oidn.h>
#endif

namespace mrt {

Denoiser::Denoiser() {
#ifdef MRT_ENABLE_OIDN
    OIDNDevice dev = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
    if (dev) {
        oidnCommitDevice(dev);
        const char* err = nullptr;
        if (oidnGetDeviceError(dev, &err) == OIDN_ERROR_NONE) {
            m_device = dev;
            m_available = true;
            m_worker = std::thread([this] { workerLoop(); });
            log(LogLevel::Info, "OIDN denoiser initialised (CPU device)");
        } else {
            log(LogLevel::Warn, std::string("OIDN unavailable: ") + (err ? err : "?"));
            oidnReleaseDevice(dev);
        }
    }
#endif
}

Denoiser::~Denoiser() {
    {
        std::lock_guard l(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
#ifdef MRT_ENABLE_OIDN
    if (m_device) oidnReleaseDevice(static_cast<OIDNDevice>(m_device));
#endif
}

bool Denoiser::idle() const {
    std::lock_guard l(m_mutex);
    return m_state == State::Idle;
}

bool Denoiser::start(uint64_t epoch, uint32_t width, uint32_t height,
                     std::vector<float> color, std::vector<float> albedo,
                     std::vector<float> normal) {
    if (!m_available) return false;
    {
        std::lock_guard l(m_mutex);
        if (m_state != State::Idle) return false;
        m_epoch = epoch;
        m_width = width; m_height = height;
        m_color = std::move(color);
        m_albedo = std::move(albedo);
        m_normal = std::move(normal);
        m_state = State::Pending;
    }
    m_cv.notify_one();
    return true;
}

bool Denoiser::tryFetch(uint64_t epoch, std::vector<float>& outColor) {
    std::lock_guard l(m_mutex);
    if (m_state != State::Done) return false;
    const bool match = (m_epoch == epoch);
    if (match) outColor = std::move(m_result);
    m_state = State::Idle; // stale results are simply discarded
    return match;
}

void Denoiser::workerLoop() {
#ifdef MRT_ENABLE_OIDN
    for (;;) {
        std::vector<float> color, albedo, normal;
        uint32_t w = 0, h = 0;
        {
            std::unique_lock l(m_mutex);
            m_cv.wait(l, [this] { return m_quit || m_state == State::Pending; });
            if (m_quit) return;
            m_state = State::Running;
            color = std::move(m_color);
            albedo = std::move(m_albedo);
            normal = std::move(m_normal);
            w = m_width; h = m_height;
        }

        OIDNDevice dev = static_cast<OIDNDevice>(m_device);
        OIDNFilter filter = oidnNewFilter(dev, "RT");
        std::vector<float> out(color.size());
        oidnSetSharedFilterImage(filter, "color",  color.data(),  OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
        oidnSetSharedFilterImage(filter, "albedo", albedo.data(), OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
        oidnSetSharedFilterImage(filter, "normal", normal.data(), OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
        oidnSetSharedFilterImage(filter, "output", out.data(),    OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
        oidnSetFilterBool(filter, "hdr", true);
        oidnCommitFilter(filter);
        oidnExecuteFilter(filter);
        const char* err = nullptr;
        if (oidnGetDeviceError(dev, &err) != OIDN_ERROR_NONE)
            log(LogLevel::Warn, std::string("OIDN filter error: ") + (err ? err : "?"));
        oidnReleaseFilter(filter);

        {
            std::lock_guard l(m_mutex);
            m_result = std::move(out);
            m_state = State::Done;
        }
    }
#endif
}

} // namespace mrt
