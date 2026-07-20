// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassBPLibrary.h"
#include "ProceduralLandmass.h"
#include "TerrainMeshData.h"

UProceduralLandmassBPLibrary::UProceduralLandmassBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

float UProceduralLandmassBPLibrary::ProceduralLandmassSampleFunction(float Param)
{
	return -1;
}

void UProceduralLandmassBPLibrary::GenerateNoiseMap(int32 ChunkSize, float Scale, int32 octaves, float persistance, float lacunarity, int32 Seed, const FVector2D& Offset, TArray<float>& OutNoiseMap)
{
	// Implementation for generating noise map
	OutNoiseMap.SetNum(ChunkSize * ChunkSize);

	TArray<FVector2D> octaveOffsets;
	octaveOffsets.Reserve(octaves);

	FMath::RandInit(Seed);
	for (int32 i = 0; i < octaves; i++)
	{
		float offsetX = FMath::RandRange(-100000.f, 100000.f);
		float offsetY = FMath::RandRange(-100000.f, 100000.f);
		octaveOffsets.Add({ offsetX,offsetY });
	}

	if (Scale <= 0)
	{
		Scale = 0.0001f;
	}
	float halfSize = ChunkSize / 2.0f;

	// Precompute theoretical amplitude range for consistent normalization across chunks.
	// fBm with octaves layers and persistance has range [-sum, sum] where sum = Σ persistence^i.
	// Using a fixed range ensures adjacent chunks produce identical values at shared edges.
	float theoreticalMax = 0.0f;
	{
		float amp = 1.0f;
		for (int32 i = 0; i < octaves; i++)
		{
			theoreticalMax += amp;
			amp *= persistance;
		}
	}

	for (int32 y = 0; y < ChunkSize; y++)
	{
		for (int32 x = 0; x < ChunkSize; x++)
		{
			float amplitude = 1.0f;
			float frequency = 1.0f;
			float noiseHeight = 0.0f;
			for (int32 i = 0; i < octaves; i++)
			{
				float sampleX = (x - halfSize + Offset.X) / Scale * frequency + octaveOffsets[i].X;
				float sampleY = (y - halfSize + Offset.Y) / Scale * frequency + octaveOffsets[i].Y;

				FVector2D sample(sampleX, sampleY);
				float perlinValue = FMath::PerlinNoise2D(sample);
				noiseHeight += perlinValue * amplitude;
				amplitude *= persistance;
				frequency *= lacunarity;
			}

			// Normalize by theoretical range: map [-theoreticalMax, theoreticalMax] → [0, 1]
			float normalizedValue = 0.5f;
			if (theoreticalMax > 0.0f)
			{
				normalizedValue = (noiseHeight / theoreticalMax + 1.0f) / 2.0f;
			}

			OutNoiseMap[y * ChunkSize + x] = normalizedValue;
		}
	}
}

UTexture2D* UProceduralLandmassBPLibrary::GenerateNoiseTexture(int32 ChunkSize, const TArray<float>& OutNoiseMap)
{
	UTexture2D* noiseTexture = UTexture2D::CreateTransient(ChunkSize, ChunkSize);
	FTexture2DMipMap& mip = noiseTexture->GetPlatformData()->Mips[0];
	void* MipData = mip.BulkData.Lock(LOCK_READ_WRITE);
	check(MipData);
	FColor* Pixels = static_cast<FColor*>(MipData);
	for (int32 y = 0; y < ChunkSize; ++y)
	{
		for (int32 x = 0; x < ChunkSize; ++x)
		{
			int32 NoiseIndex = y * ChunkSize + x;
			float alpha = OutNoiseMap[NoiseIndex];
			const FLinearColor PixelLinear = FMath::Lerp(FLinearColor::Black,
				FLinearColor::White, alpha);
		Pixels[NoiseIndex] = PixelLinear.ToFColor(true);
		}
	}

	mip.BulkData.Unlock();
	noiseTexture->UpdateResource();
	return noiseTexture;
}

