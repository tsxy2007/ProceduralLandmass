# TerrainLandmassMeshShader.usf — 代码结构分析

## 文件概览

| 属性 | 值 |
|------|-----|
| 文件 | `Plugins/ProceduralLandmass/Shaders/Private/TerrainLandmassMeshShader.usf` |
| 行数 | ~225 |
| Shader 类型 | SF_Mesh + SF_Pixel |
| 平台要求 | SM6 Tier 0 (`PLATFORM_SUPPORTS_MESH_SHADERS_TIER0`) |
| 入口点 | `MainTerrainMeshMS` (Mesh Shader), `MainTerrainPS` (Pixel Shader) |
| C++ 绑定 | `FTerrainMeshShaderMS`, `FTerrainMeshShaderPS` (TerrainMeshShaderMS.h) |
| 集成方式 | `FTerrainMeshSceneViewExtension::PostRenderBasePassDeferred_RenderThread` |
| 光照方式 | PBR (BRDF.ush GGX/Smith/Schlick) 写入 SceneColor，通过 View UFB 读取场景方向光 |

## 代码结构图

```
TerrainLandmassMeshShader.usf (~225 lines)
│
├─ [1-17]    文件头注释 — 架构说明
├─ [19]      #include "/Engine/Public/Platform.ush"
├─ [21]      #if PLATFORM_SUPPORTS_MESH_SHADERS_TIER0
├─ [23]      #include "/Engine/Public/Platform/D3D/D3DCommon.ush"
│
├─ [27-32]   ── Inputs（全局 Shader Parameter 声明）
│            ├─ Texture2D<float> HeightMap        (SRV — PF_R32_FLOAT)
│            ├─ int   GridSize
│            ├─ float HeightScale
│            ├─ float2 WorldOrigin
│            ├─ float4x4 ViewProjectionMatrix
│            └─ float3 CameraWorldPos
│
├─ [36-38]   ── Tile constants
│            ├─ #define TILE_SIZE  8
│            ├─ #define TILE_VERTS 64
│            └─ #define TILE_PRIMS 98
│
├─ [42-48]   ── FTerrainMeshVOut (vertex output struct)
│            ├─ float4 Position : SV_POSITION
│            ├─ float3 WorldPos  : TEXCOORD0
│            ├─ float3 Normal    : NORMAL
│            └─ float2 UV        : TEXCOORD1
│
├─ [54-71]   ── ComputeTerrainNormal(uint2 coord)
│            └─ 中心差分 (4-neighbor) → tangent/bitangent → cross
│
├─ [77-134]  ── Mesh Shader: MainTerrainMeshMS
│            ├─ [numthreads(8,8,1)] + [outputtopology("triangle")]
│            ├─ Thread → Tile local → Global coord mapping
│            ├─ Boundary clamp + SetMeshOutputCounts
│            ├─ Height sample → WorldPos → VP transform
│            ├─ MESH_SHADER_WRITE_VERTEX (64 vertices)
│            └─ MESH_SHADER_WRITE_TRIANGLE (98 triangles)
│
├─ [150]     #include "/Engine/Private/Common.ush"
│            ⚠️ 必须在 BRDF.ush 之前 — 提供 PI, Pow4, Pow5 等定义
│
├─ [151]     #include "/Engine/Private/BRDF.ush"
│
├─ [153-222] ── Pixel Shader: MainTerrainPS → SV_Target0 (SceneColor)
│            ├─ Height-based albedo (green→brown→white gradient)
│            ├─ PBR params: Metallic=0.0, Specular=0.5, Roughness=0.7
│            ├─ float3 N = normalize(Normal)
│            ├─ float3 V = normalize(CameraWorldPos - WorldPos)
│            ├─ float3 L = View.DirectionalLightDirection
│            ├─ BxDFContext + Init() + Diffuse_Lambert()
│            ├─ D_GGX() + Vis_SmithJointApprox() + F_Schlick()
│            ├─ DirectLight = (Diffuse + Specular) * LightColor * NoL
│            ├─ Ambient = albedo * (hemiSky + SkyAtmosphere)
│            └─ Composite: Color = (Direct + Ambient) * IndirectLightingColorScale
│
└─ [225]      #endif // PLATFORM_SUPPORTS_MESH_SHADERS_TIER0
```

## 各阶段详解

### 1. Inputs 声明

HLSL 侧声明了 6 个全局输入（5 个 direct + 1 个来自 View UFB），与 C++ 侧参数一一对应：

