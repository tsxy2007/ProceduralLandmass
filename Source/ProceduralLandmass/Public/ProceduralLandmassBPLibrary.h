// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "ProceduralLandmassBPLibrary.generated.h"

UENUM(BlueprintType)
enum class ENoiseDrawMode : uint8
{
	ENDM_Noise,
	ENDM_Color,
};

USTRUCT(BlueprintType)
struct FTerrainType 
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FString Name;

	UPROPERTY(EditAnywhere)
	float NoiseHeight;

	UPROPERTY(EditAnywhere)
	FLinearColor Color;
};



/* 
*	Function library class.
*	Each function in it is expected to be static and represents blueprint node that can be called in any blueprint.
*
*	When declaring function you can define metadata for the node. Key function specifiers will be BlueprintPure and BlueprintCallable.
*	BlueprintPure - means the function does not affect the owning object in any way and thus creates a node without Exec pins.
*	BlueprintCallable - makes a function which can be executed in Blueprints - Thus it has Exec pins.
*	DisplayName - full name of the node, shown when you mouse over the node and in the blueprint drop down menu.
*				Its lets you name the node using characters not allowed in C++ function names.
*	CompactNodeTitle - the word(s) that appear on the node.
*	Keywords -	the list of keywords that helps you to find node when you search for it using Blueprint drop-down menu. 
*				Good example is "Print String" node which you can find also by using keyword "log".
*	Category -	the category your node will be under in the Blueprint drop-down menu.
*
*	For more info on custom blueprint nodes visit documentation:
*	https://wiki.unrealengine.com/Custom_Blueprint_Node_Creation
*/
UCLASS()
class UProceduralLandmassBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Execute Sample function", Keywords = "ProceduralLandmass sample test testing"), Category = "ProceduralLandmassTesting")
	static float ProceduralLandmassSampleFunction(float Param);


	UFUNCTION(BlueprintCallable)
	static void GenerateNoiseMap(int32 Width, int32 Height, float Scale,int32 octaves,float persistance,float lacunarity, int32 Seed,const FVector2D& Offset, TArray<float>& OutNoiseMap);


	UFUNCTION(BlueprintCallable)
	static UTexture2D* GenerateNoiseTexture(int32 Width, int32 Height, const TArray<float>& InNoiseMap);

	UFUNCTION(BlueprintCallable)
	static UTexture2D* GenerateColorNoiseTexture(int32 Width, int32 Height, const TArray<FTerrainType>& TerrainTypes, const TArray<float>& InNoiseMap);

	UFUNCTION(BlueprintCallable)
	static void GenerateTerrainMesh(int32 Width, int32 Height, const TArray<float>& InNoiseMap);
};
