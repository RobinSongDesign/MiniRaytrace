#pragma once
// Dear ImGui integration + the viewer's control panels:
// model loading, material editing, light management, environment, render settings.

#include "ObjLoader.hpp"
#include "OrbitCamera.hpp"
#include "Window.hpp"

#include <mrt/Engine.hpp>

#include <string>
#include <vector>

namespace viewer {

struct MaterialEntry {
    mrt::MaterialId   id = mrt::kInvalidId;
    std::string       name;
    mrt::MaterialDesc desc; // UI-side copy; pushed via updateMaterial on edit
};

struct LightEntry {
    mrt::LightId   id = mrt::kInvalidId;
    std::string    name;
    mrt::LightDesc desc;
};

// Everything the panels read/mutate. Owned by main().
struct AppState {
    mrt::Engine* engine = nullptr;
    OrbitCamera* camera = nullptr;

    // Current model
    std::string modelPath;
    size_t triangleCount = 0;
    std::vector<mrt::MeshId>     meshes;
    std::vector<mrt::InstanceId> instances;
    std::vector<MaterialEntry>   materials;
    int selectedMaterial = 0;

    // Lights
    std::vector<LightEntry> lights;
    int lightCounter = 0;

    // Environment
    std::string envPath;
    bool  hasEnv = false;
    float envRotation = 0.0f;
    float envIntensity = 1.0f;

    // UI scratch
    char objInput[512] = {};
    char envInput[512] = {};
    char texInput[512] = {};
    std::string status = "drop an .obj / .hdr / image file onto the window";
    mrt::FrameInfo lastFrame{};
};

// Replace the current model (removes previous meshes/instances/materials).
void loadModel(AppState& app, const std::string& path);
void loadEnv(AppState& app, const std::string& path);
void addLight(AppState& app, const mrt::LightDesc& desc, const std::string& name);
// Dispatch a dropped file by extension (.obj / .hdr / image).
void handleDrop(AppState& app, const std::string& path);

class Gui {
public:
    bool init(GLFWwindow* window, mrt::Engine& engine, VkRenderPass renderPass,
              uint32_t imageCount);
    void shutdown();

    void newFrame();
    void buildPanels(AppState& app);          // between newFrame and render
    void render();                            // ImGui::Render
    void recordDrawData(VkCommandBuffer cmd); // inside the window's render pass

    // True when ImGui wants the mouse (camera callbacks should ignore input).
    static bool wantsMouse();

private:
    mrt::Engine* m_engine = nullptr;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    bool m_initialised = false;
};

} // namespace viewer
