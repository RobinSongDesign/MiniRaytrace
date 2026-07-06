#include "Gui.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace viewer {

namespace fs = std::filesystem;

// ------------------------------------------------------------ asset actions --
void loadModel(AppState& app, const std::string& path) {
    mrt::Scene& scene = app.engine->scene();

    // Remove the previous model (removeMesh cascades its instances).
    for (mrt::MeshId m : app.meshes) scene.removeMesh(m);
    for (const MaterialEntry& e : app.materials) scene.removeMaterial(e.id);
    app.meshes.clear();
    app.instances.clear();
    app.materials.clear();
    app.selectedMaterial = 0;

    const LoadResult r = loadObjIntoScene(scene, path);
    if (!r.ok) {
        app.status = "OBJ load failed: " + path;
        return;
    }
    app.modelPath = path;
    app.triangleCount = r.triangleCount;
    app.meshes = r.meshes;
    app.instances = r.instances;
    for (const auto& [id, name] : r.materials)
        app.materials.push_back({ id, name, scene.materials().at(id) });

    app.camera->fit(r.boundsLo, r.boundsHi);
    scene.setCamera(app.camera->toDesc());
    app.status = "loaded " + fs::path(path).filename().string() + " (" +
                 std::to_string(r.triangleCount) + " tris)";
}

void loadEnv(AppState& app, const std::string& path) {
    if (loadEnvironment(app.engine->scene(), path, app.envRotation, app.envIntensity)) {
        app.envPath = path;
        app.hasEnv = true;
        app.status = "environment: " + fs::path(path).filename().string();
    } else {
        app.status = "HDR load failed: " + path;
    }
}

void addLight(AppState& app, const mrt::LightDesc& desc, const std::string& name) {
    const mrt::LightId id = app.engine->scene().addLight(desc);
    app.lights.push_back({ id, name + " " + std::to_string(++app.lightCounter), desc });
}

void handleDrop(AppState& app, const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".obj") {
        loadModel(app, path);
    } else if (ext == ".hdr") {
        loadEnv(app, path);
    } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
        // Assign as base color of the selected material.
        if (app.materials.empty()) {
            app.status = "load a model before assigning textures";
            return;
        }
        const mrt::TextureId tex = loadImageTexture(app.engine->scene(), path, true);
        if (tex == mrt::kInvalidId) {
            app.status = "texture load failed: " + path;
            return;
        }
        MaterialEntry& e = app.materials[size_t(app.selectedMaterial)];
        e.desc.baseColorTex = tex;
        e.desc.baseColor[0] = e.desc.baseColor[1] = e.desc.baseColor[2] = 1.0f;
        app.engine->scene().updateMaterial(e.id, e.desc);
        app.status = "baseColor texture -> " + e.name;
    } else {
        app.status = "unsupported file type: " + ext;
    }
}

