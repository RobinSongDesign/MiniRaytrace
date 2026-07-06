#pragma once
// Orbit camera: LMB rotate, RMB / shift+LMB pan, scroll zoom.

#include <mrt/SceneTypes.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace viewer {

class OrbitCamera {
public:
    void fit(const glm::vec3& boundsLo, const glm::vec3& boundsHi) {
        m_target = 0.5f * (boundsLo + boundsHi);
        const float radius = 0.5f * glm::length(boundsHi - boundsLo);
        m_distance = std::max(radius * 2.2f, 0.01f);
        m_minDistance = m_distance * 0.01f;
    }

    void rotate(float dx, float dy) {
        m_azimuth += dx * 0.01f;
        m_elevation = std::clamp(m_elevation + dy * 0.01f, -1.55f, 1.55f);
    }

    void pan(float dx, float dy) {
        const glm::vec3 fwd = forward();
        const glm::vec3 right = glm::normalize(glm::cross(fwd, kUp));
        const glm::vec3 up = glm::cross(right, fwd);
        const float scale = m_distance * 0.0015f;
        m_target += (-right * dx + up * dy) * scale;
    }

    void zoom(float scroll) {
        m_distance = std::max(m_distance * std::pow(0.9f, scroll), m_minDistance);
    }

    mrt::CameraDesc toDesc() const {
        const glm::vec3 pos = m_target - forward() * m_distance;
        mrt::CameraDesc d;
        d.position[0] = pos.x; d.position[1] = pos.y; d.position[2] = pos.z;
        d.target[0] = m_target.x; d.target[1] = m_target.y; d.target[2] = m_target.z;
        d.up[0] = kUp.x; d.up[1] = kUp.y; d.up[2] = kUp.z;
        d.fovYDeg = 45.0f;
        return d;
    }

private:
    glm::vec3 forward() const {
        const float ce = std::cos(m_elevation);
        return glm::normalize(glm::vec3(ce * std::cos(m_azimuth),
                                        std::sin(m_elevation),
                                        ce * std::sin(m_azimuth)) * -1.0f);
    }

    static constexpr glm::vec3 kUp{ 0.0f, 1.0f, 0.0f };
    glm::vec3 m_target{ 0.0f };
    float m_distance = 5.0f;
    float m_minDistance = 0.05f;
    float m_azimuth = glm::quarter_pi<float>();
    float m_elevation = 0.4f;
};

} // namespace viewer