UTexture2D* UProceduralLandmassBPLibrary::GenerateColorNoiseTexture(int32 ChunkSize, const TArray<FTerrainType>& TerrainTypes, const TArray<float>& OutNoiseMap)
{
	UTexture2D* noiseTexture = UTexture2D::CreateTransient(ChunkSize, ChunkSize);
	FTexture2DMipMap& mip = noiseTexture->GetPlatformData()->Mips[0];
	void* MipData = mip.BulkData.Lock(LOCK_READ_WRITE);
	check(MipData);
	FColor* Pixels = static_cast<FColor*>(MipData);
	for (int32 y = 0; y < ChunkSize; ++y)
	{
		for (int32 x = 0; x < ChunkSize; ++x)
		{
			int32 NoiseIndex = y * ChunkSize + x;
			float alpha = OutNoiseMap[NoiseIndex];

			bool bMatched = false;
			for (const FTerrainType& terrainType : TerrainTypes)
			{
				if (alpha <= terrainType.NoiseHeight)
				{
					Pixels[NoiseIndex] = terrainType.Color.ToFColor(true);
					bMatched = true;
					break;
				}
			}
			// Fallback: if no terrain matched, default to black
			if (!bMatched)
			{
				Pixels[NoiseIndex] = FColor::Black;
			}
		}
	}

	mip.BulkData.Unlock();
	noiseTexture->UpdateResource();
	return noiseTexture;
}

UTerrainMeshData* UProceduralLandmassBPLibrary::GenerateTerrainMesh(int32 ChunkSize, float Scale, const TArray<float>& InNoiseMap, UCurveFloat* HeightCurve, int32 LODLevels)
{
	UTerrainMeshData* meshData = NewObject<UTerrainMeshData>();
	meshData->Init(ChunkSize, LODLevels);

	for (int32 LODIndex = 0; LODIndex < LODLevels; ++LODIndex)
	{
		const int32 Step = 1 << LODIndex;
		const int32 LodSize = ((ChunkSize - 1) / Step) + 1;

		FLODMeshData& LOD = meshData->BeginLOD(LODIndex, LodSize);

		const float topLeftX = (LodSize - 1) / -2.0f;
		const float topLeftY = (LodSize - 1) / 2.0f;

		int32 vertexIndex = 0;
		for (int32 y = 0; y < LodSize; y++)
		{
			for (int32 x = 0; x < LodSize; x++)
			{
				const int32 SampleX = x * Step;
				const int32 SampleY = y * Step;
				const float noiseValue = InNoiseMap[SampleY * ChunkSize + SampleX];

				float heightValue = noiseValue * Scale;
				if (HeightCurve)
				{
					heightValue = HeightCurve->GetFloatValue(noiseValue) * Scale;
				}

				LOD.Vertices[vertexIndex] = FVector(x + topLeftX, topLeftY - y, heightValue);
				LOD.UVs[vertexIndex] = FVector2D(x / (float)LodSize, y / (float)LodSize);

				if (x < LodSize - 1 && y < LodSize - 1)
				{
					meshData->AddTriangleToLOD(LOD, vertexIndex, vertexIndex + LodSize + 1, vertexIndex + LodSize);
					meshData->AddTriangleToLOD(LOD, vertexIndex + LodSize + 1, vertexIndex, vertexIndex + 1);
				}
				vertexIndex++;
			}
		}
	}

	// Populate backward-compatible LOD 0 arrays
	if (LODLevels > 0 && meshData->LODData.Num() > 0)
	{
		const FLODMeshData& LOD0 = meshData->LODData[0];
		meshData->ChunkSize = LOD0.ChunkSize;
		meshData->Vertices = LOD0.Vertices;
		meshData->UVs = LOD0.UVs;
		meshData->Triangles = LOD0.Triangles;
	}

	return meshData;
}
