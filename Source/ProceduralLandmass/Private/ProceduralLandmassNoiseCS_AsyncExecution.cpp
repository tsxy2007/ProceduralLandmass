// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmassNoiseCS_AsyncExecution.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RHIGPUReadback.h"

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

class FProceduralLandmassNoiseCS : public FGlobalShader
{
public:
	DECLARE_SHADER_TYPE(FProceduralLandmassNoiseCS, Global);
	SHADER_USE_PARAMETER_STRUCT(FProceduralLandmassNoiseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RenderTarget)
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

	FProceduralLandmassNoiseCSInterface::Dispatch(Params);
}

UProceduralLandmassNoiseCS_AsyncExecution*
UProceduralLandmassNoiseCS_AsyncExecution::ExecuteGPUNoiseMap(
	UObject* WorldContextObject,
	UTextureRenderTarget2D* RT,
	int32 ChunkSize,
	float Scale,
	int32 Octaves,
	float Persistence,
	float Lacunarity,
	int32 Seed,
	FVector2D Offset)
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
	Action->RegisterWithGameInstance(WorldContextObject);

	return Action;
}
