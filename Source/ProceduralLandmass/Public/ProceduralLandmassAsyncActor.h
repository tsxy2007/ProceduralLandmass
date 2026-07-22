// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralLandmassBPLibrary.h"
#include "ProceduralLandmassAsyncActor.generated.h"

class UStaticMeshComponent;
class UCurveFloat;
class UMaterialInterface;

/**
 * Async variant of AProceduralLandmassActor.
 *
 * The CPU-heavy noise map generation runs on a background thread. When it completes,
 * the work is marshalled back to the game thread to build the UStaticMesh (UObject
 * creation and BuildFromMeshDescriptions must happen on the game thread) and apply
 * the material to the mesh component.
 *
 * A monotonically increasing generation ticket is used to ignore stale results from
 * superseded requests, and a TWeakObjectPtr guards against the actor being destroyed
 * before the background task finishes.
 */
UCLASS()
class PROCEDURALLANDMASS_API AProceduralLandmassAsyncActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralLandmassAsyncActor();

	/** Kick off an asynchronous terrain generation. Safe to call again while one is in flight; the newest request wins. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "ProceduralLandmass")
	void GenerateTerrainAsync();

	/** True if a background generation request is still pending or in flight. */
	UFUNCTION(BlueprintPure, Category = "ProceduralLandmass")
	bool IsGenerating() const { return bIsGenerating; }

protected:
	virtual void PostRegisterAllComponents() override;
	virtual void BeginDestroy() override;

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
	UPROPERTY(EditAnywhere, Category = "Noise")
	float HeightScale = 1000.0f;

	UPROPERTY(EditAnywhere, Category = "Terrain")
	TObjectPtr<UCurveFloat> HeightCurve;

	UPROPERTY(EditAnywhere, Category = "Terrain", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LODLevels = 1;

	// ---- Appearance ----

	UPROPERTY(EditAnywhere, Category = "Appearance")
	TObjectPtr<UMaterialInterface> Material;

	UPROPERTY(EditAnywhere, Category = "Appearance")
	TArray<FTerrainType> TerrainTypes;

private:
	struct FGenSnapshot
	{
		int32 ChunkSize = 128;
		float Scale = 50.0f;
		int32 Octaves = 6;
		float Persistence = 0.5f;
		float Lacunarity = 2.0f;
		int32 Seed = 0;
		FVector2D Offset = FVector2D::ZeroVector;
		float HeightScale = 1000.0f;
		TWeakObjectPtr<UCurveFloat> HeightCurve;
		int32 LODLevels = 1;
		TWeakObjectPtr<UMaterialInterface> Material;
		TArray<FTerrainType> TerrainTypes;
	};

	void StartBackgroundGeneration(FGenSnapshot Snapshot, uint32 Ticket);
	void ApplyGeneratedNoise(const TArray<float>& NoiseMap, const FGenSnapshot& Snapshot, uint32 Ticket);
	void ApplyMaterial(const FGenSnapshot& Snapshot, const TArray<float>& NoiseMap);

	/** True while a background task is in flight. Atomically read/written from multiple threads. */
	FThreadSafeBool bIsGenerating;

	/** Incremented each time a new generation is requested. Results from older tickets are discarded. */
	std::atomic<uint32> GenerationTicket{ 0 };

	/** Set on BeginDestroy so in-flight background tasks skip the game-thread callback. */
	FThreadSafeBool bDestroying;

	// Generated mesh kept alive by UPROPERTY
	UPROPERTY()
	TObjectPtr<UStaticMesh> GeneratedMesh;
};
