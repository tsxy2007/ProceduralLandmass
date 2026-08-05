// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassMeshCS_AsyncExecution.h"
#include "TerrainMeshGenCS.h"

void UProceduralLandmassMeshCS_AsyncExecution::Activate()
{
	// Validate input
	if (!HeightRT)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ProceduralLandmassMeshCS_AsyncExecution: HeightRT is null."));
		OnCompleted.Broadcast();
		return;
	}

	// Build dispatch parameters
	FTerrainMeshGenParameters Params;
	Params.GridSize           = GridSize;
	Params.HeightScale        = HeightScale;
	Params.WorldOrigin        = FVector2f(WorldOriginX, WorldOriginY);
	Params.HeightRenderTarget = HeightRT->GameThread_GetRenderTargetResource();

	// Dispatch on render thread (buffers extracted but not stored — caller
	// can use the RT directly or issue a subsequent readback)
	FGeneratedMeshBuffers Buffers;
	FTerrainMeshGenCSInterface::Dispatch(Params, Buffers);

	// Signal completion on game thread
	OnCompleted.Broadcast();
}

UProceduralLandmassMeshCS_AsyncExecution*
UProceduralLandmassMeshCS_AsyncExecution::ExecuteGPUMeshGen(
	UObject* WorldContextObject,
	UTextureRenderTarget2D* HeightRT,
	int32 GridSize,
	float HeightScale,
	float WorldOriginX,
	float WorldOriginY)
{
	UProceduralLandmassMeshCS_AsyncExecution* Action =
		NewObject<UProceduralLandmassMeshCS_AsyncExecution>();
	Action->HeightRT     = HeightRT;
	Action->GridSize     = GridSize;
	Action->HeightScale  = HeightScale;
	Action->WorldOriginX = WorldOriginX;
	Action->WorldOriginY = WorldOriginY;
	Action->RegisterWithGameInstance(WorldContextObject);

	return Action;
}
