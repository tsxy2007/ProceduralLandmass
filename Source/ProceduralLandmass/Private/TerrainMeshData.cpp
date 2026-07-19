// Fill out your copyright notice in the Description page of Project Settings.


#include "TerrainMeshData.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

UTerrainMeshData::UTerrainMeshData(const FObjectInitializer& ObjectInitializer)
	:Super(ObjectInitializer)
{
}

void UTerrainMeshData::Init(int32 InMeshWidth, int32 InMeshHeight, int32 InLODLevels)
{
	MeshWidth = InMeshWidth;
	MeshHeight = InMeshHeight;
	Vertices.SetNum(MeshWidth * MeshHeight);
	UVs.SetNum(MeshWidth * MeshHeight);
	Triangles.SetNum((MeshWidth - 1) * (MeshHeight - 1) * 6);

	LODData.SetNum(InLODLevels);
	for (int32 LODIndex = 0; LODIndex < InLODLevels; ++LODIndex)
	{
		const int32 Step = 1 << LODIndex;
		LODData[LODIndex].MeshWidth = ((MeshWidth - 1) / Step) + 1;
		LODData[LODIndex].MeshHeight = ((MeshHeight - 1) / Step) + 1;
		const int32 LodVertCount = LODData[LODIndex].MeshWidth * LODData[LODIndex].MeshHeight;
		const int32 LodTriCount = (LODData[LODIndex].MeshWidth - 1) * (LODData[LODIndex].MeshHeight - 1) * 6;
		LODData[LODIndex].Vertices.SetNum(LodVertCount);
		LODData[LODIndex].UVs.SetNum(LodVertCount);
		LODData[LODIndex].Triangles.Empty(LodTriCount);
	}
}

void UTerrainMeshData::AddTriangle(int32 a, int32 b, int32 c)
{
	Triangles[TriangleIndex] = a;
	Triangles[TriangleIndex + 1] = b;
	Triangles[TriangleIndex + 2] = c;
	TriangleIndex += 3;
}

FLODMeshData& UTerrainMeshData::BeginLOD(int32 LODIndex, int32 LodWidth, int32 LodHeight)
{
	FLODMeshData& LOD = LODData[LODIndex];
	LOD.MeshWidth = LodWidth;
	LOD.MeshHeight = LodHeight;
	LOD.Vertices.SetNum(LodWidth * LodHeight);
	LOD.UVs.SetNum(LodWidth * LodHeight);
	LOD.Triangles.Empty((LodWidth - 1) * (LodHeight - 1) * 6);
	return LOD;
}

void UTerrainMeshData::AddTriangleToLOD(FLODMeshData& LOD, int32 a, int32 b, int32 c)
{
	LOD.Triangles.Add(a);
	LOD.Triangles.Add(b);
	LOD.Triangles.Add(c);
}

UStaticMesh* UTerrainMeshData::CreateMesh()
{
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>();
	StaticMesh->InitResources();

	auto BuildMeshDescription = [](const FLODMeshData& Data) -> FMeshDescription
	{
		FMeshDescription MeshDescription;
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();

		TPolygonGroupAttributesRef<FName> PolygonGroupNames = Attributes.GetPolygonGroupMaterialSlotNames();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();

		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(Data.Vertices.Num());
		for (const FVector& Vertex : Data.Vertices)
		{
			FVertexID VertexID = MeshDescription.CreateVertex();
			VertexPositions[VertexID] = FVector3f(Vertex);
			VertexIDs.Add(VertexID);
		}

		FPolygonGroupID PolygonGroupID = MeshDescription.CreatePolygonGroup();
		PolygonGroupNames[PolygonGroupID] = FName("Default");

		for (int32 i = 0; i + 2 < Data.Triangles.Num(); i += 3)
		{
			const int32 Index0 = Data.Triangles[i];
			const int32 Index1 = Data.Triangles[i + 1];
			const int32 Index2 = Data.Triangles[i + 2];

			if (Index0 >= VertexIDs.Num() || Index1 >= VertexIDs.Num() || Index2 >= VertexIDs.Num())
			{
				continue;
			}

			FVertexInstanceID Instance0 = MeshDescription.CreateVertexInstance(VertexIDs[Index0]);
			FVertexInstanceID Instance1 = MeshDescription.CreateVertexInstance(VertexIDs[Index1]);
			FVertexInstanceID Instance2 = MeshDescription.CreateVertexInstance(VertexIDs[Index2]);

			if (Data.UVs.IsValidIndex(Index0))
			{
				VertexInstanceUVs.Set(Instance0, 0, FVector2f(Data.UVs[Index0]));
			}
			if (Data.UVs.IsValidIndex(Index1))
			{
				VertexInstanceUVs.Set(Instance1, 0, FVector2f(Data.UVs[Index1]));
			}
			if (Data.UVs.IsValidIndex(Index2))
			{
				VertexInstanceUVs.Set(Instance2, 0, FVector2f(Data.UVs[Index2]));
			}

			MeshDescription.CreateTriangle(PolygonGroupID, { Instance0, Instance1, Instance2 });
		}

		return MeshDescription;
	};

	if (LODData.Num() > 0)
	{
		TArray<const FMeshDescription*> MeshDescriptions;
		TArray<FMeshDescription> OwnedDescriptions;
		OwnedDescriptions.SetNum(LODData.Num());

		for (int32 LODIndex = 0; LODIndex < LODData.Num(); ++LODIndex)
		{
			if (LODData[LODIndex].Vertices.Num() == 0 || LODData[LODIndex].Triangles.Num() == 0)
			{
				return nullptr;
			}
			OwnedDescriptions[LODIndex] = BuildMeshDescription(LODData[LODIndex]);
			MeshDescriptions.Add(&OwnedDescriptions[LODIndex]);
		}

		StaticMesh->BuildFromMeshDescriptions(MeshDescriptions);
	}
	else
	{
		if (Vertices.Num() == 0 || Triangles.Num() == 0)
		{
			return nullptr;
		}

		FMeshDescription MeshDescription = BuildMeshDescription({Vertices, UVs, Triangles, MeshWidth, MeshHeight});
		TArray<const FMeshDescription*> MeshDescriptions;
		MeshDescriptions.Add(&MeshDescription);
		StaticMesh->BuildFromMeshDescriptions(MeshDescriptions);
	}

	return StaticMesh;
}
