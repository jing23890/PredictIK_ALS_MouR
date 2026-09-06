#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "PIKAnimInstance.generated.h"

/** All path points are world-space support heights, without ankle height. */
USTRUCT(BlueprintType)
struct FPIKFootState
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") bool bInitialized = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") bool bPlanted = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") bool bPredictionValid = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") bool bPathValid = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") FVector PlantContactWS = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") FVector PathStartWS = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") FVector PredictedContactWS = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") FVector TargetAnkleWS = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") FVector ContactNormalWS = FVector::UpVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") TArray<FVector> PathPointsWS;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") float TimeToLandSec = 0.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") float PathProgress = 0.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") int32 PathBuildCount = 0;

    FVector LastBuildTargetWS = FVector::ZeroVector;
    float TimeSinceBuildSec = 0.f;
};

/** Forward-walk prototype. No Mesh movement, no ordinary-IK fallback, no curve writes. */
UCLASS(Blueprintable, BlueprintType)
class ADVANCEDLOCOMOTIONV4_API UPIKAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Enable") bool bPIK_Enabled = true;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Debug") bool bPIK_DrawDebug = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0", Units="cm")) float PIK_PlantThresholdCm = 14.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0", Units="cm")) float PIK_ReleaseThresholdCm = 14.5f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0", Units="cm")) float PIK_AnkleHeightCm = 13.46f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", Units="cm")) float PIK_TraceUpCm = 80.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", Units="cm")) float PIK_TraceDownCm = 120.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", Units="cm")) float PIK_MaxTerrainDeltaCm = 45.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="0.1", Units="cm")) float PIK_PathSurfaceLiftCm = 2.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="0.1", Units="cm")) float PIK_EdgeProbeInsetCm = 3.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="0", Units="cm")) float PIK_FootClearanceMarginCm = 8.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", ClampMax="12")) int32 PIK_MaxPathIterations = 6;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace") TEnumAsByte<ECollisionChannel> PIK_TraceChannel = ECC_Visibility;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.01", Units="s")) float PIK_RebuildIntervalSec = 0.10f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.1", Units="cm")) float PIK_RebuildDistanceCm = 6.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.5", ClampMax="1")) float PIK_MinForwardDot = 0.98f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0", Units="cm/s")) float PIK_MinSpeedCmPerSec = 10.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Blend", meta=(ClampMin="0.01", Units="s")) float PIK_BlendInSec = 0.15f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Blend", meta=(ClampMin="0.01", Units="s")) float PIK_BlendOutSec = 0.12f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Pelvis", meta=(ClampMin="0", Units="cm")) float PIK_MaxPelvisOffsetCm = 40.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Pelvis", meta=(ClampMin="0.1")) float PIK_PelvisInterpSpeed = 10.f;

    // Sampled from ALS_N_Walk_F at FootTimeToLand_L/R == 0 (0.2 / 0.7666667 sec).
    // Component-space +Y is forward for the ALS mannequin. Never derive these from solved feet.
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Walk Calibration") FVector PIK_LandingReferenceCS_L = FVector(6.076324, 10.247291, 13.465718);
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Walk Calibration") FVector PIK_LandingReferenceCS_R = FVector(-6.076340, 10.247272, 13.465560);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") float PIK_Alpha = 0.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FVector PIK_PelvisOffsetCS = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FVector PIK_FootTargetCS_L = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FVector PIK_FootTargetCS_R = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FVector PIK_KneeTargetCS_L = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FVector PIK_KneeTargetCS_R = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FRotator PIK_FootRotationCS_L = FRotator::ZeroRotator;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output") FRotator PIK_FootRotationCS_R = FRotator::ZeroRotator;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|State") FPIKFootState PIK_FootState_L;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|State") FPIKFootState PIK_FootState_R;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Debug") FString PIK_Status = TEXT("Not initialized");
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Debug") int32 PIK_TraceCount = 0;

    UFUNCTION(BlueprintCallable, Category="PIK") void PIK_ResetPrediction();

    // Shared by runtime and editor automation tests; builds a bounded, ordered safety envelope.
    bool PIK_BuildFootPath(const FVector& StartWS, const FVector& EndWS, TArray<FVector>& OutPointsWS);
    static float PIK_ProjectProgress(const FVector& PointWS, const FVector& StartWS, const FVector& EndWS);
    static float PIK_SamplePathHeight(const TArray<FVector>& PointsWS, const FVector& PositionWS);

private:
    bool PIK_UpdateFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds, const FVector& VelocityWS, float PlayRate);
    bool PIK_TraceGround(const FVector& OriginWS, FHitResult& Hit);
    bool PIK_TraceSegment(const FVector& StartWS, const FVector& EndWS, FHitResult& Hit);
    bool PIK_IsWalkable(const FHitResult& Hit) const;
    bool PIK_IsTestPoseEligible(FString& Reason) const;
    float PIK_ReadALSFloat(FName Name, float Fallback) const;
    int64 PIK_ReadALSEnum(FName Name, int64 Fallback) const;
    void PIK_DrawFoot(const FPIKFootState& State, const FColor& Color) const;
    float PIK_PelvisOffsetWorldCm = 0.f;
    // Interpolate the support pivot in world space so capsule step-up cannot move it instantly.
    double PIK_PelvisPivotWorldZCm = 0.0;
    bool bPIK_PelvisPivotInitialized = false;
};
