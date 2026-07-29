# ProceduralLandmass 插件分析文档

## 1. 概述

**ProceduralLandmass** 是一个 UE5 Runtime 插件，用于程序化生成地形网格。核心功能包括：

- **噪声地图生成**：CPU（fBm Perlin 噪声）和 GPU（Compute Shader）双路径
- **地形网格构建**：从噪声数据生成带多级 LOD 的 `UStaticMesh`
- **地形着色**：根据高度区间分配颜色，生成彩色纹理
- **世界网格系统**：九宫格（3×3）空间划分，支持世界坐标与网格坐标互转

| 属性 | 值 |
|------|-----|
| 模块名 | `ProceduralLandmass` |
| 类型 | Runtime |
| 加载阶段 | `PostConfigInit`（提前加载以注册 Shader 目录映射） |
| 依赖模块 | Core, CoreUObject, Engine, Slate, SlateCore, Renderer, RenderCore, RHI, MeshDescription, StaticMeshDescription |
| 源码文件数 | 7 个头文件 + 7 个源文件 + 1 个 .usf Shader |

---

## 2. 文件清单

```
Plugins/ProceduralLandmass/
├── ProceduralLandmass.uplugin
├── Resources/
│   └── Icon128.png
├── Shaders/Private/
│   └── ProceduralLandmassNoise.usf          # GPU Compute Shader (HLSL)
└── Source/ProceduralLandmass/
    ├── ProceduralLandmass.Build.cs
    ├── Public/
    │   ├── ProceduralLandmass.h             # 模块入口
    │   ├── ProceduralLandmassBPLibrary.h    # BP 函数库 + 数据结构
    │   ├── ProceduralLandmassActor.h         # 同步地形 Actor
    │   ├── ProceduralLandmassAsyncActor.h    # 异步地形 Actor（Pipeline 模式）
    │   ├── ProceduralLandmassNoiseCS_AsyncExecution.h  # GPU CS 接口 + 异步 BP 节点
    │   ├── GridUtils.h                      # 九宫格工具
    │   └── TerrainMeshData.h               # 网格数据容器
    └── Private/
        ├── ProceduralLandmass.cpp
        ├── ProceduralLandmassBPLibrary.cpp
        ├── ProceduralLandmassActor.cpp
        ├── ProceduralLandmassAsyncActor.cpp
        ├── ProceduralLandmassNoiseCS_AsyncExecution.cpp
        ├── GridUtils.cpp
        └── TerrainMeshData.cpp
```

---

## 3. 架构总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        ProceduralLandmass Plugin                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌──────────────────────┐    ┌──────────────────────────────┐          │
│  │ FProceduralLandmass  │    │ UProceduralLandmassBPLibrary │          │
│  │ Module               │    │ (BlueprintFunctionLibrary)    │          │
│  │ ▶ 注册 Shader 目录    │    │ ▶ GenerateNoiseMap (CPU fBm)  │          │
│  │   映射 /Procedural-   │    │ ▶ GenerateNoiseTexture        │          │
│  │   Landmass            │    │ ▶ GenerateColorNoiseTexture   │          │
│  └──────────────────────┘    │ ▶ GenerateTerrainMesh (LOD)   │          │
│                              │ ▶ Async variants (delegate)   │          │
│                              └──────────┬───────────────────┘          │
│                                         │                               │
│              ┌──────────────────────────┼──────────────────────┐       │
│              │                          │                       │      │
│              ▼                          ▼                       ▼      │
│  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────┐ │
│  │ AProceduralLandmass  │  │ AProceduralLandmass  │  │ FProcedural  │ │
│  │ Actor                │  │ AsyncActor           │  │ LandmassNoise│ │
│  │ (同步，CallInEditor)  │  │ (异步 Pipeline+Ticket)│  │ CSInterface  │ │
│  │                      │  │                      │  │ (GPU 路径)   │ │
│  │ GenerateTerrain():    │  │ GenerateTerrainAsync │  │ ▶ Dispatch   │ │
│  │  1.NoiseMap(CPU)      │  │  1.NoiseMapAsync(BG) │  │ ▶ Readback   │ │
│  │  2.TerrainMesh        │  │  2.TerrainMesh(GT)  │  │   (debug)    │ │
│  │  3.CreateMesh+Mat     │  │  3.ApplyMaterial(GT) │  └──────────────┘ │
│  └──────────────────────┘  └──────────────────────┘                   │
│                                                                         │
│  ┌──────────────────────┐    ┌──────────────────────┐                  │
│  │ UTerrainMeshData     │    │ UGridUtils           │                  │
│  │ (UObject)            │    │ (BP Function Library) │                  │
│  │ ▶ FLODMeshData[]     │    │ ▶ WorldToGrid        │                  │
│  │ ▶ CreateMesh()       │    │ ▶ GridToWorld        │                  │
│  │ ▶ AddTriangleToLOD   │    │ ▶ GetNineGridCells   │                  │
│  └──────────────────────┘    │ ▶ GridDistance       │                  │
│                              │ ▶ IsAdjacent         │                  │
│                              └──────────────────────┘                  │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 4. 模块入口

