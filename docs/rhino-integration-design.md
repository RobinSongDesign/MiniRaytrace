# MiniRaytrace × Rhino 8 — 渲染器集成设计文档

| | |
|---|---|
| 版本 | v0.1 (待审阅) |
| 日期 | 2026-07-07 |
| 作者 | Robin + Claude |
| 上游文档 | [PRD.md](../PRD.md) §8 / §9 |
| 基线代码 | `main @ b2bd83c` |
| 状态 | Draft — 审阅通过后按 §8 Issue 列表开工 |

---

## 0. TL;DR

把 MiniRaytrace 核心以 **双入口** 注册进 Rhino 8:

1. **`RenderPlugIn`** — 出现在 Rhino「当前渲染器」列表,`_Render` 产出最终帧(这是"加载为 Rhino 的一个 Renderer"的字面要求);
2. **`RealtimeDisplayMode`** — 视口实时显示模式(对标 Raytraced),PRD 的原始目标。

两个入口共享同一套 **ChangeQueue → C API 翻译层** 与 **RenderSession 渲染线程**。原生侧的关键前置是把 Vulkan 从**装载期硬依赖**改为**运行时动态加载**(volk):Windows 上用系统 loader(显卡驱动自带),macOS 上**直载插件目录里的 libMoltenVK.dylib、完全绕过 loader/ICD**。全部工作拆为 6 个 Epic、29 个 issue(§8),映射到 6 个里程碑(§9)。

---

## 1. 目标与范围

### 1.1 目标 (P0)

1. MiniRaytrace 在 Rhino 8 中注册为**可选渲染器**(渲染菜单 → 当前渲染器)并提供**实时视口显示模式**。
2. 场景同步走 `ChangeQueue`:网格/实例/材质/贴图/灯光/太阳/天光/环境/相机全量 + 增量,编辑操作 <100ms 反馈。
3. 帧呈现:核心输出 linear RGBA32F → `RenderWindow` RGBA 通道,色彩管理交给 Rhino(核心 `tonemap = LINEAR`,PRD §9 既定决策)。
4. 平台:Rhino 8 **Windows (x64)** 与 **Rhino 8 Mac (Apple Silicon)**,以 yak 包分发,目标机器**零环境前置**(不装 Vulkan SDK)。
5. 无 Vulkan 能力的机器上**优雅失败**:插件可装载、给出人话提示,绝不拖垮 Rhino。

### 1.2 非目标 (v1)

- AOV 输出到渲染窗口(depth/normal 通道)— v2。
- ClippingPlane 剖切(核心 shader 不支持)— 检测到时提示,v2 再做。
- 材质高级 lobe(clearcoat/sheen/anisotropy)— 映射时丢弃并记录。
- 多文档并发渲染优化、GPU 设备热切换。
- Rhino 7 兼容(ChangeQueue API 差异大,不做)。

### 1.3 参考实现