| HLSL 声明 | C++ SHADER_PARAMETER | 所属 Struct | 语义 |
|-----------|---------------------|-------------|------|
| `Texture2D<float> HeightMap` | `SHADER_PARAMETER_SRV(Texture2D<float>, HeightMap)` | MS FParameters | 噪声高度图 SRV，PF_R32_FLOAT |
| `int GridSize` | `SHADER_PARAMETER(int32, GridSize)` | MS FParameters | 每边顶点数 |
| `float HeightScale` | `SHADER_PARAMETER(float, HeightScale)` | MS FParameters | 世界空间高度乘数 |
| `float2 WorldOrigin` | `SHADER_PARAMETER(FVector2f, WorldOrigin)` | MS FParameters | 世界空间 XY 偏移 |
| `float4x4 ViewProjectionMatrix` | `SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)` | MS FParameters | 世界→裁剪变换矩阵 |
| `float3 CameraWorldPos` | `SHADER_PARAMETER(FVector3f, CameraWorldPos)` | MS + PS FParameters | 相机世界坐标（计算视线方向） |
| `View.*` (多个字段) | `SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)` | MS + PS FParameters | 方向光、大气散射、间接光缩放 |

### 2. Tile 常量

```hlsl
#define TILE_SIZE  8                                          // 每 Tile 边长
#define TILE_VERTS (TILE_SIZE * TILE_SIZE)                    // 64 顶点
#define TILE_PRIMS ((TILE_SIZE - 1) * (TILE_SIZE - 1) * 2)   // 98 三角形
```

每个线程组处理一个 8×8 的网格 Tile。64 顶点 + 98 三角形，远低于 Mesh Shader Tier 0 上限（256 顶点/256 三角形）。

**Tile → Group 映射**：
```
GridSize = 128 → TilesPerRow = 128/8 = 16
TotalGroups = 16 × 16 = 256

GroupID.x = 0..255
  tileX = GroupID.x % 16
  tileY = GroupID.x / 16
```

### 3. ComputeTerrainNormal — 中心差分法线

对每个顶点采样四邻域高度，通过切线和副切线叉积计算法线：

```hlsl
float hl = HeightMap[left]  * HeightScale;    // 左邻域
float hr = HeightMap[right] * HeightScale;    // 右邻域
float hd = HeightMap[down]  * HeightScale;    // 下邻域
float hu = HeightMap[up]    * HeightScale;    // 上邻域

float3 tangent   = normalize(float3(2.0, hr - hl, 0.0));
float3 bitangent = normalize(float3(0.0, hu - hd, 2.0));

return normalize(cross(tangent, bitangent));
```

texel 间距为 1.0（高度图与顶点 1:1 对应），水平跨度为 2 个世界单位。

### 4. MainTerrainMeshMS — Mesh Shader

核心入口，每组 8×8=64 线程。执行流程：

```
Step 1: Thread → 局部坐标
  lx = GTid % 8,  ly = GTid / 8

Step 2: GroupID → 全局坐标
  tilesPerRow = ceil(GridSize / 8)
  tileX = GID.x % tilesPerRow
  tileY = GID.x / tilesPerRow
  gx = tileX * 8 + lx
  gy = tileY * 8 + ly

Step 3: 越界检测
  if (gx >= GridSize || gy >= GridSize):
      SetMeshOutputCounts(0, 0); return;

Step 4: 高度采样 → 世界坐标
  height = HeightMap[gx, gy]
  halfSize = (GridSize - 1) / 2
  worldPos = (gx - halfSize + Origin.x, gy - halfSize + Origin.y, height * Scale)

Step 5: 顶点写入 (MESH_SHADER_WRITE_VERTEX)
  vtx.Position = mul(worldPos, ViewProjectionMatrix)
  vtx.WorldPos = worldPos, vtx.Normal = ComputeTerrainNormal(...)

Step 6: 三角形索引 (仅 lx<7 && ly<7)
  v00 → v11 → v01  (Triangle 0)
  v11 → v00 → v10  (Triangle 1)
```

### 5. MainTerrainPS — PBR 光照（SceneColor 输出）

像素着色器执行完整的 PBS（Physically-Based Shading），写入 SceneColor RT。

**关键点：View UFB 必须同时绑定到 Mesh Shader 和 Pixel Shader 阶段。**

在 D3D12 中，常量缓冲区是分阶段绑定的——MS 阶段的 `SHADER_PARAMETER_STRUCT_REF(View)` 绑定对 PS 阶段不可见。因此 `FTerrainMeshShaderPS::FParameters` 也必须声明 `View` 和 `CameraWorldPos`，并在 dispatch 时单独绑定。

**光照模型**：

```
Diffuse = Diffuse_Lambert(albedo)
Specular = D_GGX × Vis_SmithJointApprox × F_Schlick(F0)
DirectLight = (Diffuse + Specular) × DirectionalLightColor × saturate(NoL)
Ambient = albedo × (hemisphereSky + AtmosphereLightIlluminance × SkyLuminanceFactor × 0.5)
Final = (DirectLight + Ambient) × IndirectLightingColorScale
```