### FProceduralLandmassModule

**文件**: `ProceduralLandmass.h` / `ProceduralLandmass.cpp`

```cpp
void FProceduralLandmassModule::StartupModule()
{
    // 将 "/ProceduralLandmass" 虚拟路径映射到物理 Shaders/Private 目录
    AddShaderSourceDirectoryMapping("/ProceduralLandmass", ShaderDirectory);
}
```

- 在 `PostConfigInit` 阶段加载，确保 Shader 路径在其他模块请求 Shader 编译前注册
- 这使得 `.usf` 文件可以通过 `#include "/ProceduralLandmass/ProceduralLandmassNoise.usf"` 或 `IMPLEMENT_GLOBAL_SHADER` 的虚拟路径引用

---

## 5. 核心数据结构

### 5.1 FProceduralLandmassNoiseCSParameters (struct)

GPU Compute Shader 调度参数，镜像 `GenerateNoiseMap` 的参数：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ChunkSize` | `int` | 256 | 每边采样数 |
| `Scale` | `float` | 1.0 | 世界空间缩放 |
| `Octaves` | `int` | 4 | fBm 倍频数 |
| `Persistence` | `float` | 0.5 | 振幅衰减系数 |
| `Lacunarity` | `float` | 2.0 | 频率增长系数 |
| `Seed` | `int` | 0 | 随机种子 |
| `Offset` | `FVector2D` | (0,0) | 世界空间偏移 |
| `RenderTarget` | `FRenderTarget*` | nullptr | 输出目标（PF_R32_FLOAT） |

### 5.2 FTerrainType (USTRUCT, BlueprintType)

地形区域定义：

| 字段 | 类型 | 说明 |
|------|------|------|
| `Name` | `FString` | 区域名称 |
| `NoiseHeight` | `float` | 高度阈值（低于此值使用此颜色） |
| `Color` | `FLinearColor` | 区域颜色 |

### 5.3 FLODMeshData (USTRUCT, BlueprintType)

单级 LOD 的网格数据：

| 字段 | 类型 | 说明 |
|------|------|------|
| `Vertices` | `TArray<FVector>` | 顶点位置 |
| `UVs` | `TArray<FVector2D>` | UV 坐标 |
| `Triangles` | `TArray<int32>` | 三角形索引 |
| `ChunkSize` | `int32` | 该 LOD 级别的边长顶点数 |

### 5.4 UTerrainMeshData (UCLASS)

网格数据容器，管理多级 LOD 并提供 `CreateMesh()` 方法：

- `Init(ChunkSize, LODLevels)` — 初始化所有 LOD 级别的数组大小
- `BeginLOD(LODIndex, LodSize)` — 获取指定 LOD 的可写引用，预分配内存
- `AddTriangleToLOD(LOD, a, b, c)` — 向 LOD 添加三角形
- `CreateMesh()` — 构建 `UStaticMesh`：调用 `BuildFromMeshDescriptions` 生成多 LOD 网格，并设置 `CTF_UseComplexAsSimple` 碰撞

### 5.5 FGridCoordinate / FNineGridResult

九宫格系统的坐标类型。`FGridCoordinate` 支持 `GetTypeHash`，可作为 `TMap` 键。

`FNineGridResult` 包含中心格子坐标、全部 9 个格子坐标、及各自的世界坐标中心点。

### 5.6 ENoiseDrawMode (UENUM)

```
ENDM_Noise  — 仅噪声（灰度）
ENDM_Color  — 噪声 + 颜色映射
ENDM_Mesh   — 完整网格
```

---

## 6. CPU 噪声生成路径

### 6.1 GenerateNoiseMap — CPU fBm Perlin 噪声

**文件**: `ProceduralLandmassBPLibrary.cpp:19-83`

#### 算法流程

```
输入参数 (ChunkSize, Scale, Octaves, Persistence, Lacunarity, Seed, Offset)
│
├─ 1. 确定性偏移量生成
│     FMath::RandInit(Seed)
│     for i in [0, Octaves):
│       octaveOffsets[i] = RandRange(-100000, 100000)
│
├─ 2. 理论振幅范围预计算
│     theoreticalMax = Σ(persistence^i)  for i in [0, Octaves)
│     （确保相邻 Chunk 在边界处数值一致）
│
├─ 3. 逐像素 fBm 采样
│     for y, x in [0, ChunkSize):
│       sample = (x - halfSize + Offset) / Scale * frequency + octaveOffset
│       perlinValue = FMath::PerlinNoise2D(sample)   // [-1, 1]
│       noiseHeight += perlinValue * amplitude
│       amplitude *= Persistence  // 衰减
│       frequency *= Lacunarity   // 增长
│
└─ 4. 归一化: [-theoreticalMax, theoreticalMax] → [0, 1]
      normalizedValue = (noiseHeight / theoreticalMax + 1) / 2
