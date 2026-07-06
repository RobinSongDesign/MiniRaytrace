# MiniRaytrace — 轻量级 Vulkan 路径追踪渲染器 技术设计文档 (PRD)

| | |
|---|---|
| 版本 | v0.1 (待审阅) |
| 日期 | 2026-06-10 |
| 作者 | Robin + Claude |
| 状态 | Draft — 审阅通过后开始编码 |

---

## 1. 目标与定位

构建一个**轻量、可嵌入**的 GPU 路径追踪渲染核心,初代以 macOS (Apple Silicon) 为首要平台,最终作为渲染核心合并进 Rhino 8 的自定义显示模式 (RealtimeDisplayMode),用于视口实时预览。

### 1.1 核心目标 (P0)

1. **Vulkan compute shader 路径追踪**:不依赖 `VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure`(MoltenVK 不支持,见 §10 风险),BVH 构建与遍历全部自研,经 MoltenVK 转译 Metal 在 Mac 上运行。单一代码路径,天然兼容 Windows/Linux。
2. **渐进式渲染**:逐帧累积收敛,相机/场景变更即重置累积;静止后调用 OIDN 降噪,体验对标 Rhino 自带 Raytraced (Cycles) 模式。
3. **可嵌入架构**:C++20 核心库,暴露稳定 C ABI;Rhino 插件 (C# / RhinoCommon) 后期通过 P/Invoke 复用同一核心。
4. **增量场景 API**:add/update/remove mesh、材质、光源、相机,从第一天对齐 Rhino ChangeQueue 的推送模型。
5. **独立查看器**:GLFW 窗口程序,加载 OBJ/MTL,轨道相机交互,验证完整渲染体验。

### 1.2 非目标 (Non-Goals, v1)

- 不做硬件光追双后端(Metal RT / VK_KHR_ray_tracing)— 留作 v2 演进方向,RHI 层预留抽象缝隙。
- 不做千万面级大场景(分块流式上传、BVH 压缩)。
- 不做体积渲染、次表面散射、毛发、运动模糊。
- 不做 glTF/USD 导入(格式解析与核心解耦,后续可加)。
- v1 不交付 Rhino 插件本体,只保证 C API 与 ChangeQueue 模型的映射关系经过设计验证(§9)。

### 1.3 成功标准

| 指标 | 目标 |
|---|---|
| 平台 | macOS 13+,Apple Silicon (M1 及以上) |
| 场景规模 | 10 万–200 万三角面流畅交互 |
| 交互性能 | 1080p,相机拖动时 ≥ 1 spp/frame,≥ 24 fps (M1 Pro) |
| 收敛体验 | 静止 1–2 秒内 OIDN 出图,视觉接近收敛 |
| 正确性 | 白炉测试 (furnace test) 通过;金图 (golden image) 回归不漂移 |
| 可嵌入性 | 核心库零全局状态、无窗口依赖,仅靠 C API 完成全流程渲染 |

---

## 2. 总体架构

```
┌─────────────────────────────────────────────────────────┐
│  Consumers                                              │
│  ┌──────────────────┐   ┌─────────────────────────────┐ │
│  │ mrt_viewer (v1)  │   │ Rhino Plugin C# (v2, 后期)   │ │
│  │ GLFW + 交互       │   │ RealtimeDisplayMode          │ │
│  └────────┬─────────┘   └──────────────┬──────────────┘ │
│           │  C++ 直接调用                │ P/Invoke       │
├───────────┴─────────────────────────────┴───────────────┤
│  mrt_capi  —  稳定 C ABI (mini_raytrace.h)               │
├──────────────────────────────────────────────────────────┤
│  mrt_core (C++20, 静态库)                                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────────┐ │
│  │ Scene    │ │ Accel    │ │ Render   │ │ PostProcess │ │
│  │ 增量场景  │ │ BLAS/TLAS│ │ PathTrace│ │ Tonemap+OIDN│ │
│  └──────────┘ └──────────┘ └──────────┘ └─────────────┘ │
│  ┌──────────────────────────┐ ┌────────────────────────┐ │
│  │ GpuContext (Vulkan 封装)  │ │ IO (tinyobjloader/stb) │ │
│  └──────────────────────────┘ └────────────────────────┘ │
├──────────────────────────────────────────────────────────┤
│  Vulkan 1.2  ──macOS──▶  MoltenVK  ──▶  Metal            │
│              ──Win/Linux──▶  原生驱动                      │
└──────────────────────────────────────────────────────────┘
```

### 2.1 模块职责

| 模块 | 职责 | 关键设计 |
|---|---|---|
| `GpuContext` | Vulkan 实例/设备/队列/内存分配 (VMA)、shader 加载、命令提交 | headless 可用,swapchain 由 viewer 自己持有 |
| `Scene` | 增量式场景图:mesh / instance / material / light / camera / environment | 句柄 + 代际 (generation) 校验;脏标记驱动 GPU 同步 |
| `Accel` | BLAS (每 mesh 一棵 SAH BVH) + TLAS (实例层) | CPU 构建,多线程;transform-only 变更走 TLAS refit |
| `Render` | 路径追踪 compute pass、累积缓冲、采样状态 | megakernel(v1),wavefront 留作 v2 |
| `PostProcess` | tonemap/曝光 compute pass;OIDN CPU 降噪 | AOV: color/albedo/normal |
| `IO` | OBJ/MTL 解析 → Scene API;HDR/PNG/JPG 贴图加载 | 仅 viewer 链接,核心库不依赖 |
| `mrt_capi` | C ABI 薄封装,错误码 + 不透明句柄 | 不暴露任何 C++/Vulkan 类型 |

### 2.2 线程模型

- **主线程**(调用方):所有 C API 调用须来自单线程(文档约定,内部 assert);`render_frame()` 为阻塞式提交+等待,或异步模式(见 §8 API)。
- **构建线程池**:BVH 构建、贴图解码在内部线程池执行;`scene_commit()` 等待全部就绪。
- **OIDN 线程**:降噪在后台线程跑,完成后置位,下一帧合成输出,不阻塞交互。

---

## 3. 渲染管线

每帧执行序列:

```
[场景脏?] ──是──▶ Sync Pass: 上传脏数据 / 重建 BVH / 重置累积 (frameIndex = 0)
      │
      ▼
PathTrace Pass (compute, 8x8 threadgroup)
   ray gen → BVH traverse → BSDF eval/sample → NEE+MIS → 累积到 accumBuffer
      │
      ▼
[静止帧数 ≥ N 且未降噪?] ──是──▶ 异步 OIDN(color+albedo+normal AOV)
      │
      ▼
Resolve Pass (compute)
   accum/spp → [降噪结果可用则取降噪图] → 曝光 → tonemap (ACES) → sRGB → outputImage
      │
      ▼
调用方消费: viewer blit 到 swapchain / Rhino 读回 CPU 缓冲
```

### 3.1 PathTrace kernel (megakernel)

- **每帧 1 spp**(可配 spp-per-frame),`accumBuffer` 为 `RGBA32F` storage buffer,`rgb` 累积辐射度、`a` 存样本计数。
- **路径深度**:最大 8 次弹射;第 3 次弹射起 Russian Roulette(以 throughput 最大分量为概率)。
- **随机数**:PCG4D(pixel.x, pixel.y, frameIndex, bounce),无状态、无需 RNG 缓冲。
- **抗锯齿**:像素内 jitter(后续可换 Sobol/Owen)。
- **AOV**:首次命中时写 albedo / world normal 两个辅助缓冲(供 OIDN),仅 frameIndex==0 时写入。
- v1 选择 megakernel 而非 wavefront 的理由:实现/调试成本低一个量级;Apple Silicon 上分支发散惩罚相对可控;百万面 + 1080p 的目标下 megakernel 足够。wavefront 重构列入 v2(§11)。

### 3.2 光源与采样

| 光源 | 采样策略 |
|---|---|
| HDRI 环境光 | 2D 边缘 CDF 重要性采样(亮度加权);miss ray 直接查环境图 |
| 方向光(太阳) | 给定角半径的锥内采样,产生软阴影 |
| 点光 | 直接 NEE,无面积 |
| 矩形面光 | 面上均匀采样 + solid angle 换算 |
| 自发光 mesh | 路径自然命中 + 加入光源列表做 NEE(按功率加权选择) |

- **NEE + MIS**(balance heuristic):每个着色点采 1 个光源样本 + 1 个 BSDF 样本;光源选择按功率构建别名表 (alias table)。
- HDRI 的 CDF / 别名表在 CPU 构建,作为 buffer 上传,环境图更换时重建。

### 3.3 BSDF — "Disney-lite"

单一 über-material,参数:

```
baseColor (RGB | texture)     metallic  (float | texture)
roughness (float | texture)   ior       (float, default 1.45)
transmission (float)          emission  (RGB, 强度内含)
normalMap (texture, optional) opacity   (float | baseColor texture alpha)
```

- 实现:Lambert diffuse + GGX 微表面镜面反射(Smith 遮蔽,VNDF 采样)+ 金属 F0 由 baseColor 驱动 + 介质透射(粗糙折射,GGX)。
- 各 lobe 按 Fresnel/能量权重做轮盘选择;MTL → 该模型的映射表见 §6.2。
- 法线贴图:切线空间,切线在加载时按 UV 计算 (mikktspace 算法简化版)。

### 3.4 颜色管理

- 全程线性空间;贴图加载时 sRGB→linear(albedo/emission 类),数据类贴图 (roughness/metallic/normal) 保持线性。
- 输出:曝光 (EV) → ACES filmic tonemap(可切 AgX / 无 tonemap)→ sRGB 编码。
- Rhino 集成时输出可配置为 linear float buffer,由 Rhino 自己的显示管线接管(§9)。

---

## 4. 加速结构 (Accel)

### 4.1 两级 BVH

```
TLAS (instance 层)                    BLAS (mesh 层, 每 mesh 一棵)
┌─────────────────────┐              ┌─────────────────────────┐
│ node: 实例 AABB       │   leaf 指向  │ binned SAH BVH2          │
│ (world space)        │ ──────────▶ │ leaf: 1–4 triangles      │
│ leaf: instanceId     │              │ object space             │
└─────────────────────┘              └─────────────────────────┘
```

- **BLAS**:binned SAH(32 bins),BVH2,leaf 最多 4 三角形;多线程构建(按子树切分)。百万面构建目标 < 300 ms (M1 Pro, 8 线程)。
- **TLAS**:实例数通常 ≪ 面数,直接单线程 SAH 构建,每次实例集变更全量重建(微秒~毫秒级)。
- **增量更新策略**:

| 变更类型 | 动作 |
|---|---|
| 仅实例 transform | 重算 world AABB → TLAS 重建(便宜) |
| mesh 顶点变形 | 该 mesh BLAS 重建(异步,期间可继续渲染旧 BVH) |
| 加/删 mesh 或 instance | BLAS 增删 + TLAS 重建 |
| 仅材质/光源/相机 | 无 BVH 工作,只重置累积 |

### 4.2 GPU 内存布局

所有场景数据为 SSBO(避免依赖 buffer_device_address,最大化 MoltenVK 兼容性):

```
nodesBuffer      : BvhNode[]      // 32 bytes/node, BLAS 们连续存放 + TLAS
trianglesBuffer  : 索引化三角形 (i0,i1,i2, matId)
verticesBuffer   : position[] / normal[] / uv[] / tangent[] (SoA, 各自独立 SSBO)
instancesBuffer  : { worldToObject 3x4, objectToWorld 3x4, blasRootOffset, flags }
materialsBuffer  : MaterialGpu[] (含 texture 索引, -1 表示常量)
lightsBuffer     : LightGpu[] + aliasTable
textures         : sampler2D 数组, 固定上限 256 (见 §10 风险)
envMap + envCdf  : HDRI 与其采样表
```

```c
// BvhNode: 32 bytes, cache-friendly
struct BvhNode {
    vec3 aabbMin;  uint leftOrPrimOffset; // 内部节点=左子索引; 叶=三角形起始
    vec3 aabbMax;  uint primCountAndAxis; // 高位 count(0=内部节点), 低位分裂轴
};
```

- **遍历**:shader 内 short-stack(深度 32 的局部数组 stack),先近子树;TLAS 命中 leaf 后变换光线到 object space 续遍历 BLAS。
- 大 buffer 增长采用 1.5x 扩容 + sub-allocation(VMA),避免每次提交全量重传;脏区段 `vkCmdCopyBuffer` 增量上传。

---

## 5. Vulkan 层 (GpuContext)

| 项 | 决策 | 理由 |
|---|---|---|
| API 版本 | Vulkan 1.2 core | MoltenVK 完整支持;不依赖 1.3 |
| 必需扩展 | 仅 `VK_KHR_portability_subset`(Mac)+ viewer 的 surface 扩展 | 核心库 headless 不需要 swapchain |
| 内存分配 | VulkanMemoryAllocator (VMA) | 工业标准,省 600 行样板 |
| Shader | GLSL 460 → SPIR-V,glslang **构建期离线编译**,SPIR-V 以字节数组嵌入二进制 | 运行时零编译依赖,嵌入 Rhino 时免分发 shader 文件 |
| 同步 | 单 queue,timeline semaphore(MoltenVK 已支持);CPU 读回用 fence | 简单正确优先 |
| 描述符 | 单个大 descriptor set,场景级,布局固定 | 避免 descriptor indexing 的可移植性坑 |
| 校验 | Debug 构建启用 validation layer + `VK_EXT_debug_utils` 命名所有对象 | |

Headless 设计:`mrt_core` 只渲到内部 `outputImage`(`RGBA8` 或 `RGBA32F`),提供两种消费方式 — viewer 拿 `VkImage` 句柄自己 blit 上屏(进程内 C++ 路径);C API 走 `readback`(staging buffer → CPU 指针),Rhino 走后者。

---

## 6. 资产导入 (IO)

### 6.1 OBJ/MTL

- tinyobjloader 解析;按 material 分组拆分为多个 mesh(每 mesh 单材质,简化 GPU 侧)。
- 缺失法线 → 面积加权顶点法线;缺失 UV → (0,0) 且禁用该 mesh 贴图。
- 单位:不做自动缩放,viewer 提供 fit-to-view。

### 6.2 MTL → PBR 映射

| MTL 字段 | PBR 参数 |
|---|---|
| `Kd` / `map_Kd` | baseColor / baseColor 贴图 |
| `Ks` 强度 + `Ns` | roughness ≈ `sqrt(2/(Ns+2))`;Ks 亮度高且 Kd 暗 → metallic 倾向 |
| `Pr`/`Pm`/`Ps` (PBR 扩展) | 直接取 roughness/metallic(优先于上行推断) |
| `map_Pr`/`map_Pm`/`map_bump`/`norm` | roughness/metallic/normal 贴图 |
| `d` / `Tr` | opacity |
| `Tf` + `Ni` | transmission + ior |
| `Ke` / `map_Ke` | emission |

### 6.3 贴图

- stb_image 加载 PNG/JPG/HDR/EXR(EXR 用 tinyexr);加载后生成 mipmap(compute 或 CPU box filter)。
- 全套 PBR 贴图:baseColor (sRGB)、roughness、metallic、normal、emission;HDRI 环境:等距柱状 `.hdr`/`.exr`。

---

## 7. 降噪与后处理

- **OIDN 2.x**,CPU device(Apple Silicon NEON 路径成熟,免 GPU 互操作复杂度)。
- 触发:累积 N 帧(默认 8)且无交互 → 读回 color/albedo/normal 三个 AOV → 后台线程降噪 → 完成后 resolve pass 切换到降噪图,同时累积继续,每累积翻倍帧数 (16/32/64…) 重新降噪一次,直到 spp 上限。
- 任何重置累积的事件立即作废进行中的降噪任务(任务级取消标记)。
- OIDN 以预编译二进制 FetchContent 引入(Apache-2.0,商用安全);不可用时优雅降级为纯累积。

---

## 8. C API 设计 (`mini_raytrace.h`)

原则:不透明句柄 + 错误码;所有结构体 POD、显式 `sizeof` 版本化(`structSize` 首字段);UTF-8 字符串;调用方拥有自己传入的内存,库内部深拷贝。

```c
// ---- lifecycle ----
mrtResult mrtCreateEngine(const mrtEngineDesc* desc, mrtEngine** out);
void      mrtDestroyEngine(mrtEngine*);

// ---- scene: incremental, mirrors Rhino ChangeQueue ----
mrtMeshId     mrtAddMesh(mrtEngine*, const mrtMeshDesc*);     // 顶点/索引/uv/法线
void          mrtUpdateMesh(mrtEngine*, mrtMeshId, const mrtMeshDesc*);
void          mrtRemoveMesh(mrtEngine*, mrtMeshId);
mrtInstanceId mrtAddInstance(mrtEngine*, mrtMeshId, const float xform3x4[12], mrtMaterialId);
void          mrtSetInstanceTransform(mrtEngine*, mrtInstanceId, const float xform3x4[12]);
void          mrtRemoveInstance(mrtEngine*, mrtInstanceId);
mrtMaterialId mrtAddMaterial(mrtEngine*, const mrtMaterialDesc*);  // 贴图以 mrtTextureId 引用
void          mrtUpdateMaterial(mrtEngine*, mrtMaterialId, const mrtMaterialDesc*);
mrtTextureId  mrtAddTexture(mrtEngine*, const mrtTextureDesc*);    // 像素数据 + 格式 + 色彩空间
mrtLightId    mrtAddLight(mrtEngine*, const mrtLightDesc*);        // sun/point/rect
void          mrtSetEnvironment(mrtEngine*, const mrtTextureDesc* hdri, float rotation, float intensity);
void          mrtSetCamera(mrtEngine*, const mrtCameraDesc*);      // pos/target/up/fov 或 4x4
mrtResult     mrtCommitScene(mrtEngine*);   // 应用脏变更, 触发 BVH 构建, 重置累积

// ---- render ----
mrtResult mrtSetRenderSettings(mrtEngine*, const mrtRenderSettings*); // 分辨率/maxBounce/spp上限/曝光/tonemap/denoise
mrtResult mrtRenderFrame(mrtEngine*, mrtFrameInfo* outInfo);          // 阻塞渲一帧, 返回当前 spp 等
mrtResult mrtReadFramebuffer(mrtEngine*, mrtPixelFormat, void* dst, size_t dstSize); // RGBA8_SRGB 或 RGBA32F_LINEAR
void      mrtResetAccumulation(mrtEngine*);

// ---- misc ----
const char* mrtResultToString(mrtResult);
void        mrtSetLogCallback(mrtLogFn, void* user);
```

- Rhino 渲染循环模型:插件线程 `while (running) { 处理 ChangeQueue → mrt*; mrtCommitScene(); mrtRenderFrame(); mrtReadFramebuffer(); SignalRedraw(); }` — API 完全覆盖。
- C++ 调用方 (viewer) 可绕过 readback,直接拿 `mrt::Engine` 的 `VkImage` 上屏(内部头文件,不进 C ABI)。

---

## 9. Rhino 集成方案 (Phase 2 预研,v1 仅设计验证)

- 插件形态:RhinoCommon C# 插件,继承 `Rhino.Render.RealtimeDisplayMode`,注册自定义显示模式;Rhino 8 Mac 上 .NET 7 P/Invoke `libmrt.dylib`(arm64)。
- ChangeQueue → C API 映射:

| ChangeQueue 事件 | C API |
|---|---|
| `ApplyMeshChanges` | `mrtAddMesh` / `mrtUpdateMesh` / `mrtRemoveMesh` |
| `ApplyMeshInstanceChanges` | `mrtAddInstance` / `mrtSetInstanceTransform` / `mrtRemoveInstance` |
| `ApplyRenderSettingsChanges` / Material | `mrtAddMaterial` / `mrtUpdateMaterial`(Rhino PBR 材质字段与 mrtMaterialDesc 接近 1:1) |
| `ApplyViewChange` | `mrtSetCamera`(Rhino 视锥 → pinhole + 偏移) |
| `ApplySunChanges` / `ApplySkylightChanges` | `mrtAddLight(sun)` / `mrtSetEnvironment` |
| `ApplyLightChanges` | `mrtAddLight`(点/矩形;聚光灯 v1 降级为点光) |

- 帧呈现:`mrtReadFramebuffer(RGBA32F_LINEAR)` → `RenderWindow.Channels` 写入,Rhino 负责最终显示色彩管理(我们关闭自身 tonemap)。
- 已知约束:NURBS 由 Rhino 自行网格化后经 ChangeQueue 推送,核心永远只见三角网格 — 与现有 API 一致,无额外工作。

---

## 10. 风险与对策

| # | 风险 | 等级 | 对策 |
|---|---|---|---|
| R1 | MoltenVK 无 VK 光追扩展(已确认,[MoltenVK#1956](https://github.com/KhronosGroup/MoltenVK/issues/1956)) | 已规避 | 全程 compute shader 自研 BVH;架构上 Accel/Trace 接口隔离,v2 可插 Metal RT 后端 |
| R2 | MoltenVK compute 转译性能/正确性坑(threadgroup 内存 32KB 上限、subgroup 行为差异) | 中 | 不用 shared memory 做遍历栈(用局部数组);不依赖 subgroup ops;CI 在真机冒烟 |
| R3 | 贴图数量上限:不用 descriptor indexing,固定 256 槽 sampler 数组 | 低 | 超限报错并复用最后槽;v2 升级 `VK_EXT_descriptor_indexing`(MoltenVK 经 argument buffer 支持)或图集 |
| R4 | 百万面 BVH 重建卡交互(mesh 编辑场景) | 中 | 构建异步化 + 旧 BVH 渲染兜底;transform-only 走 TLAS 重建快路径 |
| R5 | OIDN 在 arm64 mac 的分发(dylib 签名/公证) | 低 | FetchContent 预编译包;运行时 dlopen 失败则降级纯累积 |
| R6 | Rhino Mac 对长时间占用 GPU 的 compute 任务的调度干扰 | 中 | 单帧 dispatch 切 tile(如 1/4 屏一发),避免单条超长 command buffer 触发 GPU watchdog |
| R7 | megakernel 在复杂材质下寄存器压力大、occupancy 低 | 中 | v1 接受;profile 后必要时拆 trace/shade 两 kernel,完整 wavefront 留 v2 |

---

## 11. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| **M0 基建** (週1) | CMake 工程、GpuContext、shader 离线编译管线、GLFW 窗口出三角形/全屏 compute 渐变 | Mac 真机跑通 validation 干净 |
| **M1 首条光线** (週1-2) | OBJ 加载、BLAS/TLAS CPU 构建、GPU 遍历、primary ray 出 normal/albedo 调试视图 | 百万面模型 < 300ms 构建,拖动流畅 |
| **M2 路径追踪** (週2-4) | Disney-lite BSDF、HDRI IS、NEE+MIS、四类光源、累积、RR | 白炉测试通过;Cornell Box 对照 PBRT 参考图 |
| **M3 体验完善** (週4-5) | 全套 PBR 贴图 + mipmap、法线贴图、OIDN、tonemap、viewer 交互打磨 | 静止 2s 内出干净图 |
| **M4 嵌入就绪** (週5-6) | C API 完整实现 + 增量更新压测(随机加删改 mesh)、readback 路径、文档 | C API 集成测试全绿;模拟 ChangeQueue 脚本回放无泄漏 |
| **M5 验证** (週6) | 金图回归集、性能基准、Windows 顺手验证(非阻塞) | 指标达 §1.3 |

> 工期为相对顺序估计,非承诺日期。

---

## 12. 工程规范

- **仓库布局**:

```
MiniRaytrace/
├── CMakeLists.txt
├── core/            # mrt_core 静态库 (无窗口依赖)
│   ├── include/mrt/         # C++ 内部公共头
│   ├── src/{gpu,scene,accel,render,post}/
│   └── shaders/             # GLSL → SPIR-V (构建期)
├── capi/            # mini_raytrace.h + 实现 → libmrt.dylib
├── viewer/          # mrt_viewer 可执行 (GLFW + IO)
├── tests/           # Catch2 单测 + 金图回归
└── docs/            # 本文档及后续设计记录
```

- **依赖**(全部 FetchContent,版本锁定):VMA、GLFW、glslang(仅构建期)、tinyobjloader、stb、tinyexr、OIDN(预编译)、Catch2。Vulkan SDK(含 MoltenVK)为唯一环境前置。
- **代码规范**:C++20;英文注释;RAII 封装一切 Vulkan 对象;核心库禁异常跨 C ABI 边界(边界处 catch → 错误码);clang-format 入库。
- **测试**:数学/BVH 单测;GPU 金图测试(固定 seed + 固定 spp,容差比对);CPU 参考光线求交器交叉验证 BVH 正确性;白炉测试。

---

## 13. 待审阅确认点

1. §3.1 v1 用 megakernel(而非 wavefront)— 接受性能上限换开发速度?
2. §5 shader 离线编译嵌入二进制 — 意味着改 shader 需重编译,开发期会提供 `MRT_DEV_SHADER_RELOAD` 选项从文件热加载,发布版关闭。OK?
3. §8 `mrtRenderFrame` 阻塞式(Rhino 插件自己开渲染线程)— 还是希望核心内置渲染线程 + 回调?当前选择前者,与 Cycles/RhinoCycles 模型一致。
4. §9 Rhino 集成关闭核心 tonemap、输出 linear,由 Rhino 管色彩 — 与你对显示模式的预期一致吗?
5. 里程碑顺序与范围是否有要调整的优先级?
