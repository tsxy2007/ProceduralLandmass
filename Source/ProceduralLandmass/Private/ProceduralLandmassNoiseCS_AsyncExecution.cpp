// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassNoiseCS_AsyncExecution.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RHIGPUReadback.h"
UE_DISABLE_OPTIMIZATION
// ── Thread-group dimensions (must match the HLSL [numthreads] declaration) ──

#define NUM_THREADS_PER_GROUP_X 8
#define NUM_THREADS_PER_GROUP_Y 8
#define NUM_THREADS_PER_GROUP_Z 1

// ── Stats ──────────────────────────────────────────────────────────────────

DECLARE_STATS_GROUP(TEXT("ProceduralLandmassNoiseCS"),
	STATGROUP_ProceduralLandmassNoiseCS, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("ProceduralLandmassNoiseCS Execute"),
	STAT_ProceduralLandmassNoiseCS_Execute, STATGROUP_ProceduralLandmassNoiseCS);

static TAutoConsoleVariable<int32> CVarNoiseCSReadback(
	TEXT("r.ProceduralLandmass.NoiseCS.Readback"),
	0,
	TEXT("0: Off (default)\n")
	TEXT("1: Log summary (min/max/avg + 10×10 corner)\n")
	TEXT("2: Log every pixel value"),
	ECVF_RenderThreadSafe);

DEFINE_LOG_CATEGORY_STATIC(LogProceduralLandmassNoiseCS, Log, All);

// ═══════════════════════════════════════════════════════════════════════════
//  FGlobalShader: binds C++ side → HLSL
// ═══════════════════════════════════════════════════════════════════════════

/** GPU-side mirror of FTerrainType with correct HLSL structured-buffer alignment.
 *  float4 in HLSL is 16-byte aligned, so the C++ struct must match with padding. */
struct FTerrainTypeGPU
{
	float NoiseHeight;      // offset 0,  sizeof 4
	float _Padding[3];      // offset 4,  sizeof 12 → align Color to 16
	float ColorR, ColorG, ColorB, ColorA;  // offset 16, sizeof 16

	FTerrainTypeGPU() = default;
	explicit FTerrainTypeGPU(const FTerrainType& Src)
		: NoiseHeight(Src.NoiseHeight)
		, ColorR(Src.Color.R), ColorG(Src.Color.G)
		, ColorB(Src.Color.B), ColorA(Src.Color.A)
	{}
};
static_assert(sizeof(FTerrainTypeGPU) == 32, "FTerrainTypeGPU must match HLSL 32-byte layout");

class FProceduralLandmassNoiseCS : public FGlobalShader
{
public:
	DECLARE_SHADER_TYPE(FProceduralLandmassNoiseCS, Global);
	SHADER_USE_PARAMETER_STRUCT(FProceduralLandmassNoiseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RenderTarget)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ColorRenderTarget)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FTerrainTypeGPU>, TerrainTypes)
		SHADER_PARAMETER(int32, NumTerrainTypes)
		SHADER_PARAMETER(int32, ChunkSize)
		SHADER_PARAMETER(float, Scale)
		SHADER_PARAMETER(int32, Octaves)
		SHADER_PARAMETER(float, Persistence)
		SHADER_PARAMETER(float, Lacunarity)
		SHADER_PARAMETER(int32, Seed)
		SHADER_PARAMETER(FVector2f, Offset)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static inline void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"),
			NUM_THREADS_PER_GROUP_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"),
			NUM_THREADS_PER_GROUP_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"),
			NUM_THREADS_PER_GROUP_Z);
	}
};

IMPLEMENT_GLOBAL_SHADER(FProceduralLandmassNoiseCS,
	"/ProceduralLandmass/ProceduralLandmassNoise.usf",
	"MainComputeShader", SF_Compute);

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch helpers
// ═══════════════════════════════════════════════════════════════════════════

void FProceduralLandmassNoiseCSInterface::Dispatch(
	FProceduralLandmassNoiseCSParameters Params)
{
	if (IsInRenderingThread())
	{
		DispatchRenderThread(
			GetImmediateCommandList_ForRenderCommand(), Params);
	}
	else
	{
		DispatchGameThread(Params);
	}
}

void FProceduralLandmassNoiseCSInterface::DispatchGameThread(
	FProceduralLandmassNoiseCSParameters Params)
{
	ENQUEUE_RENDER_COMMAND(ProceduralLandmassNoiseCS)(
		[Params](FRHICommandListImmediate& RHICmdList)
		{
			DispatchRenderThread(RHICmdList, Params);
		});
}