```

**关键设计决策**：使用**理论最大振幅**（Σ persistence^i）而非实际采样范围做归一化。这保证了相邻 Chunk 在共享边界上产生**完全一致**的噪声值，对于无限地形的无缝拼接至关重要。

### 6.2 GenerateNoiseTexture — 灰度纹理

将 `TArray<float>` 噪声图转为 `UTexture2D`：
- 创建 Transient 纹理 (`CreateTransient`)
- `PF_R32_FLOAT` 的每个像素值做 `Lerp(Black, White, noiseValue)` → `FColor`

### 6.3 GenerateColorNoiseTexture — 着色纹理

根据 `TerrainTypes` 数组将噪声高度映射为颜色：
- 遍历 `TerrainTypes`，第一个匹配 `noiseValue <= TerrainType.NoiseHeight` 的颜色被采用
- 无匹配时回退为 `FColor::Black`

### 6.4 GenerateTerrainMesh — 多 LOD 地形网格

```
for each LODIndex in [0, LODLevels):
  Step = 2^LODIndex
  LodSize = ((ChunkSize-1) / Step) + 1      // 该 LOD 的顶点密度

  for y, x in [0, LodSize):
    采样坐标 = (x*Step, y*Step)               // 在原始噪声图中采样
    noiseValue = NoiseMap[SampleY * ChunkSize + SampleX]
    height = HeightCurve ? Curve->GetFloatValue(noiseValue) * HeightScale
                         : noiseValue * HeightScale
    顶点 = (x + topLeftX, topLeftY - y, height)
    UV = (x/LodSize, y/LodSize)

    // 构建三角形带
    if 非最右列且非最下行:
      AddTriangle(v, v+LodSize+1, v+LodSize)
      AddTriangle(v+LodSize+1, v, v+1)
