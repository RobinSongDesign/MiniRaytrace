# MiniRaytrace

轻量级 Vulkan compute shader 路径追踪渲染器。Mac (Apple Silicon) 首要平台,经
MoltenVK 运行;架构与技术决策见 [PRD.md](PRD.md)。

## 构建 (macOS)

前置条件只有一个:[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#mac)
(自带 MoltenVK 和 glslangValidator)。安装后确保环境变量生效:

```bash
# 通常由 SDK 安装器写入 ~/.zshrc;手动设置示例:
export VULKAN_SDK=$HOME/VulkanSDK/<version>/macOS
export PATH=$VULKAN_SDK/bin:$PATH
export DYLD_LIBRARY_PATH=$VULKAN_SDK/lib:$DYLD_LIBRARY_PATH
export VK_ICD_FILENAMES=$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json
```

其余依赖 (glm / VMA / GLFW / tinyobjloader / stb) 由 CMake FetchContent 自动拉取:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 运行

```bash
./build/viewer/mrt_viewer model.obj --env studio.hdr
./build/viewer/mrt_viewer model.obj                  # 无 HDRI 时使用默认太阳光
```

交互:左键旋转 | 右键 / Shift+左键平移 | 滚轮缩放 | Esc 退出。
标题栏显示当前 spp / 帧耗时 / 降噪状态。

## 打包独立 .app(macOS)

```bash
chmod +x tools/package_app.sh   # 首次
./tools/package_app.sh
```

产出 `build-release/viewer/MiniRaytrace.app` 和分发用 zip。Vulkan loader 与
MoltenVK 已打进 bundle,**目标机器无需安装 Vulkan SDK**,双击即用(首次启动
右键 > 打开,因为是 ad-hoc 签名)。启动后空场景,直接把 .obj / .hdr 拖进窗口。

## CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `MRT_BUILD_VIEWER` | ON | GLFW 交互查看器 |
| `MRT_BUILD_TESTS` | OFF | Catch2 单元测试 (`ctest --test-dir build`) |
| `MRT_ENABLE_OIDN` | OFF | OIDN 降噪;需先 `brew install open-image-denoise` |
| `MRT_DEV_SHADER_RELOAD` | OFF | 运行时从 build 目录热加载 SPIR-V,改 shader 后只需重跑 shader 编译目标 |
| `MRT_MACOS_BUNDLE` | OFF | 构建自包含 MiniRaytrace.app(打包 loader + MoltenVK) |

## 目录结构

```
core/      mrt_core 静态库 — GPU 上下文、场景、BVH、渲染、降噪 (无窗口依赖)
  shaders/ GLSL → SPIR-V (构建期编译并嵌入二进制)
capi/      libmrt 动态库 — 稳定 C ABI (mini_raytrace.h),供 Rhino C# P/Invoke
viewer/    mrt_viewer 可执行 — OBJ/MTL/HDR 加载 + 轨道相机
tests/     BVH 正确性单测 (遍历结果 vs 暴力求交)
```

## Rhino 集成 (Phase 2)

`capi/include/mini_raytrace.h` 即为对接面:增量场景 API 与 Rhino ChangeQueue
事件一一对应 (映射表见 PRD §9)。插件侧渲染循环:处理 ChangeQueue →
`mrtRenderFrame` → `mrtReadFramebuffer(MRT_PIXEL_RGBA32F_LINEAR)` → 写入
`RenderWindow`。集成时设 `tonemap = MRT_TONEMAP_LINEAR`,色彩管理交给 Rhino。
