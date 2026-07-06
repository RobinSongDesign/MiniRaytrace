// mrt_viewer: interactive path-traced preview with an ImGui control panel.
//
// Usage: mrt_viewer [model.obj] [--env sky.hdr] [--env-intensity f]
//                   [--size WxH] [--validation]
// Controls: LMB orbit | RMB / Shift+LMB pan | scroll zoom | Esc quit
//           drag & drop: .obj replaces model, .hdr sets environment,
//           images set the selected material's base color.

#include "Gui.hpp"
#include "ObjLoader.hpp"
#include "OrbitCamera.hpp"
#include "Window.hpp"

#include <mrt/Engine.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct Options {
    std::string objPath;
    std::string envPath;
    float envIntensity = 1.0f;
    uint32_t width = 1280, height = 720;
    bool validation = false;
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--env" && i + 1 < argc) o.envPath = argv[++i];
        else if (a == "--env-intensity" && i + 1 < argc) o.envIntensity = std::stof(argv[++i]);
        else if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%ux%u", &o.width, &o.height);
        else if (a == "--validation") o.validation = true;
        else if (a[0] != '-') o.objPath = a;
        else { fprintf(stderr, "unknown option: %s\n", a.c_str()); return false; }
    }
    return true;
}

// Window-callback context: camera input + app state.
struct Ctx {
    viewer::AppState app;
    bool rotating = false, panning = false;
    double lastX = 0.0, lastY = 0.0;
    bool cameraDirty = false;
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    if (!glfwVulkanSupported()) {
        fprintf(stderr, "Vulkan loader not found (install the Vulkan SDK / MoltenVK)\n");
        return 1;
    }

    // --- engine ---------------------------------------------------------------
    const auto instanceExts = viewer::Window::requiredInstanceExtensions();
    mrt::EngineDesc desc;
    desc.enableValidation = opt.validation;
    desc.needPresentSupport = true;
    desc.instanceExtensions = instanceExts.data();
    desc.instanceExtensionCount = uint32_t(instanceExts.size());
    desc.settings.width = opt.width;
    desc.settings.height = opt.height;

    std::unique_ptr<mrt::Engine> engine;
    if (mrt::Engine::create(desc, engine) != mrt::Result::Success) {
        fprintf(stderr, "engine creation failed\n");
        return 1;
    }

    // --- window + input --------------------------------------------------------
    viewer::Window window;
    if (!window.create(*engine, opt.width, opt.height, "MiniRaytrace")) return 1;

    viewer::OrbitCamera camera;
    Ctx ctx;
    ctx.app.engine = engine.get();
    ctx.app.camera = &camera;
    glfwSetWindowUserPointer(window.glfw(), &ctx);

    // Camera callbacks are installed BEFORE Gui::init so ImGui chains them.
    glfwSetMouseButtonCallback(window.glfw(), [](GLFWwindow* w, int button, int action, int mods) {
        if (viewer::Gui::wantsMouse()) return;
        auto* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
        const bool down = action == GLFW_PRESS;
        if (button == GLFW_MOUSE_BUTTON_LEFT)
            ((mods & GLFW_MOD_SHIFT) ? c->panning : c->rotating) = down;
        if (button == GLFW_MOUSE_BUTTON_RIGHT) c->panning = down;
        if (!down) { c->rotating = false; c->panning = false; }
        glfwGetCursorPos(w, &c->lastX, &c->lastY);
    });
    glfwSetCursorPosCallback(window.glfw(), [](GLFWwindow* w, double x, double y) {
        auto* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
        const float dx = float(x - c->lastX), dy = float(y - c->lastY);
        c->lastX = x; c->lastY = y;
        if (viewer::Gui::wantsMouse()) return;
        if (c->rotating) { c->app.camera->rotate(dx, dy); c->cameraDirty = true; }
        if (c->panning)  { c->app.camera->pan(dx, dy);    c->cameraDirty = true; }
    });
    glfwSetScrollCallback(window.glfw(), [](GLFWwindow* w, double, double dy) {
        if (viewer::Gui::wantsMouse()) return;
        auto* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
        c->app.camera->zoom(float(dy));
        c->cameraDirty = true;
    });
    glfwSetKeyCallback(window.glfw(), [](GLFWwindow* w, int key, int, int action, int) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
            glfwSetWindowShouldClose(w, GLFW_TRUE);
    });
    glfwSetDropCallback(window.glfw(), [](GLFWwindow* w, int count, const char** paths) {
        auto* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
        for (int i = 0; i < count; ++i)
            viewer::handleDrop(c->app, paths[i]);
    });

    // --- GUI --------------------------------------------------------------------
    viewer::Gui gui;
    if (!gui.init(window.glfw(), *engine, window.renderPass(), window.imageCount())) {
        fprintf(stderr, "ImGui init failed\n");
        return 1;
    }

    // --- initial scene -------------------------------------------------------------
    if (!opt.objPath.empty())
        viewer::loadModel(ctx.app, opt.objPath);

    if (!opt.envPath.empty()) {
        ctx.app.envIntensity = opt.envIntensity;
        viewer::loadEnv(ctx.app, opt.envPath);
    } else {
        // Default key + fill suns so an un-lit launch still shows the model.
        mrt::LightDesc sun;
        sun.type = mrt::LightType::Sun;
        sun.direction[0] = 0.4f; sun.direction[1] = 1.0f; sun.direction[2] = 0.3f;
        sun.radiance[0] = sun.radiance[1] = sun.radiance[2] = 600.0f;
        sun.angularRadius = 0.0465f;
        viewer::addLight(ctx.app, sun, "key sun");
        mrt::LightDesc fill = sun;
        fill.direction[0] = -0.5f; fill.direction[1] = 0.6f; fill.direction[2] = -0.4f;
        fill.radiance[0] = fill.radiance[1] = fill.radiance[2] = 120.0f;
        viewer::addLight(ctx.app, fill, "fill sun");
    }
    engine->scene().setCamera(camera.toDesc());

    // --- main loop ---------------------------------------------------------------
    while (!window.shouldClose()) {
        glfwPollEvents();

        // Resize follows the framebuffer.
        uint32_t fbw = 0, fbh = 0;
        window.framebufferSize(fbw, fbh);
        const auto& s = engine->renderSettings();
        if (fbw != s.width || fbh != s.height) {
            mrt::RenderSettings ns = s;
            ns.width = fbw; ns.height = fbh;
            engine->setRenderSettings(ns);
        }

        if (ctx.cameraDirty) {
            engine->scene().setCamera(camera.toDesc());
            ctx.cameraDirty = false;
        }

        gui.newFrame();
        gui.buildPanels(ctx.app);
        gui.render();

        if (engine->renderFrame(ctx.app.lastFrame) != mrt::Result::Success) break;
        window.present(*engine, [&](VkCommandBuffer cmd) { gui.recordDrawData(cmd); });
    }

    gui.shutdown();
    window.destroy();
    engine.reset();
    glfwTerminate();
    return 0;
}
