// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ProceduralLandmassMeshShader_AsyncExecution.generated.h"

// Delegate must be declared BEFORE the class that uses it for UHT code generation
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMeshShaderGenerationCompleted);

/**
 * Blueprint async-action node for GPU terrain mesh rendering (Approach B: Mesh Shader).
 *
 * Dispatches FTerrainMeshShaderMSInterface on the render thread to render
 * terrain geometry directly from the noise heightmap RT via SM6 Mesh Shader.
 *
 * Platform requirement: SM6 Tier 0 (DX12 / Vulkan 1.3).
 * Use IsMeshShaderSupported to check at runtime before calling.
 */
UCLASS()
class PROCEDURALLANDMASS_API UProceduralLandmassMeshShader_AsyncExecution
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	virtual void Activate() override;

	UFUNCTION(BlueprintCallable,
		meta = (BlueprintInternalUseOnly = "true", Category = "ProceduralLandmass|GPU",
			WorldContext = "WorldContextObject"))
	static UProceduralLandmassMeshShader_AsyncExecution* ExecuteGPUMeshShader(
		UObject* WorldContextObject,
		UTextureRenderTarget2D* HeightRT,
		int32 GridSize     = 128,
		float HeightScale  = 1000.0f,
		float WorldOriginX = 0.0f,
		float WorldOriginY = 0.0f);

	UFUNCTION(BlueprintCallable, Category = "ProceduralLandmass|GPU")
	static bool IsMeshShaderSupported();

	UPROPERTY(BlueprintAssignable)
	FOnMeshShaderGenerationCompleted OnCompleted;

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