```

**LOD 策略**：Step-based 降采样。LOD0 = 全精度（Step=1），LOD1 = 每隔一个顶点（Step=2），以此类推。

### 6.5 异步变体

```cpp
GenerateNoiseMapAsync  → Async(EAsyncExecution::Thread)  → 回调到 GameThread
GenerateNoiseTextureAsync → 同步（纹理创建必须在 GameThread）
GenerateTerrainMeshAsync  → 同步（UObject 创建必须在 GameThread）
```

只有 `GenerateNoiseMapAsync` 真正在后台线程执行（纯计算，无 UObject 依赖）。纹理和网格的"异步"版本仅是 delegate 封装，实际工作在 GameThread 完成。

---

## 7. GPU Compute Shader 路径

### 7.1 Shader 文件: `ProceduralLandmassNoise.usf`

与 CPU 版 `GenerateNoiseMap` 功能完全等价，但有差异：

| 方面 | CPU | GPU |
|------|-----|-----|
| Perlin 实现 | `FMath::PerlinNoise2D` (引擎内置) | 自定义 `PerlinNoise2D`（Hash2D + 梯度插值） |
| 确定性偏移 | `FMath::RandInit(Seed)` + `RandRange` | `Hash2D(float2(Seed+i*127, Seed-i*311)) * 200000 - 100000` |
| 精度 | float32 | float32 |
| Group Size | N/A | 8×8×1（每个线程一个像素） |

**HLSL 入口点**: `MainComputeShader`

```hlsl
[numthreads(8, 8, 1)]
void MainComputeShader(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (x >= ChunkSize || y >= ChunkSize) return;
    float2 p = (float2(x,y) - halfSize + Offset) / max(Scale, 0.0001);
    float noise = fBm(p, Octaves, Persistence, Lacunarity);
    RenderTarget[uint2(x, y)] = (noise + 1.0) * 0.5;  // → [0,1]
}
```

### 7.2 C++ Shader 绑定: `FProceduralLandmassNoiseCS`

- 继承 `FGlobalShader`，使用 `SHADER_USE_PARAMETER_STRUCT`
- `SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RenderTarget)` — 输出 UAV
- 编译环境通过 `ModifyCompilationEnvironment` 注入 `THREADGROUPSIZE_X/Y/Z` 宏
- 注册为 `IMPLEMENT_GLOBAL_SHADER(..., "/ProceduralLandmass/ProceduralLandmassNoise.usf", "MainComputeShader", SF_Compute)`

### 7.3 调度接口: `FProceduralLandmassNoiseCSInterface`

```
Dispatch(Params)
├─ IsInRenderingThread()? → DispatchRenderThread(RHICmdList, Params)
└─ else                  → DispatchGameThread(Params)
                              └─ ENQUEUE_RENDER_COMMAND(→ DispatchRenderThread)
```

**Render Thread 执行流程**:

```
1. FRDGBuilder GraphBuilder(RHICmdList)
2. 创建中间 RDG 纹理 TmpTexture (PF_R32_FLOAT, UAV)
3. RegisterExternalTexture(外部 RenderTarget)
4. AddPass("ExecuteProceduralLandmassNoiseCS", Compute):
     FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, PassParams, GroupCount)
5. AddCopyTexturePass(TmpTexture → TargetTexture) [仅当格式匹配 PF_R32_FLOAT]
6. [Debug] 若 ReadbackMode > 0:
     AddEnqueueCopyPass(GraphBuilder, &NoiseReadback, TmpTexture)
7. GraphBuilder.Execute()
8. [Debug] BlockUntilGPUIdle() → Lock Readback → UE_LOG 统计/像素值
```

### 7.4 Debug Readback 系统

控制台变量: `r.ProceduralLandmass.NoiseCS.Readback`

| 值 | 行为 |
|----|------|
| 0 | 关闭（默认，零开销） |
| 1 | 输出 min/max/avg/zeroCount + 10×10 角采样 |
| 2 | 完整像素 dump（可能大量刷屏） |

实现：`FRHIGPUTextureReadback` + `AddEnqueueCopyPass` → `BlockUntilGPUIdle` → `Lock/Unlock` 读取 float 数据。

### 7.5 BP 异步节点: `UProceduralLandmassNoiseCS_AsyncExecution`

```cpp
UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true"))
static UProceduralLandmassNoiseCS_AsyncExecution* ExecuteGPUNoiseMap(
    UObject* WorldContextObject, UTextureRenderTarget2D* RT,
    int32 ChunkSize, float Scale, int32 Octaves,
    float Persistence, float Lacunarity, int32 Seed, FVector2D Offset);
