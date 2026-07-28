// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProceduralLandmass.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FProceduralLandmassModule"

void FProceduralLandmassModule::StartupModule()
{
	const FString ShaderDirectory = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("ProceduralLandmass"))->GetBaseDir(),
		TEXT("Shaders/Private"));
	AddShaderSourceDirectoryMapping("/ProceduralLandmass", ShaderDirectory);
}

void FProceduralLandmassModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FProceduralLandmassModule, ProceduralLandmass)