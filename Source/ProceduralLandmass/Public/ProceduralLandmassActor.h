// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralLandmassBPLibrary.h"
#include "ProceduralLandmassActor.generated.h"

class UStaticMeshComponent;
class UCurveFloat;
class UMaterialInterface;

UCLASS()
class PROCEDURALLANDMASS_API AProceduralLandmassActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralLandmassActor();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "ProceduralLandmass")
	void GenerateTerrain();

protected:
	virtual void PostRegisterAllComponents() override;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> TerrainMeshComponent;

	// ---- Noise parameters ----

	UPROPERTY(EditAnywhere, Category = "Noise")
	int32 ChunkSize = 128;

	UPROPERTY(EditAnywhere, Category = "Noise")
	float Scale = 50.0f;

	UPROPERTY(EditAnywhere, Category = "Noise")
	int32 Octaves = 6;

	UPROPERTY(EditAnywhere, Category = "Noise")
	float Persistence = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Noise")
	float Lacunarity = 2.0f;

	UPROPERTY(EditAnywhere, Category = "Noise")
	int32 Seed = 0;

	UPROPERTY(EditAnywhere, Category = "Noise")
	FVector2D Offset = FVector2D::ZeroVector;

	// ---- Terrain shaping ----

	UPROPERTY(EditAnywhere, Category = "Terrain")
	TObjectPtr<UCurveFloat> HeightCurve;

	UPROPERTY(EditAnywhere, Category = "Terrain", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LODLevels = 1;

	// ---- Appearance ----

	UPROPERTY(EditAnywhere, Category = "Appearance")
	TObjectPtr<UMaterialInterface> Material;

	UPROPERTY(EditAnywhere, Category = "Appearance")
	TArray<FTerrainType> TerrainTypes;

	// Generated mesh kept alive by UPROPERTY
	UPROPERTY()
	TObjectPtr<UStaticMesh> GeneratedMesh;
};
