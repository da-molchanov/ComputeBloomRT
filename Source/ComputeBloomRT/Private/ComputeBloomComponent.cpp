// Fill out your copyright notice in the Description page of Project Settings.


#include "ComputeBloomComponent.h"
#include "CoreMinimal.h"
#include "Runtime/Engine/Classes/Engine/TextureRenderTarget2D.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderTargetPool.h"
#include "Modules/ModuleManager.h"
#include "RenderGraphUtils.h"

TAutoConsoleVariable<float> CVarBloomRadius(
	TEXT("r.ComputeBloomRT.Radius"),
	0.85,
	TEXT("Lerp coefficient between current and downsampled color during bloom upsampling"),
	ECVF_RenderThreadSafe);

class FClearCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearCS);
	SHADER_USE_PARAMETER_STRUCT(FClearCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, MipOutUAV)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return RHISupportsComputeShaders(Parameters.Platform);
	}
};

class FDownsampleCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector2D, TexelSize)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, MipInSRV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, MipOutUAV)
		SHADER_PARAMETER_SAMPLER(SamplerState, MipSampler)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return RHISupportsComputeShaders(Parameters.Platform);
	}
};

class FUpsampleCombineCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUpsampleCombineCS);
	SHADER_USE_PARAMETER_STRUCT(FUpsampleCombineCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector2D, TexelSize)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, MipInSRV)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CurrInSRV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, MipOutUAV)
		SHADER_PARAMETER_SAMPLER(SamplerState, MipSampler)
		SHADER_PARAMETER(float, Radius)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return RHISupportsComputeShaders(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDownsampleCS, "/ComputeBloomRT/ComputeBloom.usf", "DownsampleCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUpsampleCombineCS, "/ComputeBloomRT/ComputeBloom.usf", "UpsampleCombineCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClearCS, "/ComputeBloomRT/ComputeBloom.usf", "ClearCS", SF_Compute);

FBloomCSManager* FBloomCSManager::instance = nullptr;

void FBloomCSManager::UpdateParameters(FBloomCSParameters& params)
{
	cachedParams = params;
	bCachedParamsAreValid = true;
}

void FBloomCSManager::Render()
{
	// Go from the game thread to the render thread
	ENQUEUE_RENDER_COMMAND(ComputeBloomRT)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			Execute_RenderThread(RHICmdList);
		}
	);
}

