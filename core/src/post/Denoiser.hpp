#pragma once
// Async OIDN wrapper (PRD §7). One background worker; jobs are tagged with an
// accumulation epoch so results from a stale accumulation are dropped.
// Compiled without MRT_ENABLE_OIDN this degrades to an inert stub.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace mrt {

class Denoiser {
public:
    Denoiser();
    ~Denoiser();

    bool available() const { return m_available; }
    bool idle() const;

    // Inputs are RGB float planes (3 floats per pixel). Returns false if busy.
    bool start(uint64_t epoch, uint32_t width, uint32_t height,
               std::vector<float> color, std::vector<float> albedo, std::vector<float> normal);

    // Fetch a finished result matching `epoch`. Stale results are discarded.
    bool tryFetch(uint64_t epoch, std::vector<float>& outColor);

private:
    void workerLoop();

    bool m_available = false;
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_quit = false;

    enum class State { Idle, Pending, Running, Done };
    State    m_state = State::Idle;
    uint64_t m_epoch = 0;
    uint32_t m_width = 0, m_height = 0;
    std::vector<float> m_color, m_albedo, m_normal, m_result;

    void* m_device = nullptr; // OIDNDevice when enabled
};

} // namespace mrt
