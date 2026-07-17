// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "TerrainMeshData.generated.h"

/**
 * 
 */
UCLASS()
class PROCEDURALLANDMASS_API UTerrainMeshData : public UObject
{
	GENERATED_BODY()
	
public:
	UTerrainMeshData(const FObjectInitializer& ObjectInitializer);

public:
	UPROPERTY(EditAnywhere)
	TArray<FVector> Vertices;

	UPROPERTY(EditAnywhere)
	TArray<FVector> UVs;

	UPROPERTY(EditAnywhere)
	TArray<int32> Triangles;

	UPROPERTY(EditAnywhere)
	int32 MeshWidth;

	UPROPERTY(EditAnywhere)
	int32 MeshHeight;
};
