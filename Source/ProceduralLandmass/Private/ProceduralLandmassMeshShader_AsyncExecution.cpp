// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassMeshShader_AsyncExecution.h"
#include "TerrainMeshShaderMS.h"
#include "TerrainMeshGenCS.h"

void UProceduralLandmassMeshShader_AsyncExecution::Activate()
{
	// Validate input
	if (!HeightRT)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ProceduralLandmassMeshShader_AsyncExecution: HeightRT is null."));
		OnCompleted.Broadcast();
		return;
	}

	if (!FTerrainMeshShaderMSInterface::IsSupported())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ProceduralLandmassMeshShader_AsyncExecution: Mesh shaders not supported on this platform."));
		OnCompleted.Broadcast();
		return;
	}

	// Build dispatch parameters
	FTerrainMeshGenParameters Params;
	Params.GridSize           = GridSize;
	Params.HeightScale        = HeightScale;
	Params.WorldOrigin        = FVector2f(WorldOriginX, WorldOriginY);
	Params.HeightRenderTarget = HeightRT->GameThread_GetRenderTargetResource();

	// Dispatch mesh shader on render thread
	FTerrainMeshShaderMSInterface::Dispatch(Params);

	// Signal completion on game thread
	OnCompleted.Broadcast();
}

UProceduralLandmassMeshShader_AsyncExecution*
UProceduralLandmassMeshShader_AsyncExecution::ExecuteGPUMeshShader(
	UObject* WorldContextObject,
	UTextureRenderTarget2D* HeightRT,
	int32 GridSize,
	float HeightScale,
	float WorldOriginX,
	float WorldOriginY)
{
	UProceduralLandmassMeshShader_AsyncExecution* Action =
		NewObject<UProceduralLandmassMeshShader_AsyncExecution>();
	Action->HeightRT     = HeightRT;
	Action->GridSize     = GridSize;
	Action->HeightScale  = HeightScale;
	Action->WorldOriginX = WorldOriginX;
	Action->WorldOriginY = WorldOriginY;
	Action->RegisterWithGameInstance(WorldContextObject);

	return Action;
}

bool UProceduralLandmassMeshShader_AsyncExecution::IsMeshShaderSupported()
{
	return FTerrainMeshShaderMSInterface::IsSupported();
}
