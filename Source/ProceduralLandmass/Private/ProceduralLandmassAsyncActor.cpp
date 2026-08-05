// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassAsyncActor.h"
#include "ProceduralLandmass.h"
#include "ProceduralLandmassBPLibrary.h"
#include "TerrainMeshData.h"
#include "TerrainMeshGenCS.h"
#include "ProceduralLandmassMeshCS_AsyncExecution.h"
#include "ProceduralLandmassMeshShader_AsyncExecution.h"
#include "TerrainMeshSceneViewExtension.h"
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

	// Release the view extension — this unregisters it from the render pipeline
	if (MeshShaderViewExtension.IsValid())
	{
		MeshShaderViewExtension->SetEnabled(false);
		MeshShaderViewExtension.Reset();
	}

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

	if (bUseMeshShaderRendering)
	{
		PipelineStep_MeshShader(MoveTemp(Snapshot), Ticket);
	}
	else if (bUseGPUMeshGeneration)
	{
		PipelineStep_GPUMesh(MoveTemp(Snapshot), Ticket);
	}
	else
	{
		PipelineStep_NoiseMap(MoveTemp(Snapshot), Ticket);
	}
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

void AProceduralLandmassAsyncActor::PipelineStep_GPUMesh(const FGenSnapshot& Snapshot, uint32 Ticket)
{
	bIsGenerating = true;

	const TWeakObjectPtr<AProceduralLandmassAsyncActor> WeakThis(this);

	// Step 1: Generate noise on GPU into a temporary RenderTarget
	UTextureRenderTarget2D* HeightRT = NewObject<UTextureRenderTarget2D>(this);
	HeightRT->RenderTargetFormat = RTF_R32f;
	HeightRT->InitAutoFormat(Snapshot.ChunkSize, Snapshot.ChunkSize);
	HeightRT->UpdateResourceImmediate(true);

	// Dispatch noise compute shader
	FProceduralLandmassNoiseCSParameters NoiseParams(
		Snapshot.ChunkSize, Snapshot.Scale, Snapshot.Octaves,
		Snapshot.Persistence, Snapshot.Lacunarity, Snapshot.Seed,
		Snapshot.Offset);
	NoiseParams.RenderTarget = HeightRT->GameThread_GetRenderTargetResource();
	FProceduralLandmassNoiseCSInterface::Dispatch(NoiseParams);

	// Step 2: Generate mesh from height RT on GPU, read back to CPU
	FTerrainMeshGenParameters MeshParams;
	MeshParams.GridSize           = Snapshot.ChunkSize;
	MeshParams.HeightScale        = Snapshot.HeightScale;
	MeshParams.WorldOrigin        = FVector2f(Snapshot.Offset);
	MeshParams.HeightRenderTarget = HeightRT->GameThread_GetRenderTargetResource();

	FLODMeshData LODData;
	if (!FTerrainMeshGenCSInterface::DispatchAndReadback(MeshParams, LODData))
	{
		bIsGenerating = false;
		HeightRT = nullptr;
		return;
	}

	// Clean up temporary RT
	HeightRT = nullptr;

	// Ticket check after GPU work
	if (bDestroying || Ticket != GenerationTicket.load())
	{
		bIsGenerating = false;
		return;
	}

	// Step 3: Build UStaticMesh from GPU-generated data on game thread
	UTerrainMeshData* MeshData = NewObject<UTerrainMeshData>();
	MeshData->Init(Snapshot.ChunkSize, 1);
	FLODMeshData& LOD0 = MeshData->BeginLOD(0, Snapshot.ChunkSize);
	LOD0.Vertices  = MoveTemp(LODData.Vertices);
	LOD0.UVs       = MoveTemp(LODData.UVs);
	LOD0.Triangles  = MoveTemp(LODData.Triangles);
	LOD0.ChunkSize  = Snapshot.ChunkSize;
	// Also populate backward-compat arrays
	MeshData->Vertices  = LOD0.Vertices;
	MeshData->UVs       = LOD0.UVs;
	MeshData->Triangles = LOD0.Triangles;
	MeshData->ChunkSize = Snapshot.ChunkSize;

	GeneratedMesh = MeshData->CreateMesh();
	if (!GeneratedMesh)
	{
		bIsGenerating = false;
		return;
	}

	TerrainMeshComponent->SetStaticMesh(GeneratedMesh);

	// Step 4: Apply material with GPU color texture (same as CPU path)
	// Build a dummy noise map for the material step (only used for color lookup)
	TArray<float> DummyNoiseMap;  // Not needed since GPU noise goes directly to RT
	DummyNoiseMap.SetNum(Snapshot.ChunkSize * Snapshot.ChunkSize);

	// Fill with zeros — the material step will use GPU CS for actual colors
	for (int32 i = 0; i < DummyNoiseMap.Num(); ++i)
	{
		DummyNoiseMap[i] = 0.0f;
	}

	PipelineStep_ApplyMaterial(Snapshot, DummyNoiseMap);
}

