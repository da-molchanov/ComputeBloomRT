// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ComputeBloomComponent.generated.h"


struct FBloomCSParameters
{
	class UTextureRenderTarget2D* OutRenderTarget;
	class UTextureRenderTarget2D* InRenderTarget;

	FBloomCSParameters() { }
};

// Compute shader manager singleton
class FBloomCSManager
{
public:
	//Get the instance
	static FBloomCSManager* Get()
	{
		if (!instance)
		{
			instance = new FBloomCSManager();
		}
		return instance;
	};

	// Call this whenever you have new parameters to share.
	void UpdateParameters(FBloomCSParameters& DrawParameters);

	void Render();

private:
	//Private constructor to prevent client from instanciating
	FBloomCSManager() = default;

	//The singleton instance
	static FBloomCSManager* instance;

	//Cached Shader Manager Parameters
	FBloomCSParameters cachedParams;

	//Whether we have cached parameters to pass to the shader or not
	volatile bool bCachedParamsAreValid;

	//Reference to a pooled render target where the shader will write its output
	TRefCountPtr<IPooledRenderTarget> ComputeShaderOutput;
	TRefCountPtr<IPooledRenderTarget> ComputeShaderInput;
public:
	void Execute_RenderThread(FRHICommandListImmediate& RHICmdList);
};


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class COMPUTEBLOOMRT_API UComputeBloomComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	// Sets default values for this component's properties
	UComputeBloomComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Shader)
	class UTextureRenderTarget2D* OutRenderTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Shader)
	class UTextureRenderTarget2D* InRenderTarget;

public:	
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
};
