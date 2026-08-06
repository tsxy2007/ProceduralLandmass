// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RenderGraphResources.h"

class UTextureRenderTarget2D;

/**
 * Scene View Extension that injects terrain mesh shader draws into the
 * deferred render pipeline.
 *
 * Every frame (in PostRenderBasePassDeferred_RenderThread), this extension:
 *   1. Reads the noise heightmap from a GPU RenderTarget
 *   2. Dispatches a SM6 mesh shader (FTerrainMeshShaderMS) that generates
 *      terrain geometry on-the-fly from the heightmap
 *   3. Shades via a pixel shader (FTerrainMeshShaderPS) using UE5's PBR
 *      pipeline: BRDF.ush (GGX/Smith/Schlick) with directional light from
 *      the View uniform buffer + atmospheric sky ambient
 *
 * The terrain writes directly into SceneColor — no CPU mesh, no readback,
 * no UStaticMesh involved.
 *
 * Platform requirement: SM6 Tier 0 (DX12 / Vulkan 1.3) with mesh shader support.
 *
 * Usage (game thread):
 *   TSharedPtr<FTerrainMeshSceneViewExtension> ViewExt;
 *   ViewExt = FSceneViewExtensions::NewExtension<FTerrainMeshSceneViewExtension>(World);
 *   ViewExt->SetEnabled(true);
 *   ViewExt->UpdateHeightmap(MyHeightRT);
 *   ViewExt->UpdateParams(128, 1000.0f, FVector2f::ZeroVector);
 */
class PROCEDURALLANDMASS_API FTerrainMeshSceneViewExtension : public FWorldSceneViewExtension
{
public:
	FTerrainMeshSceneViewExtension(const FAutoRegister& AutoReg, UWorld* InWorld);

	// ── Game-thread interface ───────────────────────────────────────────────

	/** Enable/disable terrain rendering this frame. */
	void SetEnabled(bool bInEnabled);

	/**
	 * Update the heightmap RenderTarget (must be PF_R32_FLOAT, GridSize×GridSize).
	 * The RHI texture is extracted and stored (thread-safe ref-counted),
	 * so the UObject may be GC'd after this call.
	 */
	void UpdateHeightmap(UTextureRenderTarget2D* RT);

	/** Update grid parameters (same units as FTerrainMeshGenParameters). */
	void UpdateParams(int32 InGridSize, float InHeightScale, FVector2f InWorldOrigin);

	// ── ISceneViewExtension overrides ───────────────────────────────────────

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

	virtual int32 GetPriority() const override { return 0; }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	// ── Render-thread state (updated via ENQUEUE_RENDER_COMMAND) ────────────
	// These are read on the render thread during PostRenderBasePassDeferred.

	FTextureRHIRef HeightmapTextureRHI;   // ref-counted, safe on RT
	int32           GridSize       = 128;
	float           HeightScale    = 1000.0f;
	FVector2f       WorldOrigin    = FVector2f::ZeroVector;
	bool            bRTDataValid   = false;    // true after successful UpdateHeightmap

	// ── Game-thread state ───────────────────────────────────────────────────
	std::atomic<bool> bEnabled{false};
};
