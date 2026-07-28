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
 * Async variant of AProceduralLandmassActor using a pipeline + ticket queue pattern.
 *
 * Pipeline: NoiseMap (background thread) -> TerrainMesh (game thread) -> ApplyMaterial (game thread).
 * Each step checks a monotonically increasing ticket -- if a newer request supersedes
 * the current one, the old pipeline discards itself. A TWeakObjectPtr guards against
 * actor destruction mid-flight.
 *
 * Calling GenerateTerrainAsync() while a pipeline is already in flight bumps the
 * ticket; the old pipeline naturally drains and the new one starts immediately.
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

	/** Pipeline step 1: kick off background noise generation. */
	void PipelineStep_NoiseMap(FGenSnapshot Snapshot, uint32 Ticket);

	/** Pipeline step 2: build terrain mesh from noise data (game thread). */
	void PipelineStep_TerrainMesh(const TArray<float>& NoiseMap, const FGenSnapshot& Snapshot, uint32 Ticket);

	/** Pipeline step 3: apply material with optional color texture (game thread). */
	void PipelineStep_ApplyMaterial(const FGenSnapshot& Snapshot, const TArray<float>& NoiseMap);

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