```

- 继承 `UBlueprintAsyncActionBase`
- `Activate()` 中调用 `FProceduralLandmassNoiseCSInterface::Dispatch(Params)`
- 要求 RT 格式为 `PF_R32_FLOAT`，尺寸为 `ChunkSize × ChunkSize`

---

## 8. Actor 层

### 8.1 AProceduralLandmassActor — 同步模式

**组件结构**:
```
Root (USceneComponent)
└── TerrainMeshComponent (UStaticMeshComponent)
```

**属性分类**:

| 分类 | 属性 | 类型 | 默认 |
|------|------|------|------|
| Noise | `ChunkSize` | `int32` | 128 |
| Noise | `Scale` | `float` | 50.0 |
| Noise | `Octaves` | `int32` | 6 |
| Noise | `Persistence` | `float` | 0.5 |
| Noise | `Lacunarity` | `float` | 2.0 |
| Noise | `Seed` | `int32` | 0 |
| Noise | `Offset` | `FVector2D` | (0,0) |
| Noise | `HeightScale` | `float` | 1000.0 |
| Terrain | `HeightCurve` | `UCurveFloat*` | null |
| Terrain | `LODLevels` | `int32` [1,8] | 1 |
| Appearance | `Material` | `UMaterialInterface*` | null |
| Appearance | `TerrainTypes` | `TArray<FTerrainType>` | [] |

**生成流程** (`GenerateTerrain()`):
```
1. GenerateNoiseMap → TArray<float> NoiseMap
2. GenerateTerrainMesh → UTerrainMeshData* MeshData
3. MeshData->CreateMesh() → UStaticMesh* GeneratedMesh
4. TerrainMeshComponent->SetStaticMesh(GeneratedMesh)
5. 若有 TerrainTypes: GenerateColorNoiseTexture → 创建 DynamicMaterialInstance
   设置 "TerrainColorTexture" 参数
```

编辑器中使用 `CallInEditor` + `PostRegisterAllComponents` 自动触发，运行时需手动调用 `GenerateTerrain()`。

### 8.2 AProceduralLandmassAsyncActor — 异步 Pipeline 模式

**核心设计：Ticket + WeakPtr 模式**

```
                    ┌─────────────────────────────────┐
                    │    GenerationTicket (atomic)      │
                    │    每次 GenerateTerrainAsync()    │
                    │    ++GenerationTicket             │
                    └─────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Pipeline Step 1: NoiseMap (Background Thread)                      │
