// Copyright Epic Games, Inc. All Rights Reserved.

#include "TerrainMeshSceneViewExtension.h"
#include "TerrainMeshShaderMS.h"
#include "TerrainMeshGenCS.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "RHI.h"
#include "SceneView.h"
#include "Engine/TextureRenderTarget2D.h"
#include "DataDrivenShaderPlatformInfo.h"

// ── Tile size (must match HLSL TILE_SIZE in TerrainLandmassMeshShader.usf) ──

#define TERRAIN_MS_TILE_SIZE 8

// ── CVar: global enable/disable ─────────────────────────────────────────────

static TAutoConsoleVariable<int32> CVarMeshShaderViewExt(
	TEXT("r.ProceduralLandmass.MeshShader.Enable"),
	0,
	TEXT("0: Disabled (default)\n")
	TEXT("1: Enable terrain mesh shader rendering via view extension\n")
	TEXT("2: Full debug logging"),
	ECVF_RenderThreadSafe);

DEFINE_LOG_CATEGORY_STATIC(LogTerrainMeshViewExt, Log, All);

// ═══════════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

FTerrainMeshSceneViewExtension::FTerrainMeshSceneViewExtension(
	const FAutoRegister& AutoReg, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoReg, InWorld)
{
}

// ═══════════════════════════════════════════════════════════════════════════
//  Game-thread interface
// ═══════════════════════════════════════════════════════════════════════════

void FTerrainMeshSceneViewExtension::SetEnabled(bool bInEnabled)
{
	bEnabled.store(bInEnabled, std::memory_order_release);
}

void FTerrainMeshSceneViewExtension::UpdateHeightmap(UTextureRenderTarget2D* RT)
{
	if (!RT)
	{
		ENQUEUE_RENDER_COMMAND(TerrainMeshViewExt_ClearRT)(
			[this](FRHICommandListImmediate&)
			{
				HeightmapTextureRHI = nullptr;
				bRTDataValid = false;
			});
		return;
	}

	FTextureRenderTargetResource* RTRes = RT->GameThread_GetRenderTargetResource();
	if (!RTRes)
	{
		return;
	}

	FTextureRHIRef TexRef = RTRes->GetRenderTargetTexture();
	if (!TexRef.IsValid())
	{
		return;
	}

	ENQUEUE_RENDER_COMMAND(TerrainMeshViewExt_UpdateRT)(
		[this, TexRef](FRHICommandListImmediate&)
		{
			HeightmapTextureRHI = TexRef;
			bRTDataValid = true;
		});
}

void FTerrainMeshSceneViewExtension::UpdateParams(
	int32 InGridSize, float InHeightScale, FVector2f InWorldOrigin)
{
	ENQUEUE_RENDER_COMMAND(TerrainMeshViewExt_UpdateParams)(
		[this, InGridSize, InHeightScale, InWorldOrigin](FRHICommandListImmediate&)
		{
			GridSize    = InGridSize;
			HeightScale = InHeightScale;
			WorldOrigin = InWorldOrigin;
		});
}

// ═══════════════════════════════════════════════════════════════════════════
//  ISceneViewExtension
// ═══════════════════════════════════════════════════════════════════════════

bool FTerrainMeshSceneViewExtension::IsActiveThisFrame_Internal(
	const FSceneViewExtensionContext& Context) const
{
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}

	if (!bEnabled.load(std::memory_order_acquire))
	{
		return false;
	}

	if (!bRTDataValid)
	{
		return false;
	}

	// Global CVar kill-switch
	if (CVarMeshShaderViewExt.GetValueOnGameThread() == 0)
	{
		return false;
	}

	// Mesh shader support is required
	if (!GRHISupportsMeshShadersTier0)
	{
		return false;
	}

	return true;
}

void FTerrainMeshSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// ── Guard: valid data ──────────────────────────────────────────────────
	if (!bRTDataValid || !HeightmapTextureRHI.IsValid())
	{
		return;
	}

	// ── Guard: CVar ────────────────────────────────────────────────────────
	const int32 CVarMode = CVarMeshShaderViewExt.GetValueOnRenderThread();
	if (CVarMode == 0)
	{
		return;
	}

	// ── Guard: mesh shader support ─────────────────────────────────────────
	if (!GRHISupportsMeshShadersTier0)
	{
		return;
	}

	// ── Scene render targets ───────────────────────────────────────────────
	FRDGTextureRef SceneColorRT = RenderTargets.Output[0].GetTexture();
	FRDGTextureRef SceneDepthRT = RenderTargets.DepthStencil.GetTexture();

	if (!SceneColorRT || !SceneDepthRT)
	{
		return;
	}

	// ── Get shaders ────────────────────────────────────────────────────────
	typename FTerrainMeshShaderMS::FPermutationDomain PermutationVector;
	TShaderMapRef<FTerrainMeshShaderMS> MeshShader(
		GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
	TShaderMapRef<FTerrainMeshShaderPS> PixelShader(
		GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!MeshShader.IsValid() || !PixelShader.IsValid())
	{
		if (CVarMode >= 1)
		{
			UE_LOG(LogTerrainMeshViewExt, Warning,
				TEXT("PostRenderBasePassDeferred: Mesh shader or pixel shader not valid (may not have compiled for this platform)."));
		}
		return;
	}

	const FRHIMeshShader* MeshShaderRHI = MeshShader.GetMeshShader();
	if (!MeshShaderRHI)
	{
		if (CVarMode >= 1)
		{
			UE_LOG(LogTerrainMeshViewExt, Warning,
				TEXT("PostRenderBasePassDeferred: GetMeshShader() returned null."));
		}
		return;
	}

	// ── Register heightmap as RDG texture ──────────────────────────────────
	FRDGTextureRef HeightRdgTexture = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(HeightmapTextureRHI, TEXT("TerrainMeshViewExt_Height")));

	// ── Tile group count ───────────────────────────────────────────────────
	const int32 TilesX = FMath::DivideAndRoundUp(GridSize, TERRAIN_MS_TILE_SIZE);
	const int32 TilesY = FMath::DivideAndRoundUp(GridSize, TERRAIN_MS_TILE_SIZE);
	const int32 NumGroups = TilesX * TilesY;

	if (NumGroups == 0)
	{
		return;
	}

	// ── View-projection matrix ─────────────────────────────────────────────
	const FMatrix44f ViewProjMatrix = FMatrix44f(InView.ViewMatrices.GetViewProjectionMatrix());

	// ── Allocate pass parameters ───────────────────────────────────────────
	FTerrainMeshShaderMS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FTerrainMeshShaderMS::FParameters>();

	PassParameters->HeightMap = GraphBuilder.CreateSRV(
		FRDGTextureSRVDesc::Create(HeightRdgTexture))->GetRHI();
	PassParameters->GridSize    = GridSize;
	PassParameters->HeightScale = HeightScale;
	PassParameters->WorldOrigin = WorldOrigin;
	PassParameters->ViewProjectionMatrix = ViewProjMatrix;
	PassParameters->CameraWorldPos = FVector3f(InView.ViewMatrices.GetViewOrigin());
	PassParameters->View = InView.ViewUniformBuffer;

	// Bind scene render targets — RDG handles resource transitions
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		SceneColorRT, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthRT,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthWrite_StencilWrite);

	// Capture format info for PSO setup inside the lambda
	const EPixelFormat ColorFormat = SceneColorRT->Desc.Format;
	const EPixelFormat DepthFormat = SceneDepthRT->Desc.Format;

	if (CVarMode >= 2)
	{
		UE_LOG(LogTerrainMeshViewExt, Log,
			TEXT("=== TerrainMeshShader ViewExt Dispatch ==="));
		UE_LOG(LogTerrainMeshViewExt, Log,
			TEXT("  GridSize=%d  TileSize=%d  Tiles=(%d,%d)  TotalGroups=%d"),
			GridSize, TERRAIN_MS_TILE_SIZE, TilesX, TilesY, NumGroups);
		UE_LOG(LogTerrainMeshViewExt, Log,
			TEXT("  ColorFormat=%d  DepthFormat=%d  HeightScale=%.1f"),
			(int32)ColorFormat, (int32)DepthFormat, HeightScale);
	}

	// ── Add raster pass ────────────────────────────────────────────────────
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("TerrainMeshShader_ViewExt"),
		PassParameters,
		ERDGPassFlags::Raster,
		[MeshShader, PixelShader, MeshShaderRHI, PassParameters, NumGroups, ColorFormat, DepthFormat]
		(FRHICommandList& RHICmdList)
		{
			// ── Graphics PSO with mesh shader ───────────────────────────────
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.RasterizerState   = TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
			GraphicsPSOInit.BlendState        = TStaticBlendStateWriteMask<CW_RGBA>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();

			GraphicsPSOInit.BoundShaderState.SetMeshShader(
				const_cast<FRHIMeshShader*>(MeshShaderRHI));
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
				PixelShader.GetPixelShader();

			GraphicsPSOInit.RenderTargetFormats[0]    = ColorFormat;
			GraphicsPSOInit.RenderTargetsEnabled       = 1;
			GraphicsPSOInit.DepthStencilTargetFormat   = DepthFormat;

			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			// ── Bind shader parameters + dispatch ──────────────────────────
			SetShaderParameters(RHICmdList, MeshShader,
				const_cast<FRHIMeshShader*>(MeshShaderRHI), *PassParameters);

			// Pixel shader needs its own View UFB + CameraWorldPos binding
			// (D3D12 binds constant buffers per-stage; MS bindings are
			//  NOT visible to the PS)
			{
				FTerrainMeshShaderPS::FParameters PSParams;
				PSParams.View = PassParameters->View;
				PSParams.CameraWorldPos = PassParameters->CameraWorldPos;
				SetShaderParameters(RHICmdList, PixelShader,
					PixelShader.GetPixelShader(), PSParams);
			}

			RHICmdList.DispatchMeshShader(NumGroups, 1, 1);
		});
}
