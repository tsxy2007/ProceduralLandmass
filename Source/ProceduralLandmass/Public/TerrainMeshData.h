// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "TerrainMeshData.generated.h"

USTRUCT(BlueprintType)
struct FLODMeshData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	TArray<FVector> Vertices;

	UPROPERTY(EditAnywhere)
	TArray<FVector2D> UVs;

	UPROPERTY(EditAnywhere)
	TArray<int32> Triangles;

	UPROPERTY(EditAnywhere)
	int32 ChunkSize = 0;
};

/**
 *
 */
UCLASS()
class PROCEDURALLANDMASS_API UTerrainMeshData : public UObject
{
	GENERATED_BODY()

public:
	UTerrainMeshData(const FObjectInitializer& ObjectInitializer);

	void Init(int32 InChunkSize, int32 InLODLevels = 1);

	void AddTriangle(int32 a, int32 b, int32 c);

	FLODMeshData& BeginLOD(int32 LODIndex, int32 LodSize);
	void AddTriangleToLOD(FLODMeshData& LOD, int32 a, int32 b, int32 c);

	UFUNCTION(BlueprintCallable)
	UStaticMesh* CreateMesh();
public:
	// LOD 0 data (backward compatible)
	UPROPERTY(EditAnywhere)
	TArray<FVector> Vertices;

	UPROPERTY(EditAnywhere)
	TArray<FVector2D> UVs;

	UPROPERTY(EditAnywhere)
	TArray<int32> Triangles;

	UPROPERTY(EditAnywhere)
	int32 ChunkSize;

	UPROPERTY(EditAnywhere)
	int32 TriangleIndex = 0;

	// Multi-LOD data
	UPROPERTY(EditAnywhere)
	TArray<FLODMeshData> LODData;
};