│  ─────────────────────────────────────────────                      │
│  GenerateNoiseMapAsync(..., OnComplete)                             │
│  回调到 GameThread 时检查: Ticket == GenerationTicket?              │
│    ├─ Yes → PipelineStep_TerrainMesh(NoiseMap, Snapshot, Ticket)    │
│    └─ No  → 丢弃（已有过时的新请求）                                 │
├─────────────────────────────────────────────────────────────────────┤
│  Pipeline Step 2: TerrainMesh (Game Thread)                         │
│  ─────────────────────────────────────────                          │
│  GenerateTerrainMeshAsync(..., OnComplete)                          │
│  回调时检查: Ticket == GenerationTicket?                            │
│    ├─ Yes → CreateMesh → SetStaticMesh → PipelineStep_ApplyMaterial │
│    └─ No  → 丢弃                                                    │
├─────────────────────────────────────────────────────────────────────┤
│  Pipeline Step 3: ApplyMaterial (Game Thread)                       │
│  ───────────────────────────────────────                            │
│  若有 TerrainTypes: GenerateColorNoiseTexture → DynamicMaterialInstance│
│  否则: SetMaterial(0, Material)                                     │
│  bIsGenerating = false                                              │
└─────────────────────────────────────────────────────────────────────┘
```

**线程安全机制**:
- `GenerationTicket` (`std::atomic<uint32>`) — 单调递增，每次新请求 +1
- `bDestroying` (`FThreadSafeBool`) — `BeginDestroy()` 设为 true，防止 Actor 销毁后回调
- `TWeakObjectPtr<AProceduralLandmassAsyncActor>` — 防止 dangling pointer
- `FGenSnapshot` — 在请求时快照全部参数（值拷贝），避免并发读写

**关键区别** vs 同步版：
- NoiseMap 计算在后台线程执行（不阻塞 GameThread）
- 连续多次调用 `GenerateTerrainAsync()` 时，只有最后一次的 Ticket 有效，旧的自动丢弃
- `IsGenerating()` 查询是否仍有 Pipeline 在飞行中

---

## 9. 九宫格系统 (GridUtils)

### 9.1 坐标转换

```
WorldToGrid(WorldPos, CellSize):
  GridX = Floor(WorldPos.X / CellSize)
  GridY = Floor(WorldPos.Y / CellSize)

GridToWorld(GridCoord, CellSize, Z):
  WorldX = (GridX + 0.5) * CellSize    // 格子中心
  WorldY = (GridY + 0.5) * CellSize
```

### 9.2 九宫格 (3×3 Neighbor Grid)

以玩家所在格子为中心，获取周围 3×3 = 9 个格子：

```
(-1,-1)  (0,-1)  (+1,-1)
(-1, 0)  (0, 0)  (+1, 0)
(-1,+1)  (0,+1)  (+1,+1)
```

遍历顺序：从左上到右下（3 行 × 3 列），Y 轴反转（UE 坐标系）。

### 9.3 辅助函数

| 函数 | 功能 |
|------|------|
| `GridDistance(A, B)` | Chebyshev 距离 `max(|Δx|, |Δy|)` |
| `IsAdjacent(A, B)` | 8 方向相邻判定（距离 == 1） |

---

## 10. 数据流总结

### CPU 路径（完整地形生成）

```
NoiseParams ──► GenerateNoiseMap() ──► TArray<float> (ChunkSize²)
                                            │
                    ┌───────────────────────┼───────────────────────┐
                    ▼                       ▼                       ▼
            GenerateNoiseTexture    GenerateColorNoiseTexture  GenerateTerrainMesh
            (灰度调试用)             (地形着色纹理)             (多LOD网格)
                    │                       │                       │
                    ▼                       ▼                       ▼
              UTexture2D              UTexture2D             UTerrainMeshData
                                                             │
                                                    CreateMesh()
                                                             │
                                                             ▼
                                                        UStaticMesh
                                                    (含碰撞体 CTF_UseComplexAsSimple)
```

### GPU 路径（仅噪声图）

```
NoiseParams ──► FProceduralLandmassNoiseCSInterface::Dispatch()
                     │
                     ▼
              ENQUEUE_RENDER_COMMAND
                     │
                     ▼
              FRDGBuilder (DispatchRenderThread)
                     │
          ┌──────────┼──────────┐
          ▼                     ▼
     Compute Shader        AddCopyTexturePass
     TmpTexture (RDG)      TmpTexture → TargetTexture (外部RT)
          │
          ▼ (Debug: ReadbackMode > 0)
     AddEnqueueCopyPass → FRHIGPUTextureReadback
          │
          ▼
     GraphBuilder.Execute()
          │
          ▼ (Debug)
     BlockUntilGPUIdle → Lock → UE_LOG (float pixel values)
