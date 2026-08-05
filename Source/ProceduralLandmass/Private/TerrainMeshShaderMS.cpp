// Copyright Epic Games, Inc. All Rights Reserved.

#include "TerrainMeshShaderMS.h"
#include "TerrainMeshGenCS.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "RHI.h"
#include "DataDrivenShaderPlatformInfo.h"

UE_DISABLE_OPTIMIZATION

// ── Tile size (must match the HLSL TILE_SIZE define) ────────────────────────

#define TERRAIN_MS_TILE_SIZE 8

// ── Stats ──────────────────────────────────────────────────────────────────

DECLARE_STATS_GROUP(TEXT("ProceduralLandmassMeshShader"),
	STATGROUP_ProceduralLandmassMeshShader, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("ProceduralLandmassMeshShader Execute"),
	STAT_ProceduralLandmassMeshShader_Execute, STATGROUP_ProceduralLandmassMeshShader);

static TAutoConsoleVariable<int32> CVarMeshShaderEnable(
	TEXT("r.ProceduralLandmass.MeshShader.Enable"),
	0,
	TEXT("0: Disabled (default)\n")
	TEXT("1: Log dispatch info\n")
	TEXT("2: Full debug logging"),
	ECVF_RenderThreadSafe);

DEFINE_LOG_CATEGORY_STATIC(LogProceduralLandmassMeshShader, Log, All);

// ═══════════════════════════════════════════════════════════════════════════
//  FGlobalShader: binds C++ side → HLSL (SF_Mesh)
// ═══════════════════════════════════════════════════════════════════════════

IMPLEMENT_GLOBAL_SHADER(FTerrainMeshShaderMS,
	"/ProceduralLandmass/TerrainLandmassMeshShader.usf",
	"MainTerrainMeshMS", SF_Mesh);

IMPLEMENT_GLOBAL_SHADER(FTerrainMeshShaderPS,
	"/ProceduralLandmass/TerrainLandmassMeshShader.usf",
	"MainTerrainPS", SF_Pixel);

bool FTerrainMeshShaderMS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	// Only compile for platforms with mesh shader support (SM6 Tier 0+)
	return RHISupportsMeshShadersTier0(Parameters.Platform);
}

void FTerrainMeshShaderMS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("TILE_SIZE"), TERRAIN_MS_TILE_SIZE);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch helpers
// ═══════════════════════════════════════════════════════════════════════════

bool FTerrainMeshShaderMSInterface::IsSupported()
{
	return GRHISupportsMeshShadersTier0 != 0;
}

void FTerrainMeshShaderMSInterface::Dispatch(
	const FTerrainMeshGenParameters& Params)
{
	if (!IsSupported())
	{
		UE_LOG(LogProceduralLandmassMeshShader, Warning,
			TEXT("Dispatch: Mesh shaders are not supported on this platform/RHI."));
		return;
	}

	if (IsInRenderingThread())
	{
		DispatchRenderThread(
			GetImmediateCommandList_ForRenderCommand(), Params);
	}
	else
	{
		DispatchGameThread(Params);
	}
}

void FTerrainMeshShaderMSInterface::DispatchGameThread(
	const FTerrainMeshGenParameters& Params)
{
	ENQUEUE_RENDER_COMMAND(ProceduralLandmassTerrainMeshShader)(
		[Params](FRHICommandListImmediate& RHICmdList)
		{
			DispatchRenderThread(RHICmdList, Params);
		});
}