- [RhinoCycles](https://github.com/mcneel/RhinoCycles)(McNeel 官方,开源)— ChangeQueue 用法、单位换算、HUD 的事实标准,拿不准的 API 语义以它为准绳。
- [rhino-developer-samples / RealtimeViewportIntegration](https://github.com/mcneel/rhino-developer-samples) — RealtimeDisplayMode 最小骨架。

---

## 2. 现状盘点(基线 b2bd83c)

### 2.1 已具备

| 能力 | 位置 | 状态 |
|---|---|---|
| C ABI 全套增量场景 API | `capi/include/mini_raytrace.h` | ✅ 覆盖 PRD §9 映射表所需的绝大部分 |
| `RGBA32F_LINEAR` 回读 + `TONEMAP_LINEAR` | `mrtReadFramebuffer` / `mrtRenderSettings.tonemap` | ✅ Rhino 呈现路径的两个前提都在 |
| headless 引擎(无窗口依赖) | `core/` | ✅ |
| macOS 自包含打包经验 | `tools/package_app.sh`(viewer .app) | ✅ loader+MoltenVK+ICD 三件套已跑通,可移植经验到插件 |

### 2.2 差距清单(逐条落到 §8 的 issue)

| # | 差距 | 证据 | 对应 issue |
|---|---|---|---|
| G1 | C ABI 无 `mrtCommitScene`(PRD §8 承诺了;现状提交隐式发生在 `mrtRenderFrame`) | `mini_raytrace.h` 无此符号;`Engine::renderFrame` 注释 "Sync dirty scene state, trace one frame" | A1 |
| G2 | 相机只有 look-at 针孔模型,无平行投影、无非对称视锥 — Rhino 视口平移后视锥不对称,还有平行/两点透视 | `SceneTypes.hpp:65` `CameraDesc{pos,target,up,fovY}` | A2 |
| G3 | 无 `mrtRemoveMaterial` / `mrtRemoveTexture` — Rhino 全天会话下材质编辑会耗尽 256 纹理槽 | `mini_raytrace.h:162-168` | A3 |
| G4 | `mrt_core` 装载期硬链接 Vulkan loader — 无 Vulkan 的 Windows 机器 P/Invoke 直接 `DllNotFoundException`;Mac 没有系统 loader | `core/CMakeLists.txt:41` `Vulkan::Vulkan` | A4 |
| G5 | 从未在 Windows/MSVC 构建过 | 仓库只有 macOS 构建产物 | A5 |
| G6 | 整帧单 dispatch,与 Rhino 自身 GPU 绘制共享设备时有 watchdog / UI 卡顿风险 | PRD R6 已识别,未实现 | A6 |
| G7 | 回读的 alpha 语义、行序、性能未定义/未测 | `readFramebuffer` 无相关文档与基准 | A7 |
| G8 | `mrtEngineDesc` 无 GPU 选择(Windows 双显卡需优先独显) | `mini_raytrace.h:70` | A4(并入) |

---

## 3. 集成形态:如何"成为 Rhino 的一个 Renderer"

Rhino 里"渲染器"有两个正交的注册点,本设计**两个都做**,因为它们共享 95% 的实现:

| 注册点 | RhinoCommon 类型 | 用户看到什么 | 用途 |
|---|---|---|---|
| 当前渲染器 | `Rhino.PlugIns.RenderPlugIn` 派生 + 插件 GUID | 渲染菜单 → 当前渲染器 → **MiniRaytrace**;`_Render` 弹渲染窗口 | 最终帧(modal,`RenderPipeline` 驱动) |
| 实时显示模式 | `Rhino.Render.RealtimeDisplayMode` 派生 + `RealtimeDisplayModeClassInfo`(固定 GUID) | 视口显示模式下拉 → **MiniRaytrace**(与 Raytraced 并列) | 视口实时预览(主体验) |

要点:

- `RenderPlugIn` 是插件本体(一个插件类只能有一个);`RealtimeDisplayModeClassInfo` 派生类由 Rhino 在插件装载时扫描注册,GUID 一旦发布**永不更改**(用户显示模式设置按 GUID 持久化)。
- 两个入口都不直接碰 C API,统一经 **RenderSession**(§4.2)。
- `_Render`(production)与视口(realtime)的差异仅是:分辨率来源(文档渲染设置 vs 视口尺寸)、终止条件(渲到 sppLimit vs 永续交互)、呈现目标(modal RenderWindow vs 视口 RenderWindow)。

---

## 4. 插件架构

### 4.1 组件图

```
Rhino 8 进程
┌────────────────────────────────────────────────────────────────┐
│ RhinoCommon (.NET 7)                                           │
│  MiniRaytrace.Rhino.rhp                                        │
│  ┌───────────────────┐  ┌──────────────────────────────────┐  │
│  │ MrtRenderPlugIn   │  │ MrtRealtimeDisplayMode           │  │
│  │ : RenderPlugIn    │  │ : RealtimeDisplayMode (+ClassInfo)│  │
│  │  _Render → modal  │  │  StartRenderer/Shutdown/Resize    │  │
│  └────────┬──────────┘  └───────────────┬──────────────────┘  │
│           │      每会话一个               │                      │
│           ▼                             ▼                      │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │ RenderSession(渲染线程,唯一碰 C API 的线程)               │ │
│  │   loop: Flush(ChangeQueue) → 翻译 → mrtCommitScene        │ │
│  │         → mrtRenderFrame → mrtReadFramebuffer             │ │
│  │         → RenderWindow.RGBA ← SignalRedraw                │ │
│  │  ┌────────────────┐   ┌────────────────────────────────┐ │ │
│  │  │ MrtChangeQueue │   │ 翻译器: Mesh/Material/Texture/  │ │ │
│  │  │ : ChangeQueue  │   │ Light/Sun/Env/Camera + 句柄映射 │ │ │
│  │  └────────────────┘   └────────────────────────────────┘ │ │
│  └───────────────┬──────────────────────────────────────────┘ │
│                  │ P/Invoke (Interop/NativeMethods, SafeHandle)│
├──────────────────┼─────────────────────────────────────────────┤
│ native           ▼                                             │
│  mrt.dll / libmrt.dylib  ── volk 运行时加载 ──▶                 │
│    Windows: 系统 vulkan-1.dll(显卡驱动自带)                     │
│    macOS:   同目录 libMoltenVK.dylib 直载(免 loader/ICD)        │
│  (可选) OpenImageDenoise 动态库,dlopen 探测,缺失则降级          │
└────────────────────────────────────────────────────────────────┘
```

### 4.2 线程模型(关键设计)

C API 约定所有调用来自单线程(PRD §2.2)。利用 `ChangeQueue.Flush()` 的语义 — **Apply\* 回调在调用 Flush 的线程上同步执行** — 把一切收敛到渲染线程,零跨线程封送:

```
Rhino 主线程                     渲染线程 (RenderSession)
────────────                    ─────────────────────────
文档变更 → NotifyBeginUpdates ─▶ changesPending = true (仅置标志)
                                loop {
                                  if (changesPending) queue.Flush();
                                    // Apply* 在本线程执行 → 直接调 mrt*
                                  mrtCommitScene();
                                  mrtRenderFrame(&info);
                                  mrtReadFramebuffer(RGBA32F, buf);
                                  RenderWindow 写入 + SignalRedraw();
                                  HUD 状态更新(spp/denoised);
                                }
StartRenderer / Shutdown ──────▶ 生命周期命令(start/pause/stop)
OnRenderSizeChanged ───────────▶ resize 命令(下轮循环应用)
```

- 每个活跃会话(一个开启了 MiniRaytrace 模式的视口,或一次 `_Render`)= 一个 `RenderSession` = 一个 `mrtEngine` = 一条渲染线程。
- **每会话独立 VkDevice**:多视口同开时显存翻倍。v1 接受(见 §11 待确认 Q4);共享 `GpuContext` 列 v2。
- 关停:排空命令 → `mrtDestroyEngine` → join,带超时强杀,保证 Rhino 退出永不挂起。
- 设备丢失(`MRT_ERROR_DEVICE_LOST`):v1 置会话错误态 + HUD 提示"渲染设备已丢失,请切换显示模式重试";自动重建列 v2。

### 4.3 仓库新增布局

```
MiniRaytrace/
├── rhino/
│   ├── MiniRaytrace.Rhino/          # net7.0 → MiniRaytrace.Rhino.rhp
│   │   ├── MrtRenderPlugIn.cs
│   │   ├── Realtime/                # DisplayMode + ClassInfo + HUD
│   │   ├── ChangeQueue/             # MrtChangeQueue + 各翻译器 + 句柄映射
│   │   ├── Session/RenderSession.cs
│   │   └── Interop/                 # NativeMethods, MrtEngine(SafeHandle)
│   ├── MiniRaytrace.Smoke/          # 无 Rhino 的 console 冒烟(纯 P/Invoke)
│   └── dist/                        # manifest.yml + yak 打包脚本
└── docs/rhino-integration-design.md # 本文档
```

- csproj 目标 **net7.0 单一目标**。Rhino 8 Windows 默认 .NET (core) runtime;用户强制 `/netfx` 模式时本插件不装载,启动检测并提示(见 §11 Q3)。
- RhinoCommon 以 NuGet 引用(锁 8.x 区间),`ExcludeAssets=runtime`。

---

## 5. 场景翻译设计(ChangeQueue → C API)

### 5.1 句柄映射

Rhino 侧标识 → 核心句柄的双向表,由 `MrtChangeQueue` 持有,生命周期与会话一致:

| Rhino 标识 | 核心句柄 | 说明 |
|---|---|---|
| `(Guid meshId, int meshIndex)` | `mrtMeshId` | ChangeQueue 的一个 Mesh 按材质拆成多个 sub-mesh,`meshIndex` 即拆分下标 — 与核心"每 mesh 单材质"模型天然对齐 |
| `uint instanceId`(MeshInstance) | `mrtInstanceId` | block 嵌套已被 ChangeQueue 展平,直接映射 |
| `uint materialId` + `RenderHash` | `mrtMaterialId` | 按 RenderHash 去重;hash 变更 → `mrtUpdateMaterial` |
| `RenderTexture.RenderHash` | `mrtTextureId` | 烘焙缓存键(§5.3) |
| Rhino 灯光 Guid | `mrtLightId` | |

### 5.2 事件映射细则

| ChangeQueue 事件 | 翻译动作 | 备注 |
|---|---|---|
| `CreateWorld` | 全量:先材质/贴图,再 mesh,再 instance,再灯光/环境/相机 | 打开文档、切入显示模式时 |
| `ApplyMeshChanges` | deleted Guid → 该 Guid 全部 sub-mesh `mrtRemoveMesh`;added → 三角化(quad 拆两三角)、提法线/UV → `mrtAddMesh`/`mrtUpdateMesh` | 顶点数据在托管侧拼 SoA float[],pin 后传入(库内深拷贝,调用返回即可释放) |
| `ApplyMeshInstanceChanges` | `mrtAddInstance(meshId, xform3x4, materialId)` / `mrtRemoveInstance`;Rhino `Transform`(行主 4×4)取前三行 → 3x4 | 坐标系**直通**:核心不关心 up 轴,全场景保持 Rhino 世界坐标(Z-up),由相机/太阳方向保证一致性 — 零转换 |
| 材质(经 `MaterialFromId`) | §5.3 | |
| `ApplyLightChanges` | point→POINT;rectangular→RECT;directional→SUN(angularRadius≈0);spot→点光降级(日志);linear→细长 RECT 降级 | 强度换算参考 RhinoCycles 系数(W → W/sr / radiance) |
| `ApplySunChanges` | `mrtAddLight(SUN)`,方向取 Rhino sun 向量,物理太阳 angularRadius | |
| `ApplySkylightChanges` | 天光强度 → `mrtSetEnvironment` 的 intensity 因子 | |
| `ApplyEnvironmentChanges` | equirect 位图直传 `mrtSetEnvironment(RGBA32F)`;程序化环境(渐变/物理天空)烘焙 1024×512;纯色背景 → 1×1 常量 env | Usage: Background + Skylight,v1 不区分反射通道 |
| `ApplyViewChange` | `ViewportInfo.GetFrustum` + 相机位置/方向/up → `mrtCameraDescEx`(§5.4) | |
| `ApplyRenderSettingsChanges` | spp 上限/尺寸等 → `mrtSetRenderSettings` | |
| `ApplyGroundPlaneChanges` | 合成大矩形 mesh(尺寸=场景包围盒×10)+ 映射其材质 | D8 |
| `ApplyClippingPlaneChanges` | 不支持:HUD 警告一次 | v2 shader 平面裁剪 |

### 5.3 材质与贴图

- 路径:`MaterialFromId` → `RenderMaterial` → **PhysicallyBased** 直映射(baseColor/roughness/metallic/opacity/ior/transmission/emission ↔ `mrtMaterialDesc` 几乎 1:1);非 PBR 材质走 `SimulateMaterial` 兜底。
- 丢弃字段(clearcoat/sheen/anisotropy/subsurface)每材质记一次日志。
- 贴图:位图贴图直取像素;**程序纹理用 TextureEvaluator 烘焙**(默认 1024²,可配),按 `IsLinear` 分类 sRGB/linear 入 `mrtTextureFormat`;缓存按 RenderHash 去重。
- 仅支持 UV 映射;WCS/OCS/box 映射 v1 降级为常量色 + 警告一次。
- bump 贴图 v1 忽略(只支持 normal map),决策记录在 D4。

### 5.4 相机(依赖 C ABI 扩展,issue A2)

Rhino 视口 = 可能不对称的视锥(平移产生偏移)+ 三种投影(透视/平行/两点透视)。现有 `mrtCameraDesc` 表达不了,新增:

```c
typedef struct mrtCameraDescEx {
    size_t  structSize;
    int32_t projection;            /* 0 = perspective, 1 = orthographic      */
    float   position[3];
    float   forward[3], up[3];     /* 正交归一,two-point 由非对称视锥自然表达  */
    float   left, right, bottom, top;  /* 视锥切片: persp 在 dist=1 处;ortho 为绝对尺寸 */
} mrtCameraDescEx;
MRT_API void mrtSetCameraEx(mrtEngine*, const mrtCameraDescEx*);
```

与 `ViewportInfo.GetFrustum(out l, out r, out b, out t, out near, out far)` 一一对应(除以 near 归一到 dist=1)。旧 `mrtSetCamera` 保留为便捷封装。验收口径:MiniRaytrace 视口与线框叠加偏差 <1px。

### 5.5 呈现

- 渲染线程:`mrtReadFramebuffer(RGBA32F_LINEAR)` → `RenderWindow.OpenChannel(StandardChannels.RGBA).SetValues` → `SignalRedraw()`。
- `tonemap = MRT_TONEMAP_LINEAR`、曝光 EV=0,色彩管理完全交 Rhino(PRD §9 决策不变)。
- 节流:spp < 32 时逐帧呈现;之后降到 ~15Hz(收敛后期帧间差异小,省回读带宽)。
- alpha 语义 v1 恒为 1(A7);透明背景列 v2。

---

## 6. Vulkan 运行时打包(重点)

### 6.1 问题定义

`mrt_core` 现在 `target_link_libraries(... Vulkan::Vulkan)`(`core/CMakeLists.txt:41`)→ 产出的 `libmrt` 对 Vulkan loader 有**装载期**依赖:

- **Windows**:目标机器若无 `vulkan-1.dll`(显卡驱动自带,但远程桌面/VM/旧集显机器可能没有),P/Invoke 第一次调用即抛 `DllNotFoundException`,用户看到的是一条天书。
- **macOS**:系统根本没有 Vulkan loader,不自带就必然装载失败。而 viewer 的 .app 打包方案(loader + ICD json + 环境变量)依赖 bundle 结构,**插件目录不是 bundle**,且在宿主进程(Rhino)里设进程级 `VK_DRIVER_FILES` 环境变量既脆弱又有污染宿主之嫌。

### 6.2 方案:volk 运行时加载(issue A4),按平台分流

核心改为 volk(`VK_NO_PROTOTYPES`)运行时解析全部 Vulkan 入口,`libmrt` 自身**零 Vulkan 装载期依赖**。加载顺序:

```
mrtCreateEngine
  └─ 1. 平台默认 loader:LoadLibrary("vulkan-1.dll") / dlopen("libvulkan.1.dylib")
  └─ 2. (macOS) 失败则:dladdr 定位 libmrt 自身目录 → dlopen("<同目录>/libMoltenVK.dylib")
        MoltenVK 是完整 Vulkan 1.2 实现,直接导出 vkGetInstanceProcAddr — 无 loader、
        无 ICD json、无环境变量
  └─ 3. 全部失败 → MRT_ERROR_VULKAN_INIT + mrtGetLastErrorMessage() 人话诊断
```

### 6.3 各平台策略与决策矩阵

**Windows(结论:不带任何 Vulkan 二进制)**

| 选项 | 评估 |
|---|---|
| ✅ 用系统 loader(驱动自带),volk 动态加载,失败出人话 | 2016 年后的 N/A/I 驱动都带 loader;覆盖率最高、零体积 |
| ❌ 打包 SDK 的 vulkan-1.dll 进插件目录 | 会 shadow 系统 loader,旧 loader + 新驱动 ICD 的兼容矩阵是给自己埋雷;loader 应归驱动管 |
| ❌ 附带 CPU Vulkan(SwiftShader/lavapipe)兜底 | 路径追踪在 CPU 实现上无实用性能,+30MB 只为渲一张 0.1fps 的图,不如明确报错 |

无 Vulkan 时的 UX:插件正常装载,切入 MiniRaytrace 模式 → HUD/对话框:"需要支持 Vulkan 1.2 的显卡驱动(NVIDIA/AMD/Intel 2016+)"。另:`mrtEngineDesc` 增加 GPU 偏好(优先独显),解决双显卡本默认选核显的问题(并入 A4)。

**macOS(结论:release 直载 MoltenVK,dev 走 loader)**

| 选项 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| P1 loader + ICD json + 进程内 setenv | validation layer 可用(还需另带 VVL dylib) | 宿主进程设环境变量;ICD json 内路径与装载顺序脆弱;文件多 | 仅 dev(viewer 已用) |
| **P2 直载 libMoltenVK.dylib(volk custom)** | 单一 dylib、零配置、零环境变量;路径由 dladdr 自定位 | 绕过 loader 即无 layer 机制(release 无所谓) | **✅ release 默认** |
| P3 MoltenVK 静态链进 libmrt | 分发单文件 | MoltenVK 升级需重编;+~15MB;宿主进程符号冲突风险 | 后备,不排期 |

MoltenVK 版本锁定自构建机 Vulkan SDK,版本号记进插件 About;升级 MoltenVK = 换一个 dylib 重新打包。

### 6.4 分发包布局(yak)

Rhino 官方分发通道是 yak 包(`_PackageManager`),按平台各出一个:

```
mini-raytrace-0.2.0-rh8_0-win.yak            mini-raytrace-0.2.0-rh8_0-mac.yak
├── manifest.yml                              ├── manifest.yml
├── MiniRaytrace.Rhino.rhp                    ├── MiniRaytrace.Rhino.rhp
└── runtimes/win-x64/native/                  └── runtimes/osx-arm64/native/
    ├── mrt.dll                                   ├── libmrt.dylib
    ├── OpenImageDenoise*.dll   (F4, 可选)         ├── libMoltenVK.dylib
    └── tbb12.dll               (OIDN 依赖)        └── libOpenImageDenoise*.dylib (F4, 可选)
```

- C# 侧 `NativeLibrary.SetDllImportResolver` 按 RID 从 `runtimes/<rid>/native/` 绝对路径加载 `mrt`,不依赖 PATH/工作目录(issue B2)。
- OIDN 是 dlopen 弱依赖(PRD R5),缺失自动降级纯累积 — 因此可做成可选内容。

### 6.5 签名与公证(macOS)

yak 从网络安装,dylib 落盘带 quarantine 属性;未签名的原生库会被 Gatekeeper 拦截。流水线(issue F3):

1. `codesign`(Developer ID Application,带 timestamp)逐个签 `libmrt.dylib` / `libMoltenVK.dylib` / OIDN dylib;
2. 打 zip 提交 `notarytool` 公证(裸 dylib 无法 staple,依赖 Gatekeeper 在线校验票据);
3. 公证通过后再打 yak。

⚠️ 风险注记:yak 内 dylib 的 Gatekeeper 实际行为(尤其离线机器)需在干净真机验证 — 列入 F3 验收。Windows 侧 Authenticode 签名可选,不阻塞。

---

## 7. 生产渲染(`_Render`)设计要点

- `MrtRenderPlugIn.Render()` → `RenderPipeline` 派生:创建离屏 `RenderSession`(分辨率取文档渲染设置),ChangeQueue 一次 `CreateWorld` + Flush(modal 渲染不跟增量),循环 `mrtRenderFrame` 直到 sppLimit / 用户取消,每 N 帧写 RenderWindow + 报进度。
- 渲染设置页(spp 上限、maxBounces、denoise、曝光)以自定义面板挂进 Rhino 渲染设置,随 3dm 持久化(issue E5)。
- v1 只出 RGBA 通道;AOV(normal/depth/albedo)列 v2。

---

## 8. Issue 分解

> 使用方式:每条 issue 直接可建(标题 = `[Epic-编号] 名称`);**规模**:S ≤ 1 天,M = 2–3 天,L ≈ 1 周。建议标签:`epic:core` / `epic:plugin` / `epic:packaging`、`platform:win` / `platform:mac`、`blocked-by:*`。

### Epic A — 核心 / C ABI 补齐(native)

---

**A1 · C ABI 增加 `mrtCommitScene` 与显式提交语义** — 规模 S,依赖 无

- 现状:提交隐式发生在 `mrtRenderFrame`(`Engine::renderFrame` 内 sync);PRD §8 承诺了显式 `mrtCommitScene`。
- 内容:新增 `MRT_API mrtResult mrtCommitScene(mrtEngine*)`;`mrtRenderFrame` 保留隐式提交(commit 幂等);返回提交统计(重建 BLAS 数、耗时 ms)供插件日志;文档化"哪些调用置脏、何时必须 commit"。
- 验收:capi 单测 — 改场景→commit→render 与 改场景→render 金图一致;commit 后累积归零;连续两次 commit 第二次为 no-op。

**A2 · 相机模型扩展:平行投影 + 非对称视锥(`mrtCameraDescEx`)** — 规模 M,依赖 无

- 现状:`CameraDesc` 仅 pos/target/up/fovY(`core/include/mrt/SceneTypes.hpp:65`),无法表达 Rhino 的平移偏心视锥、平行投影、两点透视。
- 内容:按 §5.4 定义 `mrtCameraDescEx` + `mrtSetCameraEx`;ray-gen shader 按 {left,right,bottom,top} 插值出光线(persp:方向插值;ortho:原点插值、方向恒 forward);旧 API 改为便捷封装;C++ 层 `CameraDesc` 同步扩展。
- 验收:金图 — 对称视锥下新旧 API 逐像素一致;非对称视锥 = 大对称图裁剪(容差内);ortho 场景光线平行(特征测试);为 D7 的 <1px 叠加验收提供基础。

**A3 · 材质 / 纹理资源回收 API** — 规模 M,依赖 无

- 现状:C ABI 无 `mrtRemoveMaterial` / `mrtRemoveTexture`(`capi/include/mini_raytrace.h:162-168`);256 纹理槽(PRD R3)在全天 Rhino 会话的材质编辑下必然耗尽。
- 内容:新增两个 remove API;纹理槽 free-list 复用;定义"材质仍被实例引用时 remove"的语义(建议:引用计数 + 延迟回收,决策记录入代码注释);`mrtUpdateMaterial` 换贴图时旧纹理引用递减。
- 验收:压测 — 循环 add/remove 纹理 1 万次,槽位数与内存水位平稳;validation layer 干净;被引用材质 remove 后渲染不崩(fallback 材质)。

**A4 · Vulkan 运行时动态加载(volk)+ 优雅失败 + GPU 选择** — 规模 M,依赖 无(F1/F2 的前置)

- 现状:`core/CMakeLists.txt:41` 硬链接 `Vulkan::Vulkan`,见 §6.1。
- 内容:引入 volk(FetchContent,`VK_NO_PROTOTYPES`);实现 §6.2 的三级加载顺序(平台 loader → mac 同目录 MoltenVK 直载 → 失败);VMA 改用 volk 函数表;新增 `mrtGetLastErrorMessage()`;`mrtEngineDesc` 增加 `gpuPreference`(0=auto 优先独显 / 1=指定 index),设备枚举日志化。
- 验收:隐藏 loader 的机器上 `mrtCreateEngine` 返回 `MRT_ERROR_VULKAN_INIT` 且诊断可读;mac 上卸掉 Vulkan SDK、仅同目录放 MoltenVK 仍可渲染;viewer 全回归;`otool -L libmrt.dylib` / `dumpbin /imports mrt.dll` 无 Vulkan 依赖。

**A5 · Windows (MSVC) 构建 + 双平台 CI** — 规模 M,依赖 无

- 内容:修 MSVC 编译(导出宏已有 `_WIN32` 分支,预计小改);GitHub Actions:macOS(arm64 构建 + 单测)、Windows(MSVC 构建 + 单测;可选 lavapipe headless 1spp 冒烟);产物 artifact 上传 `mrt.dll` / `libmrt.dylib` 供插件 CI 消费。
- 验收:双平台 CI 绿;Windows 真机(任一 Vulkan 独显)headless 渲染与 mac 金图容差内一致。

**A6 · 单帧分 tile dispatch(宿主共享 GPU / watchdog)** — 规模 M,依赖 无(PRD R6 落地)

- 内容:`renderFrame` 内按 tile 拆分 dispatch + submit(初版固定 4 片,预留每次提交 GPU 时间预算参数);确保 Rhino 自身 UI 绘制(同一 GPU)不被单条长 command buffer 饿死。
- 验收:1080p 重场景下 Rhino 视口自身操作(旋转其他视口)无可感卡顿(真机手测记录);金图不变;viewer 帧率回归 <5%。

**A7 · RGBA32F 回读:alpha 语义、行序、性能基准** — 规模 S,依赖 无

- 内容:文档化并实现 — alpha 恒 1(v1)、行序 top-down(与 RenderWindow 期望一致,避免 C# 侧翻转);staging buffer 常驻复用(禁止每帧分配);基准测试:1080p RGBA32F 回读 < 3ms。
- 验收:基准数字入 tests 并 CI 跟踪;E1 消费端无翻转代码、无撕裂。

### Epic B — 插件骨架与互操作

---

**B1 · C# 插件工程骨架(MiniRaytrace.Rhino)** — 规模 S,依赖 无

- 内容:`rhino/` 目录 + net7.0 csproj(RhinoCommon NuGet 锁 8.x,`ExcludeAssets=runtime`);`MrtRenderPlugIn : RenderPlugIn` + 固定插件 GUID + AssemblyInfo;构建产出 .rhp;dev 装载文档(拖入 / `_PlugInManager`);启动时检测 .NET runtime 模式,netfx 下弹提示不装载。
- 验收:Rhino 8 Win + Mac 均能装载并出现在插件管理器;`_Render` 提示"渲染管线尚未就绪"而非崩溃。

**B2 · P/Invoke 绑定层 + 原生库定位 + console 冒烟** — 规模 M,依赖 A4、A5

- 内容:`NativeMethods` 覆盖 `mini_raytrace.h` 全部条目(blittable struct 镜像、`LibraryImport` source generator);`NativeLibrary.SetDllImportResolver` 按 RID 从 `runtimes/<rid>/native/` 绝对路径加载;`MrtEngine : SafeHandle` 封装生命周期;log 回调 → `RhinoApp.WriteLine`(**委托保活防 GC**);`MiniRaytrace.Smoke` console 工程(无 Rhino 依赖):create → 硬编码三角形 → render 32spp → readback → PNG。
- 验收:冒烟双平台出非黑帧且和 viewer 同场景一致;删库 / 无 Vulkan 场景下异常与提示可读;struct 布局有 `Marshal.SizeOf` 断言测试(与 native `sizeof` 比对,防漂移)。

**B3 · RenderSession:渲染线程与生命周期** — 规模 M,依赖 B2

- 内容:实现 §4.2 全部 — 每会话一线程一 engine;`Flush` 在渲染线程调用(单线程约束的落实点);start/pause/resume/resize/shutdown 命令;shutdown 排空 + join 带超时;`MRT_ERROR_DEVICE_LOST` → 错误态;帧回调(spp/denoised/frameMs)给 HUD。
- 验收:压力测试 — 反复开关会话 50 次,托管与 native 内存水位平稳(Instruments/dotMemory 记录);Rhino 正常退出无挂起线程;pause 后 GPU 占用归零(活动监视器验证)。

### Epic C — Renderer 注册

---

**C1 · RenderPlugIn:进入"当前渲染器"列表 + 最小 `_Render`** — 规模 M,依赖 B3(完整验收另依赖 D2/D7)

- 内容:`Render()` override → `RenderPipeline` 派生按 §7 实现;两步走 — 第一步(RP1)渲测试渐变图打通 modal 管线,第二步(RP3 后)接真实场景;进度上报 + 取消。
- 验收(第一步):渲染菜单出现 MiniRaytrace 且可设为当前渲染器;`_Render` 弹窗出测试图、可取消。验收(第二步):800×600 真实场景渲到 sppLimit、可另存 PNG。

**C2 · RealtimeDisplayMode 注册与生命周期** — 规模 M,依赖 B3

- 内容:`MrtRealtimeDisplayMode : RealtimeDisplayMode` + `RealtimeDisplayModeClassInfo`(名称 "MiniRaytrace",**GUID 固定并写注释警告永不更改**);`StartRenderer(w,h,doc,view,viewport,forCapture,renderWindow)` → 创建 RenderSession;`OnRenderSizeChanged` → resize 命令;`ShutdownRenderer` → 会话关停;首版写测试渐变图进 `RenderWindow.RGBA` 验证呈现链。
- 验收:视口显示模式下拉出现 MiniRaytrace;切入出测试图、拉伸视口尺寸跟随、切回 Shaded 干净退出(B3 压测复用此路径)。

**C3 · HUD 与状态呈现** — 规模 S,依赖 C2

- 内容:`HudProductName` / `HudShowMaxPasses` / `HudMaximumPasses=sppLimit` / `HudLastRenderedPass=spp`;暂停/锁定按钮 → session pause;denoised 状态文本;错误态文案(无 Vulkan / 设备丢失,§6.3 UX)。
- 验收:HUD 数字随渲染推进;点暂停 GPU 占用立刻归零;拔掉 Vulkan(改名 loader)后 HUD 显示人话错误。

### Epic D — ChangeQueue 场景翻译

---

**D1 · MrtChangeQueue 骨架 + 句柄映射表** — 规模 M,依赖 B3

- 内容:`ChangeQueue` 派生(docSerial + ViewInfo 构造);§5.1 五张映射表及增删改一致性;`NotifyBeginUpdates/EndUpdates` → changesPending 标志;`CreateWorld` 全量装载顺序(材质→mesh→instance→灯光→环境→相机);会话结束清理。
- 验收:映射表单测(纯托管,mock 事件序列);打开文档 / 新建文档 / `_Undo` 各触发一次正确的全量或增量;日志可开关(诊断模式打印每个事件)。

**D2 · 网格与实例同步** — 规模 L,依赖 D1

- 内容:§5.2 的 Mesh/MeshInstance 行:sub-mesh 按材质拆分对齐核心模型;三角化(quad→2 tri)、法线/UV 提取为 SoA float[];`Transform` 4×4 → 行主 3x4;增量 add/update/delete 全路径;坐标系直通(Z-up 世界坐标,零转换,决策见 §5.2)。
- 验收:打开 20 万面模型几何正确(与 Shaded 轮廓叠加比对);移动/复制/删除/`_Undo` 视口 <100ms 跟随;`_ExplodeBlock`/嵌套 block 正确;大模型(100 万面)`CreateWorld` 不超 2× 模型内存峰值。

**D3 · 材质翻译(Rhino PBR → Disney-lite)** — 规模 M,依赖 D1;热更新回收依赖 A3

- 内容:§5.3 映射;PhysicallyBased 直映射 + `SimulateMaterial` 兜底;丢弃字段一次性日志;RenderHash 去重与热更新(`mrtUpdateMaterial`)。
- 验收:样例材质集(白漆/拉丝金属/玻璃/发光/磨砂塑料)与 Raytraced 同场景目视可比(截图存 docs);改基色 <100ms 生效;默认材质(无指定)渲染合理灰。

**D4 · 纹理烘焙管线** — 规模 L,依赖 D3

- 内容:位图直取像素;程序纹理 `TextureEvaluator` 烘焙(默认 1024²,设置可调);`IsLinear` → sRGB/linear 格式分类;RenderHash 缓存;WCS/OCS/box 映射降级常量色 + 警告一次;bump 忽略(仅 normal map,决策记录);贴图 tiling/offset 变换烘进像素或传 UV 变换(v1:烘进像素)。
- 验收:baseColor+roughness+normal 贴图场景对照 Raytraced;程序纹理(noise/gradient)烘焙结果正确;256 槽超限行为符合 PRD R3(报错 + 复用末槽);缓存命中率日志。

**D5 · 灯光翻译与单位换算** — 规模 M,依赖 D1

- 内容:§5.2 灯光行(point/rect/directional/spot 降级/linear 降级);强度换算系数对照 RhinoCycles 实现并记录推导;开关/删除/修改全路径。
- 验收:点/矩形/方向光与 Raytraced 同曝光下亮度目视一致(±0.5 EV,截图存档);spot 降级有日志;关灯即时生效。

**D6 · Sun / Skylight / Environment** — 规模 M,依赖 D1、D4

- 内容:§5.2 对应三行;sun 方向与强度(物理太阳 angularRadius);skylight 强度因子;equirect 位图直传 / 程序化环境烘焙 1024×512 / 纯色 1×1;环境旋转。
- 验收:_Sun 面板改时间视口日照跟随;HDRI 环境旋转/强度正确;关 skylight 变暗幅度与 Raytraced 一致;纯色背景正确。

**D7 · 相机同步** — 规模 M,依赖 D1、A2

- 内容:`ApplyViewChange` → `GetFrustum` + 相机三元组 → `mrtSetCameraEx`;透视/平行/两点透视三模式;与 `OnRenderSizeChanged` 的宽高比联动(避免 resize 瞬间畸变);wallpaper 忽略(日志一次)。
- 验收:**与线框叠加 <1px**(三种投影各验);拖动/缩放/`_Zoom Selected` 无漂移;命名视图切换正确。

**D8 · GroundPlane / ClippingPlane 策略** — 规模 S,依赖 D2

- 内容:GroundPlane → 大矩形 mesh(场景包围盒×10,随包围盒增长重建)+ 其材质映射;ClippingPlane 检测 → HUD 警告一次 + 文档记录 v2 方案(shader 平面裁剪)。
- 验收:开 GroundPlane 有地面接触阴影;含剖切面文档不崩、有提示。

### Epic E — 呈现与体验

---

**E1 · 帧呈现:readback → RenderWindow** — 规模 M,依赖 B3、C2、A7

- 内容:§5.5 全部 — RGBA32F 直写 RGBA 通道 + `SignalRedraw` 节流(spp<32 逐帧,之后 ~15Hz);resize 时 RenderWindow 尺寸同步;色彩验证(Rhino gamma 管线 vs viewer ACES,截图对照记录)。
- 验收:1080p 呈现路径(readback+写通道)<5ms/帧;无撕裂、无翻转;`_ScreenCaptureToFile` 所见即所得。

**E2 · 交互期降分辨率** — 规模 S,依赖 E1、D7

- 内容:相机变更事件密集期切 1/2 分辨率(`mrtSetRenderSettings` resize + reset),静止 200ms 恢复全分辨率;HUD 显示 "interactive";升采样呈现(RenderWindow 尺寸不变,像素放大)。
- 验收:M1 Pro / 中端 Win 独显上 1080p 视口拖动 ≥24 fps(对齐 PRD §1.3);恢复瞬间无闪黑。

**E3 · OIDN 在 Rhino 会话内启用与调优** — 规模 M,依赖 E1;分发依赖 F4

- 内容:`MRT_ENABLE_OIDN` 构建开启;验证核心 §7 降噪节奏(8/16/32… 重降噪)在插件循环下工作;交互重置正确作废任务(无 stale 帧闪现);denoised 状态入 HUD;设置开关接 E5 面板。
- 验收:静止 2s 内出干净图(PRD §1.3);快速拖动-停止循环 20 次无旧降噪帧闪现;OIDN 文件缺失时自动降级且 HUD 注明。

**E4 · 视口捕获(ViewCaptureToFile / 打印)** — 规模 S,依赖 C2、E1

- 内容:`StartRenderer(forCapture=true)` 路径:同步渲到目标分辨率与目标 spp(默认 128,可配)再返回;`-ViewCaptureToFile` 与打印预览走通。
- 验收:2× 视口分辨率捕获无裁切、无低 spp 噪点;捕获期间 UI 有进度反馈。

**E5 · 生产渲染完善 + 渲染设置 UI** — 规模 M,依赖 C1、D 全部

- 内容:`RenderPipeline` 进度/取消打磨;结果窗口保存 PNG/EXR;渲染设置自定义面板(spp 上限 / maxBounces / denoise / 曝光)随 3dm 持久化;`_Render` 用文档分辨率。
- 验收:渲→取消→改设置→再渲→保存全流程;设置随文件保存复open恢复;批渲染(`_-Render`)脚本化可用。

### Epic F — 打包分发

---

**F1 · Windows 打包(yak)** — 规模 M,依赖 A4、A5、B2

- 内容:csproj 按 §6.4 布局把 `mrt.dll`(+可选 OIDN)复制进输出;`manifest.yml` + `yak build` 产 `*-rh8_0-win.yak`;本地 `_PackageManager` 安装验证;无 Vulkan 机器的 UX 走查(§6.3)。
- 验收:干净 Win11 + Rhino 8 评估版,仅从 yak 安装即完整可用;无 Vulkan VM 中提示人话、Rhino 稳定。

**F2 · macOS 打包(yak)+ MoltenVK 直载落地** — 规模 M,依赖 A4、B2

- 内容:`libmrt.dylib` + `libMoltenVK.dylib`(版本锁定并记入 About)入 `runtimes/osx-arm64/native/`;验证 A4 的 dladdr 直载路径在插件目录布局下工作;dev 文档保留 loader+ICD 方案(validation 用);`*-rh8_0-mac.yak`。
- 验收:无 Vulkan SDK 的干净 mac + Rhino 8 从 yak 安装即用;`otool -L libmrt.dylib` 无 loader 依赖;viewer 打包(`tools/package_app.sh`)不回归。

**F3 · 签名 / 公证 + Release CI** — 规模 M,依赖 F1、F2

- 内容:§6.5 mac 签名公证流水线;Win Authenticode(可选);GitHub Actions release 工作流:tag → 双平台构建 → 签名 → 双 yak 产物挂 Release;版本号单源(CMake `project(VERSION)` → csproj → manifest)。
- 验收:从 GitHub Release 下载的 mac yak 在带 Gatekeeper 的**干净真机**(含一次离线场景)安装加载无拦截 — 此条是 §6.5 风险的验证点;CI 一键出可发布产物。

**F4 · OIDN 二进制随包分发** — 规模 S,依赖 F1、F2、E3

- 内容:OIDN 2.x 官方预编译(win x64:主库+device_cpu+tbb;mac arm64:dylib 三件套)入 native 目录并进签名清单;体积评估(~40MB/平台)后决策"默认进包 vs 拆可选包"(§11 Q6);dlopen 探测已有(PRD R5),接线即可。
- 验收:双平台降噪可用;手工删除 OIDN 文件后渲染正常、HUD 注明"降噪不可用"。

---

## 9. 里程碑

| 里程碑 | 内容 | Issues | 验收口径 |
|---|---|---|---|
| **RP0 互操作打通** (1–2 周) | volk 化、Win 构建、插件骨架、P/Invoke | A4 A5 B1 B2 | console 冒烟双平台出图;插件双平台可装载 |
| **RP1 视口出图** (1 周) | 渲染线程、双注册、HUD | B3 C2 C3 C1(第一步) A1 | MiniRaytrace 出现在显示模式与渲染器列表,视口/`_Render` 出测试图 |
| **RP2 几何与相机** (2 周) | ChangeQueue 骨架、mesh/instance、相机、呈现 | D1 D2 D7 A2 E1 | 打开真实模型,视口与线框 <1px 对齐,编辑即时跟随 |
| **RP3 材质与光照** (2–3 周) | 材质/贴图/灯光/环境 | D3 D4 D5 D6 A3 | 样例场景与 Raytraced 目视对标(截图存档) |
| **RP4 体验完善** (1–2 周) | 交互降采样、降噪、捕获、生产渲染 | E2 E3 E4 E5 C1(第二步) D8 A6 A7 | 达 PRD §1.3 指标(拖动 ≥24fps、静止 2s 出净图) |
| **RP5 打包发布** (1 周) | yak、签名、Release CI | F1 F2 F3 F4 | 干净双平台机器从 yak 安装即用 |

> 与 PRD §11 的关系:RP0 可与核心 M4/M5 并行;RP2 起需要 M4(C API 完整实现)交付质量。

---

## 10. 风险

| # | 风险 | 等级 | 对策 |
|---|---|---|---|
| RH1 | RealtimeDisplayMode/ChangeQueue 官方文档稀薄,本文引用的 API 签名可能与 Rhino 8 实际有出入 | 中 | RP1 骨架期以 RhinoCycles 源码逐一校准;发现出入回改本文档,不硬编码绕行 |
| RH2 | yak 内裸 dylib 的公证/Gatekeeper 行为(不能 staple)在离线机器上未知 | 中 | F3 验收强制干净真机 + 离线场景;兜底方案:改发 pkg 安装器 |
| RH3 | Rhino 8 Mac 自身 Metal 显示管线与 MoltenVK 同进程共享 GPU 的调度冲突 | 中 | A6 tile dispatch 先行;RP1 起每个里程碑在 mac 真机走查 Rhino UI 流畅度 |
| RH4 | 大场景 `CreateWorld` 峰值内存(Rhino mesh + 托管拷贝 + native 深拷贝三份并存) | 中 | D2 验收含内存口径;分批 add + 及时释放托管中间数组 |
| RH5 | P/Invoke 回调(log)被 GC 回收 → 崩溃 | 低 | B2 明确委托保活;code review 清单项 |
| RH6 | Rhino 8 Win 用户切 netfx 模式导致插件不装载 | 低 | B1 启动检测 + 提示;发布说明写明 |
| RH7 | 双显卡 Windows 本默认枚举到核显,性能被误判 | 低 | A4 的 gpuPreference 默认优先独显 + HUD 显示设备名 |

---

## 11. 待审阅确认点

1. **双入口范围**:v1 同时做 RenderPlugIn(`_Render`)+ RealtimeDisplayMode,还是先只做显示模式、`_Render` 放 v1.1?本文按双入口排期(`_Render` 完整验收压在 RP4),砍掉可省 ~1 周。
2. **Windows 优先级**:PRD 定 mac 首要,但 Rhino 用户大盘在 Windows。本文把 Win 构建放进 RP0(A5)、真机验证从 RP2 开始 — 认可这个"双平台同行"的节奏,还是 mac 先行、Win 全部推后?
3. **.NET 7 单目标**(不支持 Rhino 8 Win 的 netfx 运行时模式)— 接受?多目标 net48 会让 B2 的库加载逻辑复杂一档。
4. **每视口一个 Engine**(独立 VkDevice):两个视口同开 MiniRaytrace 显存翻倍。v1 是否限制同时仅一个视口可用该模式(其余显示占位提示)?本文默认不限制。
5. **降级清单**:spot→点光、linear→细长矩形、bump 忽略、WCS 映射→常量色、ClippingPlane 不支持 — 每项都有一次性警告。有必须升级为 v1 硬需求的吗?
6. **OIDN 40MB/平台**:默认进 yak(安装即有降噪)vs 拆可选包(包体小)。本文倾向默认进包。
7. **MoltenVK 直载(§6.3 P2)**:release 放弃 loader/validation 换取零配置分发 — 认可?dev 期仍有 loader 路径可用。
