// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"

// Forward-declare shared parameter struct (defined in TerrainMeshGenCS.h)
struct FTerrainMeshGenParameters;

// ═══════════════════════════════════════════════════════════════════════════
//  Global Shader: FTerrainMeshShaderMS  (SF_Mesh)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * SM6 Mesh Shader that generates terrain geometry directly from a noise heightmap RT.
 * Each mesh shader group handles one 8×8 tile (64 vertices, 98 triangles).
 *
 * Entry point: MainTerrainMeshMS  in TerrainLandmassMeshShader.usf
 *
 * Platform requirement: SM6 Tier 0 (DX12 / Vulkan 1.3) with mesh shader support.
 * Use FTerrainMeshShaderMSInterface::IsSupported() to check at runtime.
 */
class PROCEDURALLANDMASS_API FTerrainMeshShaderMS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTerrainMeshShaderMS);
	SHADER_USE_PARAMETER_STRUCT(FTerrainMeshShaderMS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: noise heightmap RT (SRV — read in mesh shader)
		SHADER_PARAMETER_SRV(Texture2D<float>, HeightMap)

		// Scalar uniforms
		SHADER_PARAMETER(int32, GridSize)
		SHADER_PARAMETER(float, HeightScale)
		SHADER_PARAMETER(FVector2f, WorldOrigin)
		SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)

		// Camera world-space position (for view-direction calculation in PS)
		SHADER_PARAMETER(FVector3f, CameraWorldPos)

		// View uniform buffer (auto-bound in standard passes, explicit in custom passes)
		// Provides: DirectionalLightColor, DirectionalLightDirection,
		//           AtmosphereLightIlluminance*, SkyAtmosphere*, IndirectLightingColorScale
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Render target bindings — required for rasterization passes via RDG
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	/**
	 * Only compile for platforms that support mesh shaders (SM6).
	 * Requires: PLATFORM_SUPPORTS_MESH_SHADERS
	 */
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Global Shader: FTerrainMeshShaderPS  (SF_Pixel)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Simple pixel shader for terrain mesh shading.
 * Reads interpolated world-position, normal, and UV from the mesh shader,
 * computes NdotL diffuse + height-based albedo.
 *
 * Entry point: MainTerrainPS  in TerrainLandmassMeshShader.usf
 */
class PROCEDURALLANDMASS_API FTerrainMeshShaderPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTerrainMeshShaderPS);
	SHADER_USE_PARAMETER_STRUCT(FTerrainMeshShaderPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// View uniform buffer — needed for BRDF.ush PBR lighting in the PS
		// (DirectionalLightColor, DirectionalLightDirection, SkyAtmosphere*, etc.)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Camera world-space position for view-direction calculation (V = Cam - Pos)
		SHADER_PARAMETER(FVector3f, CameraWorldPos)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return RHISupportsMeshShadersTier0(Parameters.Platform);
	}
};

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch Interface: FTerrainMeshShaderMSInterface
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Thread-safe dispatch interface for the terrain mesh shader.
 * Automatically routes to the render thread if called from the game thread.
 *
 * Usage:
 *   if (FTerrainMeshShaderMSInterface::IsSupported())
 *   {
 *       FTerrainMeshGenParameters Params;
 *       Params.HeightRenderTarget = MyRT->GameThread_GetRenderTargetResource();
 *       FTerrainMeshShaderMSInterface::Dispatch(Params);
 *   }
 */
class PROCEDURALLANDMASS_API FTerrainMeshShaderMSInterface
{
public:
	/**
	 * Check whether the current RHI supports mesh shaders.
	 * Returns false on DX11, non-tier-0 Vulkan, or platforms without mesh shader support.
	 */
	static bool IsSupported();

	/**
	 * Dispatch the terrain mesh shader for rendering.
	 * The mesh shader reads the heightmap RT directly and outputs geometry to the rasterizer.
	 *
	 * @param Params  Grid dimensions, height scale, and source heightmap RT.
	 */
	static void Dispatch(const FTerrainMeshGenParameters& Params);

private:
	static void DispatchRenderThread(
		FRHICommandListImmediate& RHICmdList,
		const FTerrainMeshGenParameters& Params);

	static void DispatchGameThread(const FTerrainMeshGenParameters& Params);
};
