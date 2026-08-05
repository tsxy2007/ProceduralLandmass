// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "DataDrivenShaderPlatformInfo.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Shared parameters — used by both Approach A (CS) and Approach B (Mesh Shader)
// ═══════════════════════════════════════════════════════════════════════════

/** Parameters required to generate a terrain mesh from a heightmap RT on GPU. */
struct FTerrainMeshGenParameters
{
	/** Number of vertices per side of the grid (total verts = GridSize^2). */
	int32 GridSize = 128;

	/** World-space height multiplier applied to the normalized [0,1] noise value. */
	float HeightScale = 1000.0f;

	/** World-space XY offset of the grid center. */
	FVector2f WorldOrigin = FVector2f::ZeroVector;

	/** RenderTarget containing the noise heightmap (PF_R32_FLOAT, GridSize×GridSize).
	 *  Typically produced by FProceduralLandmassNoiseCSInterface::Dispatch(). */
	FRenderTarget* HeightRenderTarget = nullptr;
};

// ═══════════════════════════════════════════════════════════════════════════
//  GPU-resident output buffers (Approach A)
// ═══════════════════════════════════════════════════════════════════════════

/** Container for GPU-generated mesh buffers.
 *  After Dispatch() completes, these pooled buffers hold the vertex/index data
 *  and can be used for DrawIndexedPrimitiveIndirect or CPU readback. */
struct FGeneratedMeshBuffers
{
	int32 NumVertices = 0;
	int32 NumIndices  = 0;

	TRefCountPtr<FRDGPooledBuffer> PositionBuffer;
	TRefCountPtr<FRDGPooledBuffer> NormalBuffer;
	TRefCountPtr<FRDGPooledBuffer> UVBuffer;
	TRefCountPtr<FRDGPooledBuffer> IndexBuffer;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Global Shader: FTerrainMeshGenCS  (SF_Compute)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Compute shader that reads a heightmap RT and writes vertex positions, normals,
 * UVs, and triangle indices into structured GPU buffers.
 *
 * Entry point: MainTerrainMeshGenCS  in TerrainMeshGenCS.usf
 */
class PROCEDURALLANDMASS_API FTerrainMeshGenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTerrainMeshGenCS);
	SHADER_USE_PARAMETER_STRUCT(FTerrainMeshGenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: noise heightmap RT
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, HeightMap)

		// Output UAVs: vertex/index buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, OutUVs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,  OutIndices)

		// Scalar uniforms
		SHADER_PARAMETER(int32, GridSize)
		SHADER_PARAMETER(float, HeightScale)
		SHADER_PARAMETER(FVector2f, WorldOrigin)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch Interface: FTerrainMeshGenCSInterface
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Thread-safe dispatch interface for the terrain mesh generation compute shader.
 * Automatically routes to the render thread if called from the game thread.
 *
 * Usage:
 *   FTerrainMeshGenParameters Params;
 *   Params.GridSize           = 128;
 *   Params.HeightScale        = 1000.0f;
 *   Params.HeightRenderTarget = MyRT->GameThread_GetRenderTargetResource();
 *   FGeneratedMeshBuffers Buffers;
 *   FTerrainMeshGenCSInterface::Dispatch(Params, Buffers);
 *   // Buffers.PositionBuffer / .IndexBuffer etc. are now populated
 */
struct FLODMeshData;  // forward-declare from TerrainMeshData.h

class PROCEDURALLANDMASS_API FTerrainMeshGenCSInterface
{
public:
	/**
	 * Dispatch the terrain mesh generation compute shader.
	 * @param Params   Grid dimensions, height scale, and source heightmap RT.
	 * @param OutBuffers  Receives the extracted GPU buffers after execution.
	 */
	static void Dispatch(const FTerrainMeshGenParameters& Params,
	                     FGeneratedMeshBuffers& OutBuffers);

	/**
	 * Dispatch the compute shader, then read back GPU buffers to CPU
	 * and populate FLODMeshData with vertex/index data.
	 *
	 * Blocks until GPU completes. Call from game thread only.
	 *
	 * @param Params       Grid dimensions and source heightmap RT.
	 * @param OutLODData   Receives the read-back vertex/index data.
	 * @return true if readback succeeded, false on failure.
	 */
	static bool DispatchAndReadback(const FTerrainMeshGenParameters& Params,
	                                FLODMeshData& OutLODData);

private:
	static void DispatchRenderThread(
		FRHICommandListImmediate& RHICmdList,
		const FTerrainMeshGenParameters& Params,
		FGeneratedMeshBuffers& OutBuffers);

	static void DispatchGameThread(
		const FTerrainMeshGenParameters& Params,
		FGeneratedMeshBuffers& OutBuffers);

	static bool ReadbackBuffers(
		FRHICommandListImmediate& RHICmdList,
		const FGeneratedMeshBuffers& Buffers,
		FLODMeshData& OutLODData);
};