void FBloomCSManager::Execute_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());

	if (!(bCachedParamsAreValid && cachedParams.InRenderTarget && cachedParams.OutRenderTarget))
	{
		// Do nothing if the render targets have not been set yet
		return;
	}

	FTextureRHIRef OutTextureRHI = cachedParams.OutRenderTarget->GetRenderTargetResource()->TextureRHI;
	FTextureRHIRef InTextureRHI = cachedParams.InRenderTarget->GetRenderTargetResource()->TextureRHI;

	if (OutTextureRHI->GetNumMips() <= 1 || InTextureRHI->GetNumMips() <= 1)
	{
		UE_LOG(LogTemp, Error, TEXT("Missing mips in render targets!"));
		return;
	}

	if (InTextureRHI->GetNumMips() != OutTextureRHI->GetNumMips())
	{
		UE_LOG(LogTemp, Error, TEXT("Render targets have different mip count! Ensure they have the same resolution."));
		return;
	}

	SCOPED_DRAW_EVENT(RHICmdList, ComputeBloomRT)

	FRDGBuilder GraphBuilder(RHICmdList);

	// Output texture
	CacheRenderTarget(OutTextureRHI, TEXT("BloomOutput"), ComputeShaderOutput);
	FRDGTextureRef OutTextureRDG = GraphBuilder.RegisterExternalTexture(ComputeShaderOutput);
	const FRDGTextureDesc& TextureDesc = OutTextureRDG->Desc;

	// Input texture
	CacheRenderTarget(InTextureRHI, TEXT("BloomInput"), ComputeShaderInput);
	FRDGTextureRef InTextureRDG = GraphBuilder.RegisterExternalTexture(ComputeShaderInput);

	// Texture sampler
	FSamplerStateInitializerRHI SamplerInit(SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp);
	FSamplerStateRHIRef Sampler = RHICreateSamplerState(SamplerInit);

	{
		// Clear the highest mip level of the output render target
		// to prevent previous frames from bleeding into the current frame
		uint32 MipLevel = TextureDesc.NumMips - 1;
		const FIntPoint DestTextureSize(
			FMath::Max(TextureDesc.Extent.X >> MipLevel, 1),
			FMath::Max(TextureDesc.Extent.Y >> MipLevel, 1));

		TShaderMapRef<FClearCS> ClearCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FClearCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearCS::FParameters>();
		PassParameters->MipOutUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutTextureRDG, MipLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Clear DestMipLevel=%d", MipLevel),
			ClearCS,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DestTextureSize, FComputeShaderUtils::kGolden2DGroupSize));
	}

	// Downsample into the mips of the input render target
	TShaderMapRef<FDownsampleCS> DownsampleCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Loop through each level of the mips that require creation and add a dispatch pass per level.
	for (uint32 MipLevel = 1, MipCount = TextureDesc.NumMips; MipLevel < MipCount; ++MipLevel)
	{
		const FIntPoint DestTextureSize(
			FMath::Max(TextureDesc.Extent.X >> MipLevel, 1),
			FMath::Max(TextureDesc.Extent.Y >> MipLevel, 1));

		FDownsampleCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDownsampleCS::FParameters>();
		PassParameters->TexelSize  = FVector2D(1.0f / DestTextureSize.X, 1.0f / DestTextureSize.Y);
		PassParameters->MipInSRV   = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(InTextureRDG, MipLevel - 1));
		PassParameters->MipOutUAV  = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(InTextureRDG, MipLevel));
		PassParameters->MipSampler = Sampler;

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Downsample DestMipLevel=%d", MipLevel),
			DownsampleCS,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DestTextureSize, FComputeShaderUtils::kGolden2DGroupSize));
	}


	// Upsample and combine into the mips of the output render target
	TShaderMapRef<FUpsampleCombineCS> UpsampleCombineCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Loop through each level of the mips that require creation and add a dispatch pass per level.
	for (uint32 MipLevel = TextureDesc.NumMips - 1; MipLevel > 0; --MipLevel)
	{
		// Upsample MipLevel of the output texture
		// Combine with MipLevel - 1 of the input texture
		// Put into MipLevel - 1 of the output texture

		const FIntPoint DestTextureSize(
			FMath::Max(TextureDesc.Extent.X >> (MipLevel - 1), 1),
			FMath::Max(TextureDesc.Extent.Y >> (MipLevel - 1), 1));

		FUpsampleCombineCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpsampleCombineCS::FParameters>();
		PassParameters->TexelSize  = FVector2D(1.0f / DestTextureSize.X, 1.0f / DestTextureSize.Y);
		PassParameters->MipInSRV   = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(OutTextureRDG, MipLevel));
		PassParameters->CurrInSRV   = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(InTextureRDG, MipLevel - 1));
		PassParameters->MipOutUAV  = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutTextureRDG, MipLevel - 1));
		PassParameters->MipSampler = Sampler;
		PassParameters->Radius = CVarBloomRadius.GetValueOnRenderThread();

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("UpsampleCombine DestMipLevel=%d", MipLevel - 1),
			UpsampleCombineCS,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DestTextureSize, FComputeShaderUtils::kGolden2DGroupSize));
	}

	GraphBuilder.Execute();
}

// Sets default values for this component's properties
UComputeBloomComponent::UComputeBloomComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}


// Called every frame
void UComputeBloomComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Screen capture triggers mip generation if bAutoGenerateMips is true.
	// The stock downsampling filter is not suitable, so we disable automatic
	// generation and build the mips manually.
	if (InRenderTarget)
	{
		InRenderTarget->bAutoGenerateMips = false;
	}

	FBloomCSParameters params;
	params.InRenderTarget = InRenderTarget;
	params.OutRenderTarget = OutRenderTarget;

	FBloomCSManager::Get()->UpdateParameters(params);
	FBloomCSManager::Get()->Render();
}

