// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ProceduralLandmassMeshCS_AsyncExecution.generated.h"

// Delegate must be declared BEFORE the class that uses it for UHT code generation
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMeshGenerationCompleted);

/**
 * Blueprint async-action node for GPU terrain mesh generation (Approach A: CS -> Buffer).
 *
 * Dispatches FTerrainMeshGenCSInterface on the render thread and extracts
 * GPU buffers containing vertex positions, normals, UVs, and triangle indices.
 *
 * Usage from Blueprint:
 *   Call ExecuteGPUMeshGen, wire the latent exec pin.
 */
UCLASS()
class PROCEDURALLANDMASS_API UProceduralLandmassMeshCS_AsyncExecution
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	virtual void Activate() override;

	UFUNCTION(BlueprintCallable,
		meta = (BlueprintInternalUseOnly = "true", Category = "ProceduralLandmass|GPU",
			WorldContext = "WorldContextObject"))
	static UProceduralLandmassMeshCS_AsyncExecution* ExecuteGPUMeshGen(
		UObject* WorldContextObject,
		UTextureRenderTarget2D* HeightRT,
		int32 GridSize     = 128,
		float HeightScale  = 1000.0f,
		float WorldOriginX = 0.0f,
		float WorldOriginY = 0.0f);

	UPROPERTY(BlueprintAssignable)
	FOnMeshGenerationCompleted OnCompleted;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> HeightRT;

	UPROPERTY()
	int32 GridSize = 128;

	UPROPERTY()
	float HeightScale = 1000.0f;

	UPROPERTY()
	float WorldOriginX = 0.0f;

	UPROPERTY()
	float WorldOriginY = 0.0f;
};