void FProceduralLandmassNoiseCSInterface::DispatchRenderThread(
	FRHICommandListImmediate& RHICmdList,
	FProceduralLandmassNoiseCSParameters Params)
{
	// Match CPU-side guard
	if (Params.Scale <= 0.0f)
	{
		Params.Scale = 0.0001f;
	}

	FRDGBuilder GraphBuilder(RHICmdList);

	// Debug readback object — must outlive GraphBuilder.Execute()
	FRHIGPUTextureReadback NoiseReadback(TEXT("ProceduralLandmassNoiseReadback"));
	const int32 ReadbackMode = CVarNoiseCSReadback.GetValueOnRenderThread();
	FRDGTextureRef TmpTextureForReadback = nullptr;

	UE_LOG(LogProceduralLandmassNoiseCS, Log,
		TEXT("[Readback] DispatchRenderThread entered  ReadbackMode=%d  ChunkSize=%d  RT=%p"),
		ReadbackMode, Params.ChunkSize, Params.RenderTarget);

	{
		SCOPE_CYCLE_COUNTER(STAT_ProceduralLandmassNoiseCS_Execute);
		DECLARE_GPU_STAT(ProceduralLandmassNoiseCS);
		RDG_EVENT_SCOPE(GraphBuilder, "ProceduralLandmassNoiseCS");
		RDG_GPU_STAT_SCOPE(GraphBuilder, ProceduralLandmassNoiseCS);

		// Prepare terrain-type data for GPU structured buffer
		TArray<FTerrainTypeGPU> GPUTerrainTypes;
		GPUTerrainTypes.Reserve(Params.TerrainTypes.Num());
		for (const FTerrainType& TT : Params.TerrainTypes)
		{
			GPUTerrainTypes.Emplace(TT);
			UE_LOG(LogProceduralLandmassNoiseCS, Log,
				TEXT("[Color] TerrainType: Name=%s  NoiseHeight=%.3f  Color=(%.3f,%.3f,%.3f,%.3f)"),
				*TT.Name, TT.NoiseHeight, TT.Color.R, TT.Color.G, TT.Color.B, TT.Color.A);
		}
		const int32 NumTerrainTypes = GPUTerrainTypes.Num();
		const bool bHasColorOutput = Params.ColorRenderTarget != nullptr && NumTerrainTypes > 0;
		UE_LOG(LogProceduralLandmassNoiseCS, Log,
			TEXT("[Color] NumTerrainTypes=%d  ColorRenderTarget=%p  bHasColorOutput=%d"),
			NumTerrainTypes, Params.ColorRenderTarget, bHasColorOutput ? 1 : 0);

		typename FProceduralLandmassNoiseCS::FPermutationDomain PermutationVector;
		TShaderMapRef<FProceduralLandmassNoiseCS> ComputeShader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

		const bool bShaderValid = ComputeShader.IsValid();
		UE_LOG(LogProceduralLandmassNoiseCS, Log,
			TEXT("[Readback] Shader valid: %d"), bShaderValid ? 1 : 0);

		if (bShaderValid)
		{
			FProceduralLandmassNoiseCS::FParameters* PassParameters =
				GraphBuilder.AllocParameters<
					FProceduralLandmassNoiseCS::FParameters>();

			// Intermediate RDG texture (R32F → one float per pixel)
			FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(
				FIntPoint(Params.ChunkSize, Params.ChunkSize),
				PF_R32_FLOAT,
				FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV));

			FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(
				Desc, TEXT("ProceduralLandmassNoiseCS_Temp"));

			// External target (the caller's RenderTarget)
			FRDGTextureRef TargetTexture = RegisterExternalTexture(
				GraphBuilder,
				Params.RenderTarget->GetRenderTargetTexture(),
				TEXT("ProceduralLandmassNoiseCS_RT"));

			// Color output texture (RGBA8, same dimensions)
			FRDGTextureRef TmpColorTexture = nullptr;
			FRDGTextureRef TargetColorTexture = nullptr;
			if (bHasColorOutput)
			{
				// Register external first so we can match its format
				TargetColorTexture = RegisterExternalTexture(
					GraphBuilder,
					Params.ColorRenderTarget->GetRenderTargetTexture(),
					TEXT("ProceduralLandmassNoiseCS_ColorRT"));

				FRDGTextureDesc ColorDesc(FRDGTextureDesc::Create2D(
					FIntPoint(Params.ChunkSize, Params.ChunkSize),
					TargetColorTexture->Desc.Format,
					FClearValueBinding::Black,
					TexCreate_ShaderResource | TexCreate_UAV));

				TmpColorTexture = GraphBuilder.CreateTexture(
					ColorDesc, TEXT("ProceduralLandmassNoiseCS_ColorTemp"));
			}

			// Structured buffer for terrain types (always create, even if empty)
			FRDGBufferRef TerrainTypesBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("TerrainTypesBuffer"),
				sizeof(FTerrainTypeGPU),
				FMath::Max(NumTerrainTypes, 1),  // minimum 1 element for empty case
				NumTerrainTypes > 0 ? GPUTerrainTypes.GetData() : nullptr,
				NumTerrainTypes * sizeof(FTerrainTypeGPU));

			PassParameters->RenderTarget = GraphBuilder.CreateUAV(TmpTexture);
			PassParameters->ChunkSize    = Params.ChunkSize;
			PassParameters->Scale        = Params.Scale;
			PassParameters->Octaves      = Params.Octaves;
			PassParameters->Persistence  = Params.Persistence;
			PassParameters->Lacunarity   = Params.Lacunarity;
			PassParameters->Seed         = Params.Seed;
			PassParameters->Offset       = FVector2f(
				static_cast<float>(Params.Offset.X),
				static_cast<float>(Params.Offset.Y));

			// Color output binding (always bind; fallback to height texture UAV when no color needed)
			PassParameters->ColorRenderTarget = bHasColorOutput
				? GraphBuilder.CreateUAV(TmpColorTexture)
				: GraphBuilder.CreateUAV(TmpTexture);  // fallback: bind height texture (unused when NumTerrainTypes==0)

			PassParameters->TerrainTypes = GraphBuilder.CreateSRV(TerrainTypesBuffer);
			PassParameters->NumTerrainTypes = NumTerrainTypes;

			auto GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntVector(Params.ChunkSize, Params.ChunkSize, 1),
				FComputeShaderUtils::kGolden2DGroupSize);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExecuteProceduralLandmassNoiseCS"),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, ComputeShader, GroupCount](
					FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(
						RHICmdList, ComputeShader,
						*PassParameters, GroupCount);
				});

			// Copy intermediate → external if format matches
			if (TargetTexture->Desc.Format == PF_R32_FLOAT)
			{
				AddCopyTexturePass(GraphBuilder, TmpTexture, TargetTexture,
					FRHICopyTextureInfo());
			}

			// Copy intermediate color → external color RT
			if (bHasColorOutput)
			{
				AddCopyTexturePass(GraphBuilder, TmpColorTexture, TargetColorTexture,
					FRHICopyTextureInfo());
			}

			// Enqueue readback from intermediate texture
			if (ReadbackMode > 0)
			{
				TmpTextureForReadback = TmpTexture;
				AddEnqueueCopyPass(GraphBuilder, &NoiseReadback, TmpTexture);
				UE_LOG(LogProceduralLandmassNoiseCS, Log,
					TEXT("[Readback] Readback pass enqueued  TmpTextureForReadback=%p"), TmpTextureForReadback);
			}
			else
			{
				UE_LOG(LogProceduralLandmassNoiseCS, Verbose,
					TEXT("[Readback] Skipped — ReadbackMode=%d"), ReadbackMode);
			}
		}
	}

	GraphBuilder.Execute();

	UE_LOG(LogProceduralLandmassNoiseCS, Log,
		TEXT("[Readback] Post-Execute  ReadbackMode=%d  TmpTextureForReadback=%p"),
		ReadbackMode, TmpTextureForReadback);

	// ── Readback & log ────────────────────────────────────────────────────
	if (ReadbackMode > 0 && TmpTextureForReadback)
	{
		// Ensure GPU has finished so readback data is available
		RHICmdList.BlockUntilGPUIdle();

		if (NoiseReadback.IsReady())
		{
			int32 RowPitchInPixels = 0;
			const float* Data = static_cast<const float*>(
				NoiseReadback.Lock(RowPitchInPixels));

			if (Data)
			{
				const int32 TotalPixels = Params.ChunkSize * Params.ChunkSize;
				const int32 RowStride = (RowPitchInPixels > 0)
					? RowPitchInPixels : Params.ChunkSize;

				UE_LOG(LogProceduralLandmassNoiseCS, Log,
					TEXT("=== NoiseCS RenderTarget Readback ==="));
				UE_LOG(LogProceduralLandmassNoiseCS, Log,
					TEXT("  Size: %dx%d  Total pixels: %d  RowPitch: %d"),
					Params.ChunkSize, Params.ChunkSize, TotalPixels, RowPitchInPixels);

				// Compute statistics
				float MinVal = TNumericLimits<float>::Max();
				float MaxVal = TNumericLimits<float>::Lowest();
				double Sum = 0.0;
				int32 ZeroCount = 0;

				for (int32 i = 0; i < TotalPixels; ++i)
				{
					const float Val = Data[i];
					MinVal = FMath::Min(MinVal, Val);
					MaxVal = FMath::Max(MaxVal, Val);
					Sum += static_cast<double>(Val);
					if (Val == 0.0f) { ++ZeroCount; }
				}

				UE_LOG(LogProceduralLandmassNoiseCS, Log,
					TEXT("  Min: %.6f  Max: %.6f  Avg: %.6f  ZeroPixels: %d"),
					MinVal, MaxVal, Sum / static_cast<double>(TotalPixels), ZeroCount);

				// Print 10×10 corner samples
				UE_LOG(LogProceduralLandmassNoiseCS, Log,
					TEXT("  ── Top-left 10×10 corner ──"));
				const int32 SampleSize = Params.ChunkSize;// FMath::Min(10, Params.ChunkSize);
				for (int32 Y = 0; Y < SampleSize; ++Y)
				{
					FString RowStr;
					for (int32 X = 0; X < SampleSize; ++X)
					{
						RowStr += FString::Printf(TEXT("%.3f "),
							Data[Y * RowStride + X]);
					}
					UE_LOG(LogProceduralLandmassNoiseCS, Log,
						TEXT("    [%02d] %s"), Y, *RowStr);
				}

				// Mode 2: dump every pixel
				if (ReadbackMode >= 2)
				{
					UE_LOG(LogProceduralLandmassNoiseCS, Log,
						TEXT("  ── Full pixel dump ──"));
					for (int32 Y = 0; Y < Params.ChunkSize; ++Y)
					{
						FString RowStr;
						for (int32 X = 0; X < Params.ChunkSize; ++X)
						{
							RowStr += FString::Printf(TEXT("%.4f "),
								Data[Y * RowStride + X]);
						}
						UE_LOG(LogProceduralLandmassNoiseCS, Log,
							TEXT("    [%04d] %s"), Y, *RowStr);
					}
				}

				NoiseReadback.Unlock();
			}
		}
		else
		{
			UE_LOG(LogProceduralLandmassNoiseCS, Warning,
				TEXT("NoiseCS readback not ready after BlockUntilGPUIdle"));
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Blueprint async-action node
// ═══════════════════════════════════════════════════════════════════════════

void UProceduralLandmassNoiseCS_AsyncExecution::Activate()
{
	FProceduralLandmassNoiseCSParameters Params(
		ChunkSize, Scale, Octaves, Persistence, Lacunarity, Seed, Offset);

	Params.RenderTarget = RT->GameThread_GetRenderTargetResource();

	if (ColorRT && TerrainTypes.Num() > 0)
	{
		Params.ColorRenderTarget = ColorRT->GameThread_GetRenderTargetResource();
		Params.TerrainTypes = TerrainTypes;
	}

	FProceduralLandmassNoiseCSInterface::Dispatch(Params);
}

UProceduralLandmassNoiseCS_AsyncExecution*
UProceduralLandmassNoiseCS_AsyncExecution::ExecuteGPUNoiseMap(
	UObject* WorldContextObject,
	const TArray<FTerrainType>& TerrainTypes,
	int32 ChunkSize,
	float Scale,
	int32 Octaves,
	float Persistence,
	float Lacunarity,
	int32 Seed,
	FVector2D Offset,
	UTextureRenderTarget2D* RT,
	UTextureRenderTarget2D* ColorRT)
{
	UProceduralLandmassNoiseCS_AsyncExecution* Action =
		NewObject<UProceduralLandmassNoiseCS_AsyncExecution>();
	Action->RT          = RT;
	Action->ChunkSize   = ChunkSize;
	Action->Scale       = Scale;
	Action->Octaves     = Octaves;
	Action->Persistence = Persistence;
	Action->Lacunarity  = Lacunarity;
	Action->Seed        = Seed;
	Action->Offset      = Offset;
	Action->ColorRT      = ColorRT;
	Action->TerrainTypes = TerrainTypes;
	Action->RegisterWithGameInstance(WorldContextObject);

	return Action;
}

UE_ENABLE_OPTIMIZATION