// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ProceduralLandmassBPLibrary.h"
#include "ProceduralLandmassNoiseCS_AsyncExecution.generated.h"

/**
 * Parameters required to dispatch the noise-map compute shader.
 * Mirrors UProceduralLandmassBPLibrary::GenerateNoiseMap arguments.
 */
struct FProceduralLandmassNoiseCSParameters
{
	int         ChunkSize    = 256;
	float       Scale        = 1.0f;
	int         Octaves      = 4;
	float       Persistence  = 0.5f;
	float       Lacunarity   = 2.0f;
	int         Seed         = 0;
	FVector2D   Offset       = FVector2D::ZeroVector;
	FRenderTarget* RenderTarget = nullptr;
	FRenderTarget* ColorRenderTarget = nullptr;  // RGBA8 color output
	TArray<FTerrainType> TerrainTypes;           // Terrain type array

	FProceduralLandmassNoiseCSParameters() = default;

	FProceduralLandmassNoiseCSParameters(
		int InChunkSize, float InScale, int InOctaves,
		float InPersistence, float InLacunarity, int InSeed,
		const FVector2D& InOffset)
		: ChunkSize(InChunkSize), Scale(InScale), Octaves(InOctaves)
		, Persistence(InPersistence), Lacunarity(InLacunarity)
		, Seed(InSeed), Offset(InOffset)
	{}

	FProceduralLandmassNoiseCSParameters(
		int InChunkSize, float InScale, int InOctaves,
		float InPersistence, float InLacunarity, int InSeed,
		const FVector2D& InOffset,
		FRenderTarget* InColorRenderTarget,
		const TArray<FTerrainType>& InTerrainTypes)
		: ChunkSize(InChunkSize), Scale(InScale), Octaves(InOctaves)
		, Persistence(InPersistence), Lacunarity(InLacunarity)
		, Seed(InSeed), Offset(InOffset)
		, ColorRenderTarget(InColorRenderTarget), TerrainTypes(InTerrainTypes)
	{}
};

/**
 * Thread-safe dispatch interface.
 * Automatically routes to the render thread if called from the game thread.
 */
class PROCEDURALLANDMASS_API FProceduralLandmassNoiseCSInterface
{
public:
	static void Dispatch(FProceduralLandmassNoiseCSParameters Params);

private:
	static void DispatchRenderThread(
		FRHICommandListImmediate& RHICmdList,
		FProceduralLandmassNoiseCSParameters Params);

	static void DispatchGameThread(FProceduralLandmassNoiseCSParameters Params);
};

/**
 * Blueprint async-action node.
 *
 * Usage from Blueprint:
 *   Call ExecuteGPUNoiseMap, wire the latent exec pin, then read the RT
 *   after the latent action completes.
 */
UCLASS()
class PROCEDURALLANDMASS_API UProceduralLandmassNoiseCS_AsyncExecution
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	virtual void Activate() override;

	/**
	 * Schedule a compute-shader noise-map generation onto the render thread.
	 *
	 * @param WorldContextObject  World context
	 * @param RT                  Target RenderTarget (must be PF_R32_FLOAT, ChunkSize×ChunkSize)
	 * @param ChunkSize           Number of samples per side
	 * @param Scale               World-space scale
	 * @param Octaves             fBm octave count
	 * @param Persistence         Amplitude decay per octave
	 * @param Lacunarity          Frequency growth per octave
	 * @param Seed                RNG seed
	 * @param Offset              World-space offset
	 */
	UFUNCTION(BlueprintCallable,
		meta = (BlueprintInternalUseOnly = "true", Category = "ProceduralLandmass|GPU",
			WorldContext = "WorldContextObject"))
	static UProceduralLandmassNoiseCS_AsyncExecution* ExecuteGPUNoiseMap(
		UObject* WorldContextObject,
		const TArray<FTerrainType>& InTerrainTypes,
		int32 ChunkSize  = 256,
		float Scale      = 1.0f,
		int32 Octaves    = 4,
		float Persistence = 0.5f,
		float Lacunarity = 2.0f,
		int32 Seed = 0,
		FVector2D Offset = FVector2D::ZeroVector,
		UTextureRenderTarget2D* RT = nullptr,
		UTextureRenderTarget2D* ColorRT = nullptr);

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> RT;

	UPROPERTY()
	int32 ChunkSize;

	UPROPERTY()
	float Scale;

	UPROPERTY()
	int32 Octaves;

	UPROPERTY()
	float Persistence;

	UPROPERTY()
	float Lacunarity;

	UPROPERTY()
	int32 Seed;

	UPROPERTY()
	FVector2D Offset;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ColorRT;

	UPROPERTY()
	TArray<FTerrainType> TerrainTypes;
};
