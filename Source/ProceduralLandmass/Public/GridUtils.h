// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "GridUtils.generated.h"

/** 网格坐标 */
USTRUCT(BlueprintType)
struct FGridCoordinate
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 Y = 0;

	FGridCoordinate() = default;
	FGridCoordinate(int32 InX, int32 InY) : X(InX), Y(InY) {}

	FIntPoint ToIntPoint() const { return FIntPoint(X, Y); }
	static FGridCoordinate FromIntPoint(const FIntPoint& P) { return FGridCoordinate(P.X, P.Y); }

	bool operator==(const FGridCoordinate& Other) const { return X == Other.X && Y == Other.Y; }
	bool operator!=(const FGridCoordinate& Other) const { return !(*this == Other); }

	friend uint32 GetTypeHash(const FGridCoordinate& C) { return HashCombine(GetTypeHash(C.X), GetTypeHash(C.Y)); }
};

/** 九宫格计算结果 */
USTRUCT(BlueprintType)
struct FNineGridResult
{
	GENERATED_BODY()

	/** 中心格子坐标（玩家所在格子） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	FGridCoordinate Center;

	/** 周围9个格子的坐标（包含中心），按从左到右、从上到下的顺序排列 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FGridCoordinate> Cells;

	/** 各格子对应的世界坐标中心 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FVector> WorldPositions;
};

/**
 * 地图九宫格计算工具
 * 将世界划分为均匀网格，根据玩家位置计算周围9个格子
 */
UCLASS()
class PROCEDURALLANDMASS_API UGridUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 将世界坐标转换为网格坐标
	 * @param WorldPosition 世界位置
	 * @param CellSize 格子大小（世界单位）
	 * @return 对应的网格坐标
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Grid")
	static FGridCoordinate WorldToGrid(FVector WorldPosition, float CellSize = 10000.0f);

	/**
	 * 将网格坐标转换为世界坐标（返回格子中心点）
	 * @param GridCoord 网格坐标
	 * @param CellSize 格子大小（世界单位）
	 * @param Z 世界Z轴高度
	 * @return 格子中心的世界位置
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Grid")
	static FVector GridToWorld(const FGridCoordinate& GridCoord, float CellSize = 10000.0f, float Z = 0.0f);

	/**
	 * 根据玩家世界位置计算九宫格（玩家所在格 + 周围8格）
	 * @param PlayerPosition 玩家世界位置
	 * @param CellSize 格子大小
	 * @return 九宫格计算结果
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Grid", meta = (DisplayName = "Get Nine Grid Cells"))
	static FNineGridResult GetNineGridCells(FVector PlayerPosition, float CellSize = 10000.0f);

	/**
	 * 根据指定网格坐标计算九宫格
	 * @param CenterGrid 中心网格坐标
	 * @param CellSize 格子大小
	 * @return 九宫格计算结果
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Grid")
	static FNineGridResult GetNineGridCellsFromGrid(const FGridCoordinate& CenterGrid, float CellSize = 10000.0f);

	/**
	 * 计算两个网格坐标之间的棋盘距离（Chebyshev distance）
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Grid")
	static int32 GridDistance(const FGridCoordinate& A, const FGridCoordinate& B);

	/**
	 * 判断两个网格坐标是否相邻（8方向）
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Grid")
	static bool IsAdjacent(const FGridCoordinate& A, const FGridCoordinate& B);

private:
	static void FillNineGridResult(FNineGridResult& Result, const FGridCoordinate& Center, float CellSize);
};
