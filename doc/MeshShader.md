# FTerrainMeshSceneViewExtension — Mesh Shader 地形渲染

## 概述

`FTerrainMeshSceneViewExtension` 通过 SM6 Mesh Shader 将地形几何体直接注入 Unreal 的延迟渲染管线（`PostRenderBasePassDeferred`）。无需 CPU Mesh、无需 Buffer 回读、无需 `UStaticMesh`——每帧在 GPU 上从高度 RT 实时生成地形并写入 GBuffer。

**平台要求**: SM6 Tier 0（DX12 / Vulkan 1.3），`GRHISupportsMeshShadersTier0 == true`。

## 架构

```
┌─ Game Thread ─────────────────────┐   ┌─ Render Thread (每帧) ──────────────────┐
│                                   │   │                                         │
│  ViewExt->UpdateHeightmap(RT) ────┼──►│  PostRenderBasePassDeferred             │
│  ViewExt->UpdateParams(...) ──────┼──►│    │                                    │
│  ViewExt->SetEnabled(true) ───────┼──►│    ├─ 读 PersistentHeightRT (R32F)      │
│                                   │   │    ├─ 拿 View.ViewProjectionMatrix       │
│  FSceneViewExtensions             │   │    ├─ RDG Raster Pass:                  │
│    ::NewExtension<T>(World)       │   │    │   ├─ MeshShader (8×8 tiles)        │
│                                   │   │    │   │   ├─ 64 verts, 98 tris/group  │
│                                   │   │    │   │   └─ Central-diff normals      │
│                                   │   │    │   └─ PixelShader                   │
│                                   │   │    │       ├─ NdotL diffuse             │
│                                   │   │    │       └─ Height-based albedo       │
│                                   │   │    └─ RT: SceneColor + SceneDepth       │
│                                   │   │                                         │
└───────────────────────────────────┘   └─────────────────────────────────────────┘
```

## 手动集成

### 1. 创建并注册

```cpp
#include "TerrainMeshSceneViewExtension.h"

TSharedPtr<FTerrainMeshSceneViewExtension> ViewExt;

void AMyActor::BeginPlay()
{
    Super::BeginPlay();

    // 注册到当前 World 的场景渲染管线
    ViewExt = FSceneViewExtensions::NewExtension<FTerrainMeshSceneViewExtension>(GetWorld());
}
```

### 2. 准备高度 RenderTarget

ViewExtension 需要一个 `PF_R32_FLOAT` 格式的 RenderTarget，每个像素存储一个 [0,1] 的高度值：

```cpp
void AMyActor::SetupHeightRT()
{
    HeightRT = NewObject<UTextureRenderTarget2D>(this);
    HeightRT->RenderTargetFormat = RTF_R32f;
    HeightRT->InitAutoFormat(GridSize, GridSize);  // e.g. 128×128
    HeightRT->UpdateResourceImmediate(true);
}
```

往 RT 写入内容（例如使用 Noise Compute Shader）：

```cpp
FProceduralLandmassNoiseCSParameters NoiseParams(GridSize, Scale, Octaves,
    Persistence, Lacunarity, Seed, Offset);
NoiseParams.RenderTarget = HeightRT->GameThread_GetRenderTargetResource();
FProceduralLandmassNoiseCSInterface::Dispatch(NoiseParams);
FlushRenderingCommands();  // 确保 GPU 写完
```

### 3. 更新 ViewExtension

```cpp
// 传入高度 RT（ViewExtension 内部用 FTextureRHIRef 引用计数，RT 可以被 GC 回收）
ViewExt->UpdateHeightmap(HeightRT);

// 传入网格参数
ViewExt->UpdateParams(/*GridSize*/ 128, /*HeightScale*/ 1000.0f, /*WorldOrigin*/ FVector2f::ZeroVector);

// 激活每帧渲染
ViewExt->SetEnabled(true);
```

### 4. 开启 CVar

ViewExtension 受到 CVar 全局开关保护，默认关闭：

```
r.ProceduralLandmass.MeshShader.Enable 1
```

| 值 | 行为 |
|----|------|
| `0` | 关闭（默认） |
| `1` | 开启 + 日志摘要 |
| `2` | 详细日志（每次 dispatch 都打印参数） |

### 5. 清理

`TSharedPtr` 出作用域后自动取消注册。手动释放：

```cpp
void AMyActor::EndPlay(const EEndPlayReason::Type Reason)
{
    if (ViewExt.IsValid())
    {
        ViewExt->SetEnabled(false);
        ViewExt.Reset();  // 从 FSceneViewExtensions 注销
    }
    Super::EndPlay(Reason);
}
```

## 通过 AProceduralLandmassAsyncActor 使用（已集成）

在编辑器中选中 Actor，Details 面板：

