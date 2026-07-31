// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassAsyncActor.h"
#include "ProceduralLandmass.h"
#include "ProceduralLandmassBPLibrary.h"
#include "TerrainMeshData.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Async/Async.h"
#include "ProceduralLandmassNoiseCS_AsyncExecution.h"

AProceduralLandmassAsyncActor::AProceduralLandmassAsyncActor()
{
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	TerrainMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TerrainMesh"));
	TerrainMeshComponent->SetupAttachment(Root);
}

void AProceduralLandmassAsyncActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

#if WITH_EDITOR
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		GenerateTerrainAsync();
	}
#endif
}

void AProceduralLandmassAsyncActor::BeginDestroy()
{
	bDestroying = true;
	Super::BeginDestroy();
}

void AProceduralLandmassAsyncActor::GenerateTerrainAsync()
{
	FGenSnapshot Snapshot;
	Snapshot.ChunkSize = ChunkSize;
	Snapshot.Scale = Scale;
	Snapshot.Octaves = Octaves;
	Snapshot.Persistence = Persistence;
	Snapshot.Lacunarity = Lacunarity;
	Snapshot.Seed = Seed;
	Snapshot.Offset = Offset;
	Snapshot.HeightScale = HeightScale;
	Snapshot.HeightCurve = HeightCurve;
	Snapshot.LODLevels = FMath::Clamp(LODLevels, 1, 8);
	Snapshot.Material = Material;
	Snapshot.TerrainTypes = TerrainTypes;

	const uint32 Ticket = ++GenerationTicket;
	PipelineStep_NoiseMap(MoveTemp(Snapshot), Ticket);
}

void AProceduralLandmassAsyncActor::PipelineStep_NoiseMap(FGenSnapshot Snapshot, uint32 Ticket)
{
	bIsGenerating = true;

	FOnNoiseMapGenerated OnComplete;
	const TWeakObjectPtr<AProceduralLandmassAsyncActor> WeakThis(this);

	OnComplete.BindLambda([WeakThis, Snapshot = MoveTemp(Snapshot), Ticket](const TArray<float>& NoiseMap)
	{
		if (AProceduralLandmassAsyncActor* This = WeakThis.Get())
		{
			if (This->bDestroying || Ticket != This->GenerationTicket.load())
			{
				This->bIsGenerating = false;
				return;
			}

			if (NoiseMap.Num() == 0)
			{
				This->bIsGenerating = false;
				return;
			}

			This->PipelineStep_TerrainMesh(NoiseMap, Snapshot, Ticket);
		}
	});

	UProceduralLandmassBPLibrary::GenerateNoiseMapAsync(
		Snapshot.ChunkSize, Snapshot.Scale, Snapshot.Octaves, Snapshot.Persistence,
		Snapshot.Lacunarity, Snapshot.Seed, Snapshot.Offset, OnComplete);
}

void AProceduralLandmassAsyncActor::PipelineStep_TerrainMesh(const TArray<float>& NoiseMap, const FGenSnapshot& Snapshot, uint32 Ticket)
{
	if (bDestroying || Ticket != GenerationTicket.load())
	{
		bIsGenerating = false;
		return;
	}

	const TWeakObjectPtr<AProceduralLandmassAsyncActor> WeakThis(this);

	FOnTerrainMeshGenerated OnComplete;
	OnComplete.BindLambda([WeakThis, Snapshot, NoiseMap, Ticket](UTerrainMeshData* MeshData)
	{
		if (AProceduralLandmassAsyncActor* This = WeakThis.Get())
		{
			if (This->bDestroying || Ticket != This->GenerationTicket.load())
			{
				This->bIsGenerating = false;
				return;
			}

			if (!MeshData)
			{
				This->bIsGenerating = false;
				return;
			}

			This->GeneratedMesh = MeshData->CreateMesh();
			if (!This->GeneratedMesh)
			{
				This->bIsGenerating = false;
				return;
			}

			This->TerrainMeshComponent->SetStaticMesh(This->GeneratedMesh);
			This->PipelineStep_ApplyMaterial(Snapshot, NoiseMap);
		}
	});

	UProceduralLandmassBPLibrary::GenerateTerrainMeshAsync(
		Snapshot.ChunkSize, Snapshot.HeightScale, NoiseMap,
		Snapshot.HeightCurve.Get(), Snapshot.LODLevels, OnComplete);
}

void AProceduralLandmassAsyncActor::PipelineStep_ApplyMaterial(const FGenSnapshot& Snapshot, const TArray<float>& NoiseMap)
{
	if (!Snapshot.Material.IsValid())
	{
		bIsGenerating = false;
		return;
	}

	UMaterialInterface* MaterialToApply = Snapshot.Material.Get();

	if (Snapshot.TerrainTypes.Num() > 0)
	{
		// Create transient RGBA8 RenderTarget for GPU color output
		UTextureRenderTarget2D* ColorRT = NewObject<UTextureRenderTarget2D>(this);
		ColorRT->RenderTargetFormat = RTF_RGBA8;
		ColorRT->InitAutoFormat(Snapshot.ChunkSize, Snapshot.ChunkSize);
		ColorRT->UpdateResourceImmediate(true);

		// Dispatch GPU compute shader — generates noise height + color texture
		FProceduralLandmassNoiseCSParameters CSParams(
			Snapshot.ChunkSize, Snapshot.Scale, Snapshot.Octaves,
			Snapshot.Persistence, Snapshot.Lacunarity, Snapshot.Seed,
			Snapshot.Offset);

		// Height output RT (must match ChunkSize × ChunkSize, PF_R32_FLOAT)
		UTextureRenderTarget2D* HeightRT = NewObject<UTextureRenderTarget2D>(this);
		HeightRT->RenderTargetFormat = RTF_R32f;
		HeightRT->InitAutoFormat(Snapshot.ChunkSize, Snapshot.ChunkSize);
		HeightRT->UpdateResourceImmediate(true);

		CSParams.RenderTarget = HeightRT->GameThread_GetRenderTargetResource();
		CSParams.ColorRenderTarget = ColorRT->GameThread_GetRenderTargetResource();
		CSParams.TerrainTypes = Snapshot.TerrainTypes;

		FProceduralLandmassNoiseCSInterface::Dispatch(CSParams);

		// Block until GPU finishes (required: RT must be ready for material)
		FlushRenderingCommands();

		UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(MaterialToApply, this);
		DynMat->SetTextureParameterValue(TEXT("TerrainColorTexture"), ColorRT);
		TerrainMeshComponent->SetMaterial(0, DynMat);

		// Clean up temporary height RT (not needed after material is applied)
		HeightRT = nullptr;
		bIsGenerating = false;
		return;
	}

	TerrainMeshComponent->SetMaterial(0, MaterialToApply);
	bIsGenerating = false;
}