**高度渐变 Albedo**：
```
lowColor  = (0.15, 0.45, 0.10)  // 深绿 (低地)
midColor  = (0.35, 0.30, 0.20)  // 棕岩 (中山)
highColor = (0.85, 0.80, 0.75)  // 灰白 (积雪)

t1 = saturate(height01 * 2.0)           // low→mid at y=0.5
t2 = saturate((height01 - 0.6) * 2.5)   // mid→high at y=0.6
albedo = lerp(lowColor, midColor, t1)
albedo = lerp(albedo, highColor, t2)
```

**使用的 View UFB 字段**：

| HLSL 访问 | View UFB 字段 | 作用 |
|-----------|--------------|------|
| `View.DirectionalLightDirection` | `FVector4f DirectionalLightDirection` | 主方向光方向 |
| `View.DirectionalLightColor` | `FLinearColor DirectionalLightColor` | 主方向光颜色 |
| `View.SkyAtmospherePresentInScene` | `float SkyAtmospherePresentInScene` | 是否有天空大气 |
| `View.AtmosphereLightIlluminanceOnGroundPostTransmittance[0]` | `FLinearColor[]` | 大气散射地面辐照度 |
| `View.SkyAtmosphereSkyLuminanceFactor` | `FLinearColor SkyAtmosphereSkyLuminanceFactor` | 天空亮度因子 |
| `View.IndirectLightingColorScale` | `FLinearColor IndirectLightingColorScale` | 间接光颜色缩放 |

## C++ 侧架构

### Shader 类（TerrainMeshShaderMS.h）

两个 Global Shader 类，各有独立的 FParameters：

```cpp
// Mesh Shader — 拥有网格参数 + 渲染目标
class FTerrainMeshShaderMS : public FGlobalShader {
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(Texture2D<float>, HeightMap)
        SHADER_PARAMETER(int32, GridSize)
        SHADER_PARAMETER(float, HeightScale)
        SHADER_PARAMETER(FVector2f, WorldOrigin)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FVector3f, CameraWorldPos)       // 相机世界坐标
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        RENDER_TARGET_BINDING_SLOTS()                     // SceneColor + Depth
    END_SHADER_PARAMETER_STRUCT()
};

// Pixel Shader — 需要独立的 View UFB + CameraWorldPos 绑定
class FTerrainMeshShaderPS : public FGlobalShader {
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER(FVector3f, CameraWorldPos)
    END_SHADER_PARAMETER_STRUCT()
};
```

### Scene View Extension（TerrainMeshSceneViewExtension.cpp）

```
PostRenderBasePassDeferred_RenderThread()    ← ISceneViewExtension hook
  │
  ├─ Guard: bRTDataValid, CVar, GRHISupportsMeshShadersTier0
  ├─ RenderTargets.Output[0] → SceneColor RT  (ELoad, 保留已渲染内容)
  ├─ RenderTargets.DepthStencil → SceneDepth  (ELoad)
  ├─ RegisterExternalTexture(HeightmapRHI)     RDG 注册
  ├─ AllocParameters<MS::FParameters>()
  │   ├─ HeightMap SRV, GridSize, HeightScale, WorldOrigin
  │   ├─ ViewProjectionMatrix, CameraWorldPos, View
  │   ├─ RenderTargets[0] = SceneColor
  │   └─ RenderTargets.DepthStencil = SceneDepth
  │
  └─ AddPass(Raster, λ)
      ├─ PSO: CW_RGBA blend, DepthNearOrEqual × Write
      ├─ SetGraphicsPipelineState()             (1 RT enabled)
      ├─ SetShaderParameters(MS, *PassParams)   绑定 MS 参数
      ├─ SetShaderParameters(PS, PSParams)      绑定 PS 参数 ← 关键!
      └─ DispatchMeshShader(NumGroups, 1, 1)
```

### 参数绑定顺序（关键）

```cpp
// 1. MS 参数 — View UFB 绑定到 Mesh Shader 阶段
SetShaderParameters(RHICmdList, MeshShader, MeshShaderRHI, *PassParameters);

// 2. PS 参数 — 必须单独分配 + 绑定
FTerrainMeshShaderPS::FParameters PSParams;
PSParams.View = PassParameters->View;
PSParams.CameraWorldPos = PassParameters->CameraWorldPos;
SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParams);

// 3. 发起 Draw
RHICmdList.DispatchMeshShader(NumGroups, 1, 1);
```

