// Copyright Epic Games, Inc. All Rights Reserved.

#include "TerrainMeshGenCS.h"
#include "TerrainMeshData.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "DataDrivenShaderPlatformInfo.h"

UE_DISABLE_OPTIMIZATION

// ── Thread-group dimensions (must match the HLSL [numthreads] declaration) ──

#define NUM_THREADS_PER_GROUP_X 8
#define NUM_THREADS_PER_GROUP_Y 8
#define NUM_THREADS_PER_GROUP_Z 1

// ── Stats ──────────────────────────────────────────────────────────────────

DECLARE_STATS_GROUP(TEXT("ProceduralLandmassMeshCS"),
	STATGROUP_ProceduralLandmassMeshCS, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("ProceduralLandmassMeshCS Execute"),
	STAT_ProceduralLandmassMeshCS_Execute, STATGROUP_ProceduralLandmassMeshCS);

static TAutoConsoleVariable<int32> CVarMeshCSReadback(
	TEXT("r.ProceduralLandmass.MeshCS.Readback"),
	0,
	TEXT("0: Off (default)\n")
	TEXT("1: Log summary (min/max/avg of positions)\n")
	TEXT("2: Log every vertex value"),
	ECVF_RenderThreadSafe);

DEFINE_LOG_CATEGORY_STATIC(LogProceduralLandmassMeshCS, Log, All);

// ═══════════════════════════════════════════════════════════════════════════
//  FGlobalShader: binds C++ side → HLSL
// ═══════════════════════════════════════════════════════════════════════════

IMPLEMENT_GLOBAL_SHADER(FTerrainMeshGenCS,
	"/ProceduralLandmass/TerrainMeshGenCS.usf",
	"MainTerrainMeshGenCS", SF_Compute);

void FTerrainMeshGenCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_X);
	OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_Y);
	OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_Z);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch helpers
// ═══════════════════════════════════════════════════════════════════════════

void FTerrainMeshGenCSInterface::Dispatch(
	const FTerrainMeshGenParameters& Params,
	FGeneratedMeshBuffers& OutBuffers)
{
	if (IsInRenderingThread())
	{
		DispatchRenderThread(
			GetImmediateCommandList_ForRenderCommand(), Params, OutBuffers);
	}
	else
	{
		DispatchGameThread(Params, OutBuffers);
	}
}

void FTerrainMeshGenCSInterface::DispatchGameThread(
	const FTerrainMeshGenParameters& Params,
	FGeneratedMeshBuffers& OutBuffers)
{
	ENQUEUE_RENDER_COMMAND(ProceduralLandmassTerrainMeshCS)(
		[Params, &OutBuffers](FRHICommandListImmediate& RHICmdList)
		{
			DispatchRenderThread(RHICmdList, Params, OutBuffers);
		});
}