void FTerrainMeshShaderMSInterface::DispatchRenderThread(
	FRHICommandListImmediate& RHICmdList,
	const FTerrainMeshGenParameters& Params)
{
	const int32 GridSize = FMath::Max(Params.GridSize, 1);
	if (Params.HeightRenderTarget == nullptr)
	{
		UE_LOG(LogProceduralLandmassMeshShader, Warning,
			TEXT("DispatchRenderThread: HeightRenderTarget is null, skipping."));
		return;
	}

	const int32 ReadbackMode = CVarMeshShaderEnable.GetValueOnRenderThread();
	if (ReadbackMode == 0)
	{
		return;
	}

	typename FTerrainMeshShaderMS::FPermutationDomain PermutationVector;
	TShaderMapRef<FTerrainMeshShaderMS> MeshShader(
		GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

	if (!MeshShader.IsValid())
	{
		UE_LOG(LogProceduralLandmassMeshShader, Error,
			TEXT("FTerrainMeshShaderMS shader is not valid (may not have compiled for this platform)."));
		return;
	}

	const FRHIMeshShader* MeshShaderRHI = MeshShader.GetMeshShader();
	if (!MeshShaderRHI)
	{
		UE_LOG(LogProceduralLandmassMeshShader, Error,
			TEXT("GetMeshShader() returned null."));
		return;
	}

	const int32 TilesX = FMath::DivideAndRoundUp(GridSize, TERRAIN_MS_TILE_SIZE);
	const int32 TilesY = FMath::DivideAndRoundUp(GridSize, TERRAIN_MS_TILE_SIZE);
	const int32 NumGroups = TilesX * TilesY;

	FRDGBuilder GraphBuilder(RHICmdList);

	{
		SCOPE_CYCLE_COUNTER(STAT_ProceduralLandmassMeshShader_Execute);
		DECLARE_GPU_STAT(ProceduralLandmassMeshShader);
		RDG_EVENT_SCOPE(GraphBuilder, "ProceduralLandmassMeshShader");
		RDG_GPU_STAT_SCOPE(GraphBuilder, ProceduralLandmassMeshShader);

		FRDGTextureRef HeightRdgTexture = RegisterExternalTexture(
			GraphBuilder,
			Params.HeightRenderTarget->GetRenderTargetTexture(),
			TEXT("TerrainMeshShader_HeightRT"));

		FRDGTextureSRVRef HeightSrv = GraphBuilder.CreateSRV(
			FRDGTextureSRVDesc::Create(HeightRdgTexture));

		FTerrainMeshShaderMS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FTerrainMeshShaderMS::FParameters>();

		PassParameters->HeightMap   = HeightSrv->GetRHI();
		PassParameters->GridSize    = GridSize;
		PassParameters->HeightScale = Params.HeightScale;
		PassParameters->WorldOrigin = Params.WorldOrigin;
		PassParameters->ViewProjectionMatrix = FMatrix44f::Identity;

		if (ReadbackMode >= 1)
		{
			UE_LOG(LogProceduralLandmassMeshShader, Log,
				TEXT("=== TerrainMeshShader Dispatch ==="));
			UE_LOG(LogProceduralLandmassMeshShader, Log,
				TEXT("  GridSize=%d  TileSize=%d  Tiles=(%d,%d)  TotalGroups=%d"),
				GridSize, TERRAIN_MS_TILE_SIZE, TilesX, TilesY, NumGroups);
			UE_LOG(LogProceduralLandmassMeshShader, Log,
				TEXT("  HeightScale=%.1f  WorldOrigin=(%.1f, %.1f)"),
				Params.HeightScale, Params.WorldOrigin.X, Params.WorldOrigin.Y);
			UE_LOG(LogProceduralLandmassMeshShader, Log,
				TEXT("  MeshShaderRHI=%p  IsValid=%d"),
				MeshShaderRHI, MeshShader.IsValid() ? 1 : 0);
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ExecuteTerrainMeshShader"),
			PassParameters,
			ERDGPassFlags::Raster,
			[MeshShader, MeshShaderRHI, PassParameters, NumGroups](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				GraphicsPSOInit.BlendState        = TStaticBlendState<>::GetRHI();
				GraphicsPSOInit.RasterizerState   = TStaticRasterizerState<>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<>::GetRHI();
				GraphicsPSOInit.BoundShaderState.SetMeshShader(
					const_cast<FRHIMeshShader*>(MeshShaderRHI));

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				SetShaderParameters(RHICmdList, MeshShader,
					const_cast<FRHIMeshShader*>(MeshShaderRHI),
					*PassParameters);

				RHICmdList.DispatchMeshShader(NumGroups, 1, 1);

				if (CVarMeshShaderEnable.GetValueOnRenderThread() >= 2)
				{
					UE_LOG(LogProceduralLandmassMeshShader, Log,
						TEXT("  [RasterPass] Dispatched %d mesh shader groups."), NumGroups);
				}
			});
	}

	GraphBuilder.Execute();
}

UE_ENABLE_OPTIMIZATION