**为什么 PS 需要独立绑定？** D3D12 的 root signature 为每个着色器阶段维护独立的描述符表/常量缓冲区视图。MS 阶段绑定的 `View` constant buffer 不会自动对 PS 可见。UE5 的 SHADER_PARAMETER_STRUCT 系统将参数绑定到特定着色器阶段——MS 的 FParameters 绑定到 SF_Mesh 阶段，PS 的 FParameters 绑定到 SF_Pixel 阶段。

## 为什么要加 Common.ush？

`BRDF.ush` 不是自包含的——它依赖 `Common.ush` 中定义的：

| 符号 | 定义位置 | 用途 |
|------|---------|------|
| `PI` | Common.ush:116 | 半球积分归一化 (`Diffuse_Lambert`, `D_GGX`) |
| `Pow4(x)` | Common.ush:964 | GGX 粗糙度参数 (`a2 = Pow4(Roughness)`) |
| `Pow5(x)` | Common.ush:988 | Schlick Fresnel 的 Fc 项 (`F_Schlick`) |

在标准 UE 渲染管线中，`Common.ush` 通过多层 include 链被拉入（MaterialTemplate.ush → LocalVertexFactory.ush → ...），但自定义 Mesh Shader 插件的 include 链较短，必须手动 include。

## Dispatch 生命周期

```
Actor::BeginPlay()
  └─ SetupMeshShaderViewExt()
      ├─ FSceneViewExtensions::NewExtension<T>(World)    创建 View Extension
      ├─ ViewExt->UpdateHeightmap(HeightRT)               传递 R32F 高度图 RT
      ├─ ViewExt->UpdateParams(GridSize, Scale, Origin)   传递网格参数
      └─ ViewExt->SetEnabled(true)                        激活
           │
           ▼ 每帧 ─────────────────────────────────────
          IsActiveThisFrame_Internal()
           │  检查: bEnabled + bRTDataValid + CVar + RHISupport
           │
           ▼ 渲染线程
          PostRenderBasePassDeferred_RenderThread()
           │  在 Base Pass (GBuffer) 完成后、Deferred Lighting 之前
           │  直接写入 SceneColor，与已渲染几何体混合
           └──────────────────────────────────────────
```

## 激活条件（IsActiveThisFrame_Internal）

每帧检查 5 个条件：

1. `FWorldSceneViewExtension::IsActiveThisFrame_Internal()` — World 有效
2. `bEnabled.load()` — `SetEnabled(true)` 已调用
3. `bRTDataValid` — `UpdateHeightmap()` 成功获取 RHI 纹理
4. `CVarMeshShaderViewExt != 0` — 控制台变量 `r.ProceduralLandmass.MeshShader.Enable`
5. `GRHISupportsMeshShadersTier0` — 平台支持 DX12/Vulkan 1.3

## 与 Approach A (Compute Shader) 对比

| | Approach A: CS → CPU Readback | Approach B: Mesh Shader |
|---|---|---|
| **Shader 类型** | SF_Compute | SF_Mesh + SF_Pixel |
| **输出** | UAV → Buffer → CPU → UStaticMesh | GPU 直写 SceneColor |
| **CPU 开销** | 生成一次性 StaticMesh | 每帧 0 字节 CPU↔GPU 传输 |
| **光照** | 标准 Deferred (StaticMesh) | BRDF.ush PBR + View UFB DirectionalLight |
| **动态更新** | 需重新 Dispatch CS + Readback | 每帧直接从 RT 读取 |
| **编辑器交互** | 正常 (StaticMeshComponent) | 无 (GPU 几何体不可选) |
| **平台要求** | SM5+ | SM6 Tier 0 |
| **适用场景** | 静态地形 | 实时变化地形 / 全 GPU 管线 |

## 文件依赖

```
TerrainLandmassMeshShader.usf
├── /Engine/Public/Platform.ush               (PLATFORM_SUPPORTS_MESH_SHADERS_TIER0)
├── /Engine/Public/Platform/D3D/D3DCommon.ush (MESH_SHADER_* macros)
├── /Engine/Private/Common.ush                (PI, Pow4, Pow5, ...)
└── /Engine/Private/BRDF.ush                  (BxDFContext, D_GGX, Vis_SmithJointApprox, F_Schlick, Diffuse_Lambert)

C++ 侧:
TerrainMeshShaderMS.h         — FTerrainMeshShaderMS + FTerrainMeshShaderPS (GlobalShader 声明)
TerrainMeshShaderMS.cpp       — IMPLEMENT_GLOBAL_SHADER + 编译环境
TerrainMeshGenCS.h            — FTerrainMeshGenParameters (共享参数)
TerrainMeshSceneViewExtension — 每帧注入 PostRenderBasePassDeferred_RenderThread
ProceduralLandmassAsyncActor  — Actor 层集成 PipelineStep_MeshShader()
```