void FTerrainMeshGenCSInterface::DispatchRenderThread(
	FRHICommandListImmediate& RHICmdList,
	const FTerrainMeshGenParameters& Params,
	FGeneratedMeshBuffers& OutBuffers)
{
	// Guard: validate input
	const int32 GridSize = FMath::Max(Params.GridSize, 1);
	if (Params.HeightRenderTarget == nullptr)
	{
		UE_LOG(LogProceduralLandmassMeshCS, Warning,
			TEXT("DispatchRenderThread: HeightRenderTarget is null, skipping."));
		return;
	}

	const int32 NumVertices = GridSize * GridSize;
	const int32 NumQuads    = (GridSize - 1) * (GridSize - 1);
	const int32 NumIndices  = NumQuads * 6;  // 6 indices per quad (2 triangles)

	FRDGBuilder GraphBuilder(RHICmdList);

	const int32 ReadbackMode = CVarMeshCSReadback.GetValueOnRenderThread();

	{
		SCOPE_CYCLE_COUNTER(STAT_ProceduralLandmassMeshCS_Execute);
		DECLARE_GPU_STAT(ProceduralLandmassMeshCS);
		RDG_EVENT_SCOPE(GraphBuilder, "ProceduralLandmassMeshCS");
		RDG_GPU_STAT_SCOPE(GraphBuilder, ProceduralLandmassMeshCS);

		typename FTerrainMeshGenCS::FPermutationDomain PermutationVector;
		TShaderMapRef<FTerrainMeshGenCS> ComputeShader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

		if (!ComputeShader.IsValid())
		{
			UE_LOG(LogProceduralLandmassMeshCS, Error,
				TEXT("FTerrainMeshGenCS shader is not valid, skipping."));
			return;
		}

		// ── Register external heightmap RT ─────────────────────────────────
		FRDGTextureRef HeightRdgTexture = RegisterExternalTexture(
			GraphBuilder,
			Params.HeightRenderTarget->GetRenderTargetTexture(),
			TEXT("TerrainMeshGenCS_HeightRT"));

		// ── Create output structured buffers ────────────────────────────────
		FRDGBufferRef PositionsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumVertices),
			TEXT("TerrainMeshGenCS_Positions"));

		FRDGBufferRef NormalsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumVertices),
			TEXT("TerrainMeshGenCS_Normals"));

		FRDGBufferRef UVsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), NumVertices),
			TEXT("TerrainMeshGenCS_UVs"));

		FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(NumIndices, 1)),
			TEXT("TerrainMeshGenCS_Indices"));

		// ── Allocate and fill pass parameters ───────────────────────────────
		FTerrainMeshGenCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FTerrainMeshGenCS::FParameters>();

		PassParameters->HeightMap    = GraphBuilder.CreateSRV(
			FRDGTextureSRVDesc::Create(HeightRdgTexture));
		PassParameters->OutPositions = GraphBuilder.CreateUAV(PositionsBuffer);
		PassParameters->OutNormals   = GraphBuilder.CreateUAV(NormalsBuffer);
		PassParameters->OutUVs       = GraphBuilder.CreateUAV(UVsBuffer);
		PassParameters->OutIndices   = GraphBuilder.CreateUAV(IndicesBuffer);
		PassParameters->GridSize     = GridSize;
		PassParameters->HeightScale  = Params.HeightScale;
		PassParameters->WorldOrigin  = Params.WorldOrigin;

		// ── Compute group count ─────────────────────────────────────────────
		FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
			FIntPoint(GridSize, GridSize),
			FIntPoint(NUM_THREADS_PER_GROUP_X, NUM_THREADS_PER_GROUP_Y));

		// ── Dispatch compute pass ───────────────────────────────────────────
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ExecuteTerrainMeshGenCS"),
			ComputeShader,
			PassParameters,
			GroupCount);

		// ── Extract buffers for caller use ──────────────────────────────────
		GraphBuilder.QueueBufferExtraction(PositionsBuffer, &OutBuffers.PositionBuffer);
		GraphBuilder.QueueBufferExtraction(NormalsBuffer,   &OutBuffers.NormalBuffer);
		GraphBuilder.QueueBufferExtraction(UVsBuffer,       &OutBuffers.UVBuffer);
		GraphBuilder.QueueBufferExtraction(IndicesBuffer,   &OutBuffers.IndexBuffer);

		OutBuffers.NumVertices = NumVertices;
		OutBuffers.NumIndices  = NumIndices;

		// ── Debug readback ─────────────────────────────────────────────────
		// Note: Structured buffer readback from RDG requires EnqueueCopy.
		// For simplicity, we log the expected buffer sizes and the first few
		// vertices can be verified by reading the pooled buffer post-Execute.
		if (ReadbackMode > 0)
		{
			UE_LOG(LogProceduralLandmassMeshCS, Log,
				TEXT("=== TerrainMeshGenCS Dispatch ==="));
			UE_LOG(LogProceduralLandmassMeshCS, Log,
				TEXT("  GridSize=%d  NumVertices=%d  NumIndices=%d"),
				GridSize, NumVertices, NumIndices);
			UE_LOG(LogProceduralLandmassMeshCS, Log,
				TEXT("  HeightScale=%.1f  WorldOrigin=(%.1f, %.1f)"),
				Params.HeightScale, Params.WorldOrigin.X, Params.WorldOrigin.Y);
			UE_LOG(LogProceduralLandmassMeshCS, Log,
				TEXT("  GroupCount=(%d, %d, %d)  ReadbackMode=%d"),
				GroupCount.X, GroupCount.Y, GroupCount.Z, ReadbackMode);
		}
	}

	GraphBuilder.Execute();

	// ── Post-execute readback (if requested and buffers were extracted) ────
	if (ReadbackMode >= 2 && OutBuffers.PositionBuffer.IsValid())
	{
		// Readback structured buffer contents to CPU for logging.
		// This copies GPU→CPU on the render thread.
		{
			FRHICommandListImmediate& ReadbackCmdList =
				GetImmediateCommandList_ForRenderCommand();

			ReadbackCmdList.BlockUntilGPUIdle();

			// Get the underlying RHI buffer
			FRHIBuffer* RhiBuffer = OutBuffers.PositionBuffer->GetRHI();
			if (RhiBuffer)
			{
				const int32 ByteCount = NumVertices * sizeof(FVector4f);
				void* MappedData = ReadbackCmdList.LockBuffer(RhiBuffer, 0, ByteCount, RLM_ReadOnly);
				if (MappedData)
				{
					const FVector4f* Positions = static_cast<const FVector4f*>(MappedData);
					const int32 MaxLogVerts = FMath::Min(NumVertices, 20);

					UE_LOG(LogProceduralLandmassMeshCS, Log,
						TEXT("  ── First %d vertex positions ──"), MaxLogVerts);
					for (int32 i = 0; i < MaxLogVerts; ++i)
					{
						UE_LOG(LogProceduralLandmassMeshCS, Log,
							TEXT("    [%04d] (%.1f, %.1f, %.1f)"),
							i, Positions[i].X, Positions[i].Y, Positions[i].Z);
					}
					ReadbackCmdList.UnlockBuffer(RhiBuffer);
				}
				else
				{
					UE_LOG(LogProceduralLandmassMeshCS, Warning,
						TEXT("  Failed to lock position buffer for readback."));
				}
			}
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch with CPU readback
// ═══════════════════════════════════════════════════════════════════════════

bool FTerrainMeshGenCSInterface::DispatchAndReadback(
	const FTerrainMeshGenParameters& Params,
	FLODMeshData& OutLODData)
{
	if (Params.HeightRenderTarget == nullptr)
	{
		return false;
	}

	const int32 GridSize = FMath::Max(Params.GridSize, 1);
	const int32 NumVertices = GridSize * GridSize;
	const int32 NumQuads = (GridSize - 1) * (GridSize - 1);
	const int32 NumIndices = NumQuads * 6;

	// Pre-allocate output arrays
	OutLODData.Vertices.SetNum(NumVertices);
	OutLODData.UVs.SetNum(NumVertices);
	OutLODData.Triangles.SetNum(NumIndices);
	OutLODData.ChunkSize = GridSize;

	// Dispatch on render thread and get GPU buffers
	FGeneratedMeshBuffers Buffers;
	Dispatch(Params, Buffers);

	// Flush rendering so buffers are available
	FlushRenderingCommands();

	// Read back on render thread
	ENQUEUE_RENDER_COMMAND(ReadbackTerrainMeshBuffers)(
		[&Buffers, &OutLODData, NumVertices, NumIndices](FRHICommandListImmediate& RHICmdList)
		{
			// Read positions
			if (Buffers.PositionBuffer.IsValid())
			{
				FRHIBuffer* RhiBuf = Buffers.PositionBuffer->GetRHI();
				void* Data = RHICmdList.LockBuffer(RhiBuf, 0,
					NumVertices * sizeof(FVector4f), RLM_ReadOnly);
				if (Data)
				{
					const FVector4f* SrcPos = static_cast<const FVector4f*>(Data);
					for (int32 i = 0; i < NumVertices; ++i)
					{
						OutLODData.Vertices[i] = FVector(SrcPos[i]);
					}
					RHICmdList.UnlockBuffer(RhiBuf);
				}
			}

			// Read UVs
			if (Buffers.UVBuffer.IsValid())
			{
				FRHIBuffer* RhiBuf = Buffers.UVBuffer->GetRHI();
				void* Data = RHICmdList.LockBuffer(RhiBuf, 0,
					NumVertices * sizeof(FVector2f), RLM_ReadOnly);
				if (Data)
				{
					FMemory::Memcpy(OutLODData.UVs.GetData(), Data,
						NumVertices * sizeof(FVector2f));
					RHICmdList.UnlockBuffer(RhiBuf);
				}
			}

			// Read indices
			if (Buffers.IndexBuffer.IsValid())
			{
				FRHIBuffer* RhiBuf = Buffers.IndexBuffer->GetRHI();
				void* Data = RHICmdList.LockBuffer(RhiBuf, 0,
					NumIndices * sizeof(uint32), RLM_ReadOnly);
				if (Data)
				{
					const uint32* SrcIdx = static_cast<const uint32*>(Data);
					for (int32 i = 0; i < NumIndices; ++i)
					{
						OutLODData.Triangles[i] = static_cast<int32>(SrcIdx[i]);
					}
					RHICmdList.UnlockBuffer(RhiBuf);
				}
			}
		});

	FlushRenderingCommands();
	return true;
}

UE_ENABLE_OPTIMIZATION