// ------------------------------------------------------------------- panels --
namespace {

void statsPanel(AppState& app) {
    ImGui::Text("%zu tris | %u spp | %.1f ms", app.triangleCount,
                app.lastFrame.spp, app.lastFrame.frameMs);
    if (app.lastFrame.denoised) ImGui::SameLine(), ImGui::TextUnformatted("| denoised");
    if (app.lastFrame.converged) ImGui::SameLine(), ImGui::TextUnformatted("| converged");
    ImGui::TextWrapped("%s", app.status.c_str());
}

void modelPanel(AppState& app) {
    if (!ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("current: %s",
                       app.modelPath.empty() ? "(none)" : app.modelPath.c_str());
    ImGui::InputText("##objpath", app.objInput, sizeof(app.objInput));
    ImGui::SameLine();
    if (ImGui::Button("Load OBJ") && app.objInput[0])
        loadModel(app, app.objInput);
    ImGui::TextDisabled("tip: drag & drop an .obj onto the window");
}

void materialPanel(AppState& app) {
    if (!ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (app.materials.empty()) {
        ImGui::TextDisabled("no materials (load a model)");
        return;
    }

    if (ImGui::BeginCombo("material",
                          app.materials[size_t(app.selectedMaterial)].name.c_str())) {
        for (int i = 0; i < int(app.materials.size()); ++i)
            if (ImGui::Selectable(app.materials[size_t(i)].name.c_str(),
                                  i == app.selectedMaterial))
                app.selectedMaterial = i;
        ImGui::EndCombo();
    }

    MaterialEntry& e = app.materials[size_t(app.selectedMaterial)];
    mrt::MaterialDesc& d = e.desc;
    bool changed = false;
    changed |= ImGui::ColorEdit3("base color", d.baseColor);
    changed |= ImGui::SliderFloat("roughness", &d.roughness, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("metallic", &d.metallic, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("transmission", &d.transmission, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("ior", &d.ior, 1.0f, 2.5f);
    changed |= ImGui::SliderFloat("opacity", &d.opacity, 0.0f, 1.0f);
    changed |= ImGui::DragFloat3("emission", d.emission, 0.1f, 0.0f, 100.0f);

    ImGui::Text("textures: %s%s%s%s",
                d.baseColorTex != mrt::kInvalidId ? "[color] " : "",
                d.normalTex != mrt::kInvalidId ? "[normal] " : "",
                d.roughnessTex != mrt::kInvalidId ? "[rough] " : "",
                d.emissionTex != mrt::kInvalidId ? "[emissive] " : "");
    ImGui::InputText("##texpath", app.texInput, sizeof(app.texInput));
    ImGui::SameLine();
    if (ImGui::Button("Set color tex") && app.texInput[0]) {
        const mrt::TextureId tex =
            loadImageTexture(app.engine->scene(), app.texInput, true);
        if (tex != mrt::kInvalidId) {
            d.baseColorTex = tex;
            d.baseColor[0] = d.baseColor[1] = d.baseColor[2] = 1.0f;
            changed = true;
        }
    }
    ImGui::TextDisabled("tip: drop an image to set the selected material's color");

    if (changed)
        app.engine->scene().updateMaterial(e.id, d);
}

void lightPanel(AppState& app) {
    if (!ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (ImGui::Button("+ Sun")) {
        mrt::LightDesc d;
        d.type = mrt::LightType::Sun;
        d.direction[0] = 0.4f; d.direction[1] = 1.0f; d.direction[2] = 0.3f;
        d.radiance[0] = d.radiance[1] = d.radiance[2] = 600.0f;
        d.angularRadius = 0.0465f;
        addLight(app, d, "sun");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Point")) {
        mrt::LightDesc d;
        d.type = mrt::LightType::Point;
        d.position[1] = 2.0f;
        d.radiance[0] = d.radiance[1] = d.radiance[2] = 20.0f;
        addLight(app, d, "point");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Rect")) {
        mrt::LightDesc d;
        d.type = mrt::LightType::Rect;
        d.corner[0] = -1.0f; d.corner[1] = 3.0f; d.corner[2] = -1.0f;
        d.edge0[0] = 2.0f; d.edge0[1] = 0.0f; d.edge0[2] = 0.0f;
        d.edge1[0] = 0.0f; d.edge1[1] = 0.0f; d.edge1[2] = 2.0f;
        d.radiance[0] = d.radiance[1] = d.radiance[2] = 8.0f;
        addLight(app, d, "rect");
    }

    int removeIdx = -1;
    for (int i = 0; i < int(app.lights.size()); ++i) {
        LightEntry& e = app.lights[size_t(i)];
        ImGui::PushID(i);
        if (ImGui::TreeNode(e.name.c_str())) {
            mrt::LightDesc& d = e.desc;
            bool changed = false;
            switch (d.type) {
                case mrt::LightType::Sun:
                    changed |= ImGui::DragFloat3("direction", d.direction, 0.01f, -1.0f, 1.0f);
                    changed |= ImGui::DragFloat3("radiance", d.radiance, 5.0f, 0.0f, 5000.0f);
                    changed |= ImGui::SliderFloat("angular radius", &d.angularRadius, 0.005f, 0.4f);
                    break;
                case mrt::LightType::Point:
                    changed |= ImGui::DragFloat3("position", d.position, 0.05f);
                    changed |= ImGui::DragFloat3("intensity", d.radiance, 0.5f, 0.0f, 2000.0f);
                    break;
                case mrt::LightType::Rect:
                    changed |= ImGui::DragFloat3("corner", d.corner, 0.05f);
                    changed |= ImGui::DragFloat3("edge u", d.edge0, 0.05f);
                    changed |= ImGui::DragFloat3("edge v", d.edge1, 0.05f);
                    changed |= ImGui::DragFloat3("radiance", d.radiance, 0.5f, 0.0f, 2000.0f);
                    changed |= ImGui::Checkbox("two sided", &d.twoSided);
                    break;
            }
            if (changed)
                app.engine->scene().updateLight(e.id, d);
            if (ImGui::Button("Remove")) removeIdx = i;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeIdx >= 0) {
        app.engine->scene().removeLight(app.lights[size_t(removeIdx)].id);
        app.lights.erase(app.lights.begin() + removeIdx);
    }
}

void environmentPanel(AppState& app) {
    if (!ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("current: %s", app.hasEnv ? app.envPath.c_str() : "(none)");
    ImGui::InputText("##envpath", app.envInput, sizeof(app.envInput));
    ImGui::SameLine();
    if (ImGui::Button("Load HDR") && app.envInput[0])
        loadEnv(app, app.envInput);

    if (app.hasEnv) {
        bool changed = false;
        changed |= ImGui::SliderAngle("rotation", &app.envRotation, 0.0f, 360.0f);
        changed |= ImGui::SliderFloat("intensity", &app.envIntensity, 0.0f, 8.0f);
        if (changed)
            app.engine->scene().setEnvironmentParams(app.envRotation, app.envIntensity);
        if (ImGui::Button("Clear environment")) {
            app.engine->scene().clearEnvironment();
            app.hasEnv = false;
        }
    }
}

void renderPanel(AppState& app) {
    if (!ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) return;
    mrt::RenderSettings s = app.engine->renderSettings();
    bool changed = false;

    changed |= ImGui::SliderFloat("exposure EV", &s.exposureEV, -4.0f, 4.0f);

    static const char* tonemaps[] = { "Linear", "ACES" };
    int tm = int(s.tonemap);
    if (ImGui::Combo("tonemap", &tm, tonemaps, 2)) { s.tonemap = mrt::TonemapMode(tm); changed = true; }

    static const char* views[] = { "Render", "Albedo", "Normal" };
    int dv = int(s.debugView);
    if (ImGui::Combo("view", &dv, views, 3)) { s.debugView = mrt::DebugView(dv); changed = true; }

    int bounces = int(s.maxBounces);
    if (ImGui::SliderInt("max bounces", &bounces, 1, 16)) { s.maxBounces = uint32_t(bounces); changed = true; }
    changed |= ImGui::Checkbox("denoise", &s.denoise);

    if (changed)
        app.engine->setRenderSettings(s);
}

} // namespace

// ---------------------------------------------------------------- Gui class --
bool Gui::init(GLFWwindow* window, mrt::Engine& engine, VkRenderPass renderPass,
               uint32_t imageCount) {
    m_engine = &engine;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha = 0.96f;

    // install_callbacks=true: ImGui chains the app's previously-set callbacks.
    if (!ImGui_ImplGlfw_InitForVulkan(window, true)) return false;

    VkDevice device = reinterpret_cast<VkDevice>(engine.vkDevice());
    const VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 16;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &pci, nullptr, &m_pool) != VK_SUCCESS) return false;

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = reinterpret_cast<VkInstance>(engine.vkInstance());
    info.PhysicalDevice = reinterpret_cast<VkPhysicalDevice>(engine.vkPhysicalDevice());
    info.Device = device;
    info.QueueFamily = engine.queueFamilyIndex();
    info.Queue = reinterpret_cast<VkQueue>(engine.vkQueue());
    info.DescriptorPool = m_pool;
    info.RenderPass = renderPass;
    info.MinImageCount = 2;
    info.ImageCount = std::max(imageCount, 2u);
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info)) return false;

    m_initialised = true;
    return true;
}

void Gui::shutdown() {
    if (!m_initialised) return;
    VkDevice device = reinterpret_cast<VkDevice>(m_engine->vkDevice());
    vkDeviceWaitIdle(device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device, m_pool, nullptr);
    m_initialised = false;
}

void Gui::newFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Gui::buildPanels(AppState& app) {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("MiniRaytrace");
    statsPanel(app);
    ImGui::Separator();
    modelPanel(app);
    materialPanel(app);
    lightPanel(app);
    environmentPanel(app);
    renderPanel(app);
    ImGui::End();
}

void Gui::render() { ImGui::Render(); }

void Gui::recordDrawData(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

bool Gui::wantsMouse() {
    return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

} // namespace viewer