```

### 异步 Actor Pipeline

```
GenerateTerrainAsync()
│ Snapshot 参数快照
│ ++GenerationTicket
│
├─► PipelineStep_NoiseMap (Background Thread)
│   └─ GenerateNoiseMapAsync → callback (GameThread)
│      └─ Ticket check → PipelineStep_TerrainMesh
│
├─► PipelineStep_TerrainMesh (GameThread)
│   └─ GenerateTerrainMesh → CreateMesh → SetStaticMesh
│      → PipelineStep_ApplyMaterial
│
└─► PipelineStep_ApplyMaterial (GameThread)
    └─ GenerateColorNoiseTexture → DynamicMaterialInstance
       或 SetMaterial(0, Material)
       bIsGenerating = false
```

---

## 11. 关键设计决策

| 决策 | 原因 |
|------|------|
| 理论振幅归一化 | 保证相邻 Chunk 边界像素值一致，支持无限地形无缝拼接 |
| `PostConfigInit` 加载阶段 | Shader 目录映射必须在其他模块请求 Shader 编译前完成 |
| GPU 输出为 RDG 中间纹理 + Copy | RDG 的 UAV 写入与外部 RT 的兼容性处理；Copy Pass 确保数据写入外部可见纹理 |
| Ticket 模式（AsyncActor） | 比 cancel-and-restart 模式更简单可靠；旧任务自然丢弃，不需要 CancellationToken |
| `FGenSnapshot` 值拷贝 | 防止参数在异步执行期间被修改（如编辑器拖动滑块时） |
| `TWeakObjectPtr` + `bDestroying` 双重保护 | 防止 Actor 销毁后回调访问野指针 |
| 引擎内置 `FMath::PerlinNoise2D`（CPU）vs 自定义 Perlin（GPU） | CPU 使用 UE 优化的实现；GPU 无等价函数，需自行实现 |
| `CTF_UseComplexAsSimple` 碰撞 | 地形需要三角形级精确碰撞（如子弹命中检测），优先正确性 |

---

## 12. 已知限制与改进方向

| 限制 | 说明 |
|------|------|
| **单 Chunk 生成** | 当前 Actor 一次只生成一个 Chunk，不支持自动无限地形扩展。需要在上层组合多个 Actor 配合 GridUtils 实现 |
| **GPU 噪声仅输出到 RT** | GPU 路径写入 RenderTarget，不能直接用于网格生成。若要用 GPU 噪声做地形网格，需要通过 Readback 读回 CPU，跨越 GPU/CPU 同步边界 |
| **Texture 创建为同步** | `GenerateNoiseTexture` 和 `GenerateColorNoiseTexture` 都在 GameThread 同步执行 `BulkData.Lock/Unlock`，对 2048+ Chunk 会有明显卡顿 |
| **无运行时 LOD 切换反馈** | 虽然生成了多 LOD 网格数据，但 LOD 切换完全依赖引擎的 `UStaticMesh` 渲染系统，无自定义逻辑 |
| **GPU Readback 为 Debug Only** | 同步 `BlockUntilGPUIdle` 对帧率有显著影响，不应在 Shipping 中使用 |
| **噪声算法固定在 fBm Perlin** | 没有插件化噪声策略接口。如果要支持 Worley/Simplex/Ridged 噪声，需要修改核心代码 |

---

## 13. 扩展点

| 扩展方向 | 建议方式 |
|----------|---------|
| **新噪声算法** | 在 `UProceduralLandmassBPLibrary` 中添加新的 `Generate*NoiseMap` 静态函数 + 对应的 Compute Shader `.usf` |
| **GPU 地形网格** | 添加新的 Compute Shader 直接在 GPU 生成顶点/索引 Buffer，通过 `FRDGBuffer` 或 `FRHIComputeCommandList` 回读或直接绑定 |
| **无限地形** | 在上层创建 World Subsystem 或 GameMode 子系统，使用 GridUtils 做 Chunk 管理（加载/卸载/生成） |
| **Biome 系统** | 扩展 `FTerrainType`，加入湿度/温度等维度，用多层噪声生成更复杂的地形分布 |
| **GPU Readback 生产化** | 将同步 Readback 改为异步（pooled `FRHIGPUTextureReadback` + frame delay），在 Shipping build 中可用 |