void AProceduralLandmassAsyncActor::PipelineStep_MeshShader(FGenSnapshot Snapshot, uint32 Ticket)
{
	bIsGenerating = true;

	// ── Create or reuse persistent height RT ───────────────────────────────
	if (!PersistentHeightRT)
	{
		PersistentHeightRT = NewObject<UTextureRenderTarget2D>(this);
		PersistentHeightRT->RenderTargetFormat = RTF_R32f;
		PersistentHeightRT->InitAutoFormat(Snapshot.ChunkSize, Snapshot.ChunkSize);
		PersistentHeightRT->UpdateResourceImmediate(true);
	}
	else if (PersistentHeightRT->SizeX != Snapshot.ChunkSize ||
	         PersistentHeightRT->SizeY != Snapshot.ChunkSize)
	{
		PersistentHeightRT->InitAutoFormat(Snapshot.ChunkSize, Snapshot.ChunkSize);
		PersistentHeightRT->UpdateResourceImmediate(true);
	}

	FlushRenderingCommands();

	// ── Generate noise height RT on GPU ────────────────────────────────────
	FProceduralLandmassNoiseCSParameters NoiseParams(
		Snapshot.ChunkSize, Snapshot.Scale, Snapshot.Octaves,
		Snapshot.Persistence, Snapshot.Lacunarity, Snapshot.Seed,
		Snapshot.Offset);
	NoiseParams.RenderTarget = PersistentHeightRT->GameThread_GetRenderTargetResource();
	FProceduralLandmassNoiseCSInterface::Dispatch(NoiseParams);

	// Ensure GPU noise is complete before the view extension samples it
	FlushRenderingCommands();

	// ── Ticket check ───────────────────────────────────────────────────────
	if (bDestroying || Ticket != GenerationTicket.load())
	{
		bIsGenerating = false;
		return;
	}

	// ── Setup view extension with the new height RT ────────────────────────
	SetupMeshShaderViewExt();

	if (MeshShaderViewExtension.IsValid())
	{
		MeshShaderViewExtension->UpdateHeightmap(PersistentHeightRT);
		MeshShaderViewExtension->UpdateParams(
			Snapshot.ChunkSize, Snapshot.HeightScale,
			FVector2f(Snapshot.Offset));
		MeshShaderViewExtension->SetEnabled(true);
	}

	// ── Apply material (if set) to the empty mesh component ────────────────
	// The static mesh component is NOT used for rendering when mesh shader is active,
	// but apply material as a fallback visual for non-SM6 platforms.
	if (Snapshot.Material.IsValid())
	{
		TerrainMeshComponent->SetMaterial(0, Snapshot.Material.Get());
	}

	bIsGenerating = false;
}

void AProceduralLandmassAsyncActor::SetupMeshShaderViewExt()
{
	if (MeshShaderViewExtension.IsValid())
	{
		return; // Already registered
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	MeshShaderViewExtension =
		FSceneViewExtensions::NewExtension<FTerrainMeshSceneViewExtension>(World);

	UE_LOG(LogTemp, Log,
		TEXT("ProceduralLandmassAsyncActor: Registered FTerrainMeshSceneViewExtension for world %s"),
		*World->GetName());
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