| Group | Property | Value |
|-------|----------|-------|
| **GPU** | Mesh Shader Rendering | `true` |
| Noise | ChunkSize | `128` |
| Noise | HeightScale | `1000` |
| Noise | Scale / Octaves / Seed | 按需设置 |

控制台: `r.ProceduralLandmass.MeshShader.Enable 1`

Actor 自动：
1. 生成 Noise RT
2. 注册 ViewExtension
3. 更新高度图和参数
4. 销毁时清理 ViewExtension

## 激活条件

`IsActiveThisFrame_Internal` 在以下情况返回 `false`（跳过当前帧）：

```cpp
// 1. World 已失效
if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context)) return false;

// 2. 未通过 SetEnabled(true) 启用
if (!bEnabled) return false;

// 3. 未调用过 UpdateHeightmap（无有效数据）
if (!bRTDataValid) return false;

// 4. CVar 全局关闭
if (r.ProceduralLandmass.MeshShader.Enable == 0) return false;

// 5. 平台不支持 Mesh Shader
if (!GRHISupportsMeshShadersTier0) return false;
```

## API 参考

```cpp
class FTerrainMeshSceneViewExtension : public FWorldSceneViewExtension
{
public:
    // 构造。通过 FSceneViewExtensions::NewExtension<T>(World) 调用。
    FTerrainMeshSceneViewExtension(const FAutoRegister& AutoReg, UWorld* InWorld);

    // ── Game Thread API ────────────────────────────────────────────────

    /** 启用/禁用每帧渲染。 */
    void SetEnabled(bool bInEnabled);

    /**
     * 更新高度 RenderTarget。
     * @param RT  PF_R32_FLOAT 格式，GridSize×GridSize 尺寸。
     *            内部提取 FTextureRHIRef 引用计数，调用后 RT 可被 GC。
     *            传 nullptr 清除数据并停用。
     */
    void UpdateHeightmap(UTextureRenderTarget2D* RT);

    /**
     * 更新网格参数。
     * @param InGridSize    每边顶点数（总顶点数 = GridSize²）
     * @param InHeightScale 世界空间高度缩放
     * @param InWorldOrigin 世界空间 XY 偏移
     */
    void UpdateParams(int32 InGridSize, float InHeightScale, FVector2f InWorldOrigin);

    // ── ISceneViewExtension Overrides ───────────────────────────────────

    void SetupViewFamily(FSceneViewFamily&) override {}
    void SetupView(FSceneViewFamily&, FSceneView&) override {}
    void BeginRenderViewFamily(FSceneViewFamily&) override {}

    /** 核心：每帧在 Base Pass 之后注入 Mesh Shader 绘制。 */
    void PostRenderBasePassDeferred_RenderThread(
        FRDGBuilder& GraphBuilder,
        FSceneView& InView,
        const FRenderTargetBindingSlots& RenderTargets,
        TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

    int32 GetPriority() const override { return 0; }
};
```

## 相关文件

| 文件 | 作用 |
|------|------|
| `TerrainMeshSceneViewExtension.h/.cpp` | ViewExtension 实现 |
| `TerrainMeshShaderMS.h/.cpp` | Mesh Shader + Pixel Shader C++ 绑定 |
| `TerrainLandmassMeshShader.usf` | HLSL: `MainTerrainMeshMS` + `MainTerrainPS` |
| `ProceduralLandmassAsyncActor.h/.cpp` | Actor 集成示例 |
| `TerrainMeshGenCS.h/.cpp` | Approach A: Compute Shader 方案（对比） |

## 限制

- **SM6 only** — DX11 不可用，`IsActiveThisFrame` 自动返回 false
- **单个 RT** — ViewExtension 只有一个高度 RT，多个地形需要多个 Extension 实例
- **ViewProjectionMatrix** — 自动从当前 `FSceneView` 获取，无需手动设置
- **Mesh Shader 输出上限** — 每组 256 顶点 + 256 三角形以内。8×8 tile 产生 64 顶点 + 98 三角形，安全

## 两种 GPU 方案对比

| | Approach A: Compute Shader | Approach B: Mesh Shader |
|---|---|---|
| Shader | `TerrainMeshGenCS.usf` (SF_Compute) | `TerrainLandmassMeshShader.usf` (SF_Mesh + SF_Pixel) |
| 输出 | GPU Buffers → CPU 回读 → UStaticMesh | 直接光栅化到 SceneColor |
| CPU Mesh | 有 | 无 |
| 每帧 GPU 开销 | 无（生成后是 StaticMesh） | 每帧重新生成几何体 |
| 平台 | SM5+ | SM6 Tier 0 (DX12/Vulkan 1.3) |
| 编辑/选中 | 正常（StaticMeshComponent） | 无（GPU 程序化几何体） |
| 启用属性 | `bUseGPUMeshGeneration` | `bUseMeshShaderRendering` |
