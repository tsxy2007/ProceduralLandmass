// Copyright Epic Games, Inc. All Rights Reserved.

#include "GridUtils.h"

FGridCoordinate UGridUtils::WorldToGrid(FVector WorldPosition, float CellSize)
{
	if (CellSize <= 0.0f)
	{
		CellSize = 10000.0f;
	}

	const int32 GridX = FMath::FloorToInt(WorldPosition.X / CellSize);
	const int32 GridY = FMath::FloorToInt(WorldPosition.Y / CellSize);

	return FGridCoordinate(GridX, GridY);
}

FVector UGridUtils::GridToWorld(const FGridCoordinate& GridCoord, float CellSize, float Z)
{
	if (CellSize <= 0.0f)
	{
		CellSize = 10000.0f;
	}

	// 返回格子中心点
	const float WorldX = (GridCoord.X + 0.5f) * CellSize;
	const float WorldY = (GridCoord.Y + 0.5f) * CellSize;

	return FVector(WorldX, WorldY, Z);
}

FNineGridResult UGridUtils::GetNineGridCells(FVector PlayerPosition, float CellSize)
{
	const FGridCoordinate Center = WorldToGrid(PlayerPosition, CellSize);
	return GetNineGridCellsFromGrid(Center, CellSize);
}

FNineGridResult UGridUtils::GetNineGridCellsFromGrid(const FGridCoordinate& CenterGrid, float CellSize)
{
	FNineGridResult Result;
	FillNineGridResult(Result, CenterGrid, CellSize);
	return Result;
}

int32 UGridUtils::GridDistance(const FGridCoordinate& A, const FGridCoordinate& B)
{
	return FMath::Max(FMath::Abs(A.X - B.X), FMath::Abs(A.Y - B.Y));
}

bool UGridUtils::IsAdjacent(const FGridCoordinate& A, const FGridCoordinate& B)
{
	return GridDistance(A, B) == 1;
}

void UGridUtils::FillNineGridResult(FNineGridResult& Result, const FGridCoordinate& Center, float CellSize)
{
	Result.Center = Center;
	Result.Cells.Reset(9);
	Result.WorldPositions.Reset(9);

	// 从左上到右下，按行填充3×3格子
	// (Center.X-1, Center.Y-1)  (Center.X, Center.Y-1)  (Center.X+1, Center.Y-1)
	// (Center.X-1, Center.Y)    (Center.X, Center.Y)    (Center.X+1, Center.Y)
	// (Center.X-1, Center.Y+1)  (Center.X, Center.Y+1)  (Center.X+1, Center.Y+1)
	for (int32 Row = -1; Row <= 1; ++Row)
	{
		for (int32 Col = -1; Col <= 1; ++Col)
		{
			const FGridCoordinate Cell(Center.X + Col, Center.Y - Row);
			Result.Cells.Add(Cell);
			Result.WorldPositions.Add(GridToWorld(Cell, CellSize, 0.0f));
		}
	}
}
