// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassBPLibrary.h"
#include "ProceduralLandmass.h"

UProceduralLandmassBPLibrary::UProceduralLandmassBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

float UProceduralLandmassBPLibrary::ProceduralLandmassSampleFunction(float Param)
{
	return -1;
}

void UProceduralLandmassBPLibrary::GenerateNoiseMap(int32 Width, int32 Height, float Scale, int32 octaves, float persistance, float lacunarity, int32 Seed, TArray<float>& OutNoiseMap)
{
	// Implementation for generating noise map
	OutNoiseMap.SetNum(Width * Height);

	if (Scale <= 0)
	{
		Scale = 0.0001f;
	}
	float halfWidth = Width / 2.0f;
	float halfHeight = Height / 2.0f;

	float maxNoiseHeight = TNumericLimits<float>::Lowest();
	float minNoiseHeight = TNumericLimits<float>::Max();

	for (int32 x = 0; x < Width; x++)
	{
		for (int32 y = 0; y < Height; y++)
		{
			float amplitude = 1.0f;
            float frequency = 1.0f;
			float noiseHeight = 0.0f;
			const float seedOffsetX = Seed * 0.137f;
			const float seedOffsetY = Seed * 0.247f;
			for (int32 i = 0; i < octaves; i++)
			{

				float sampleX = (x - halfWidth) / Scale * frequency + seedOffsetX;
				float sampleY = (y - halfHeight) / Scale * frequency + seedOffsetY;

				FVector2D sample(sampleX, sampleY);
				float perlinValue = FMath::PerlinNoise2D(sample);
				noiseHeight += perlinValue * amplitude;
				amplitude *= persistance;
				frequency *= lacunarity;
			}
			if (noiseHeight > maxNoiseHeight)
			{
				maxNoiseHeight = noiseHeight;
			}
			if (noiseHeight < minNoiseHeight)
			{
				minNoiseHeight = noiseHeight;
			}
			OutNoiseMap[y * Width + x] = noiseHeight;
		}
	}
	
	for (int32 y = 0; y < Height; y++)
	{
		for (int32 x = 0; x < Width; x++)
		{
			float noiseHeight = OutNoiseMap[y * Width + x];
			noiseHeight = (noiseHeight - minNoiseHeight) / (maxNoiseHeight - minNoiseHeight);
			OutNoiseMap[y * Width + x] = noiseHeight;
		}
	}
}

UTexture2D* UProceduralLandmassBPLibrary::GenerateNoiseTexture(int32 Width, int32 Height, const TArray<float>& OutNoiseMap)
{
	UTexture2D* noiseTexture = UTexture2D::CreateTransient(Width, Height);
	FTexture2DMipMap& mip = noiseTexture->GetPlatformData()->Mips[0];
	void* MipData = mip.BulkData.Lock(LOCK_READ_WRITE);
	check(MipData);
	FColor* Pixels = static_cast<FColor*>(MipData);
	for (int32 y = 0; y < Height; ++y)
	{
		for (int32 x = 0; x < Width; ++x)
		{
			int32 NoiseIndex = y * Width + x;
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
