// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassAsyncActor.h"
#include "ProceduralLandmass.h"
#include "ProceduralLandmassBPLibrary.h"
#include "TerrainMeshData.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Async/Async.h"

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
	Snapshot.LODLevels = LODLevels;
	Snapshot.Material = Material;
	Snapshot.TerrainTypes = TerrainTypes;

	const uint32 Ticket = ++GenerationTicket;
	StartBackgroundGeneration(MoveTemp(Snapshot), Ticket);
}

void AProceduralLandmassAsyncActor::StartBackgroundGeneration(FGenSnapshot Snapshot, uint32 Ticket)
{
	bIsGenerating = true;

	// Capture a weak pointer so the background task can bail if the actor is destroyed.
	const TWeakObjectPtr<AProceduralLandmassAsyncActor> WeakThis(this);

	Async(EAsyncExecution::Thread, [WeakThis, Snapshot = MoveTemp(Snapshot), Ticket]()
	{
		TArray<float> NoiseMap;
		UProceduralLandmassBPLibrary::GenerateNoiseMap(
			Snapshot.ChunkSize, Snapshot.Scale, Snapshot.Octaves, Snapshot.Persistence,
			Snapshot.Lacunarity, Snapshot.Seed, Snapshot.Offset, NoiseMap);

		// Marshal back to the game thread to create the UStaticMesh and update the component.
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Snapshot, NoiseMap = MoveTemp(NoiseMap), Ticket]()
		{
			if (AProceduralLandmassAsyncActor* This = WeakThis.Get())
			{
				This->ApplyGeneratedNoise(NoiseMap, Snapshot, Ticket);
			}
		});
	});
}

void AProceduralLandmassAsyncActor::ApplyGeneratedNoise(const TArray<float>& NoiseMap, const FGenSnapshot& Snapshot, uint32 Ticket)
{
	bIsGenerating = false;

	// Discard stale results from superseded requests, and skip if the actor is tearing down.
	if (bDestroying || Ticket != GenerationTicket.load())
	{
		return;
	}

	if (NoiseMap.Num() == 0)
	{
		return;
	}

	UTerrainMeshData* MeshData = UProceduralLandmassBPLibrary::GenerateTerrainMesh(
		Snapshot.ChunkSize, Snapshot.HeightScale, NoiseMap,
		Snapshot.HeightCurve.Get(), Snapshot.LODLevels);
	if (!MeshData)
	{
		return;
	}

	GeneratedMesh = MeshData->CreateMesh();
	if (!GeneratedMesh)
	{
		return;
	}

	TerrainMeshComponent->SetStaticMesh(GeneratedMesh);

	ApplyMaterial(Snapshot, NoiseMap);
}

void AProceduralLandmassAsyncActor::ApplyMaterial(const FGenSnapshot& Snapshot, const TArray<float>& NoiseMap)
{
	if (!Snapshot.Material.IsValid())
	{
		return;
	}

	UMaterialInterface* MaterialToApply = Snapshot.Material.Get();

	if (Snapshot.TerrainTypes.Num() > 0)
	{
		if (UTexture2D* ColorTexture = UProceduralLandmassBPLibrary::GenerateColorNoiseTexture(
			Snapshot.ChunkSize, Snapshot.TerrainTypes, NoiseMap))
		{
			UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(MaterialToApply, this);
			DynMat->SetTextureParameterValue(TEXT("TerrainColorTexture"), ColorTexture);
			TerrainMeshComponent->SetMaterial(0, DynMat);
			return;
		}
	}

	TerrainMeshComponent->SetMaterial(0, MaterialToApply);
}
