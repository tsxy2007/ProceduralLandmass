// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassActor.h"
#include "ProceduralLandmass.h"
#include "ProceduralLandmassBPLibrary.h"
#include "TerrainMeshData.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

AProceduralLandmassActor::AProceduralLandmassActor()
{
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	TerrainMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TerrainMesh"));
	TerrainMeshComponent->SetupAttachment(Root);
}

void AProceduralLandmassActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// Only auto-generate in editor, not at runtime (call GenerateTerrain from BP/game code at runtime)
#if WITH_EDITOR
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		GenerateTerrain();
	}
#endif
}

void AProceduralLandmassActor::GenerateTerrain()
{
	// 1. Generate noise map
	TArray<float> NoiseMap;
	UProceduralLandmassBPLibrary::GenerateNoiseMap(ChunkSize, Scale, Octaves, Persistence, Lacunarity, Seed, Offset, NoiseMap);

	if (NoiseMap.Num() == 0)
	{
		return;
	}

	// 2. Build terrain mesh data
	UTerrainMeshData* MeshData = UProceduralLandmassBPLibrary::GenerateTerrainMesh(ChunkSize, Scale, NoiseMap, HeightCurve, LODLevels);
	if (!MeshData)
	{
		return;
	}

	// 3. Create static mesh
	GeneratedMesh = MeshData->CreateMesh();
	if (!GeneratedMesh)
	{
		return;
	}

	TerrainMeshComponent->SetStaticMesh(GeneratedMesh);

	// 4. Apply material (with color texture if terrain types are defined)
	if (Material)
	{
		if (TerrainTypes.Num() > 0)
		{
			UTexture2D* ColorTexture = UProceduralLandmassBPLibrary::GenerateColorNoiseTexture(ChunkSize, TerrainTypes, NoiseMap);
			if (ColorTexture)
			{
				UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(Material, this);
				DynMat->SetTextureParameterValue(TEXT("TerrainColorTexture"), ColorTexture);
				TerrainMeshComponent->SetMaterial(0, DynMat);
			}
			else
			{
				TerrainMeshComponent->SetMaterial(0, Material);
			}
		}
		else
		{
			TerrainMeshComponent->SetMaterial(0, Material);
		}
	}
}
