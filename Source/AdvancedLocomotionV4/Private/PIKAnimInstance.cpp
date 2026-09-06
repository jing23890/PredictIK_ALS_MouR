#include "PIKAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

namespace PIK
{
    constexpr float ProgressEpsilon = 0.001f;
    constexpr float PositionEpsilonCm = 0.1f;
    FVector Horizontal(const FVector& V) { return FVector(V.X, V.Y, 0.0); }
}

void UPIKAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    PIK_ResetPrediction();
}

void UPIKAnimInstance::PIK_ResetPrediction()
{
    PIK_FootState_L = FPIKFootState();
    PIK_FootState_R = FPIKFootState();
    PIK_Alpha = 0.f;
    PIK_PelvisOffsetWorldCm = 0.f;
    PIK_PelvisPivotWorldZCm = 0.0;
    bPIK_PelvisPivotInitialized = false;
    PIK_PelvisOffsetCS = FVector::ZeroVector;
    PIK_Status = TEXT("Waiting for forward walk");
}

float UPIKAnimInstance::PIK_ReadALSFloat(FName Name, float Fallback) const
{
    if (const FFloatProperty* P = FindFProperty<FFloatProperty>(GetClass(), Name))
        return P->GetPropertyValue_InContainer(this);
    if (const FDoubleProperty* P = FindFProperty<FDoubleProperty>(GetClass(), Name))
        return static_cast<float>(P->GetPropertyValue_InContainer(this));
    return Fallback;
}

int64 UPIKAnimInstance::PIK_ReadALSEnum(FName Name, int64 Fallback) const
{
    if (const FByteProperty* P = FindFProperty<FByteProperty>(GetClass(), Name))
        return P->GetPropertyValue_InContainer(this);
    if (const FEnumProperty* P = FindFProperty<FEnumProperty>(GetClass(), Name))
        return P->GetUnderlyingProperty()->GetSignedIntPropertyValue(P->ContainerPtrToValuePtr<void>(this));
    return Fallback;
}

bool UPIKAnimInstance::PIK_IsTestPoseEligible(FString& Reason) const
{
    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    if (!bPIK_Enabled) { Reason = TEXT("Disabled"); return false; }
    if (!Character || !Mesh || !GetWorld() || !GetWorld()->IsGameWorld())
    { Reason = TEXT("Waiting for game character"); return false; }
    if (!Character->GetCharacterMovement()->IsMovingOnGround() || Character->bIsCrouched)
    { Reason = TEXT("Excluded: air or crouch"); return false; }
    // ALS_Gait.Walking=0, ALS_MovementState.Grounded=1, ALS_MovementAction.None=0.
    // Missing properties fail closed rather than enabling the solver on another AnimBP.
    if (PIK_ReadALSEnum(TEXT("Gait"), -1) != 0 || PIK_ReadALSEnum(TEXT("MovementState"), -1) != 1 ||
        PIK_ReadALSEnum(TEXT("MovementAction"), -1) != 0)
    { Reason = TEXT("Excluded: only grounded walking gait"); return false; }
    const FVector Velocity = PIK::Horizontal(Character->GetVelocity());
    if (Velocity.Size() < PIK_MinSpeedCmPerSec)
    { Reason = TEXT("Excluded: idle"); return false; }
    if (FVector::DotProduct(Velocity.GetSafeNormal(), PIK::Horizontal(Character->GetActorForwardVector()).GetSafeNormal()) < PIK_MinForwardDot)
    { Reason = TEXT("Excluded: not straight forward"); return false; }
    if (!Mesh->DoesSocketExist(TEXT("ik_foot_l")) || !Mesh->DoesSocketExist(TEXT("ik_foot_r")))
    { Reason = TEXT("Missing reference IK bones"); return false; }
    // Height channels distinguish prepared forward poses from missing curves. Time==0 is VALID.
    for (FName Curve : { FName(TEXT("FootHeight_L")), FName(TEXT("FootHeight_R")) })
    {
        const float H = GetCurveValue(Curve);
        if (!FMath::IsFinite(H) || H < 10.f || H > 40.f)
        { Reason = TEXT("Waiting for prepared walk curves"); return false; }
    }
    Reason = TEXT("Forward walk");
    return true;
}

bool UPIKAnimInstance::PIK_IsWalkable(const FHitResult& Hit) const
{
    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    const float MinZ = Character ? Character->GetCharacterMovement()->GetWalkableFloorZ() : 0.71f;
    return Hit.bBlockingHit && !Hit.bStartPenetrating && Hit.ImpactNormal.Z >= MinZ &&
        !Hit.ImpactPoint.ContainsNaN();
}

bool UPIKAnimInstance::PIK_TraceSegment(const FVector& StartWS, const FVector& EndWS, FHitResult& Hit)
{
    if (!GetWorld()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PIKPath), false, TryGetPawnOwner());
    ++PIK_TraceCount;
    const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, StartWS, EndWS, PIK_TraceChannel, Params);
    if (bPIK_DrawDebug)
    {
        // Non-persistent, zero lifetime: show only the frame in which this trace ran.
        const FVector TraceEndWS = bHit ? Hit.ImpactPoint : EndWS;
        DrawDebugLine(GetWorld(), StartWS, TraceEndWS, bHit ? FColor::Green : FColor::Red, false, 0.f, 0, 0.7f);
        if (bHit)
        {
            DrawDebugLine(GetWorld(), Hit.ImpactPoint, EndWS, FColor::Red, false, 0.f, 0, 0.5f);
            DrawDebugBox(GetWorld(), Hit.ImpactPoint, FVector(2.5), FColor::Yellow, false, 0.f, 0, 1.f);
        }
    }
    return bHit;
}

bool UPIKAnimInstance::PIK_TraceGround(const FVector& OriginWS, FHitResult& Hit)
{
    const FVector StartWS = OriginWS + FVector::UpVector * PIK_TraceUpCm;
    const FVector EndWS = OriginWS - FVector::UpVector * PIK_TraceDownCm;
    return PIK_TraceSegment(StartWS, EndWS, Hit) && PIK_IsWalkable(Hit);
}

float UPIKAnimInstance::PIK_ProjectProgress(const FVector& PointWS, const FVector& StartWS, const FVector& EndWS)
{
    const FVector Delta = PIK::Horizontal(EndWS - StartWS);
    const double LengthSq = Delta.SizeSquared();
    if (LengthSq < 0.01) return 0.f;
    return FMath::Clamp(static_cast<float>(FVector::DotProduct(PIK::Horizontal(PointWS - StartWS), Delta) / LengthSq), 0.f, 1.f);
}

float UPIKAnimInstance::PIK_SamplePathHeight(const TArray<FVector>& PointsWS, const FVector& PositionWS)
{
    if (PointsWS.IsEmpty()) return 0.f;
    if (PointsWS.Num() == 1) return PointsWS[0].Z;
    const FVector& Start = PointsWS[0];
    const FVector& End = PointsWS.Last();
    const float U = PIK_ProjectProgress(PositionWS, Start, End);
    for (int32 Index = 1; Index < PointsWS.Num(); ++Index)
    {
        const float A = PIK_ProjectProgress(PointsWS[Index-1], Start, End);
        const float B = PIK_ProjectProgress(PointsWS[Index], Start, End);
        if (U <= B || Index == PointsWS.Num()-1)
        {
            const float T = B-A > PIK::ProgressEpsilon ? FMath::Clamp((U-A)/(B-A),0.f,1.f) : 1.f;
            return FMath::Lerp(static_cast<float>(PointsWS[Index-1].Z), static_cast<float>(PointsWS[Index].Z), T);
        }
    }
    return End.Z;
}

bool UPIKAnimInstance::PIK_BuildFootPath(const FVector& StartWS, const FVector& EndWS, TArray<FVector>& OutPointsWS)
{
    OutPointsWS.Reset(); // Failed builds never expose stale or partially constructed paths.
    if (StartWS.ContainsNaN() || EndWS.ContainsNaN() || !GetWorld()) return false;
    if (FMath::Abs(EndWS.Z-StartWS.Z) > PIK_MaxTerrainDeltaCm) return false;
    const FVector Direction = PIK::Horizontal(EndWS-StartWS).GetSafeNormal();
    if (Direction.IsNearlyZero()) { OutPointsWS = { StartWS, EndWS }; return true; }
    TArray<FVector> Front { StartWS }, Back { EndWS };
    FVector FrontWS = StartWS, BackWS = EndWS;
    const FVector Lift = FVector::UpVector * PIK_PathSurfaceLiftCm;

    auto Advance = [&](bool bFromFront, const FHitResult& WallHit) -> bool
    {
        if (WallHit.bStartPenetrating) return false;
        const FVector StepDirection = bFromFront ? Direction : -Direction;
        FVector ProbeWS = WallHit.ImpactPoint + StepDirection * PIK_EdgeProbeInsetCm;
        ProbeWS.Z = FMath::Max3(StartWS.Z, EndWS.Z, WallHit.ImpactPoint.Z);
        FHitResult TopHit;
        if (!PIK_TraceGround(ProbeWS, TopHit)) return false;
        if (TopHit.ImpactPoint.Z > FMath::Max(StartWS.Z,EndWS.Z) + PIK_MaxTerrainDeltaCm) return false;
        FVector NewPointWS = TopHit.ImpactPoint;
        // Begin lifting before a riser, and stay high until the heel clears its far edge.
        const float SpanCm = PIK::Horizontal(BackWS-FrontWS).Size();
        const float Margin = FMath::Min(PIK_FootClearanceMarginCm, SpanCm * 0.15f);
        NewPointWS -= StepDirection * Margin;
        const float U = PIK_ProjectProgress(NewPointWS, StartWS, EndWS);
        const float FrontU = PIK_ProjectProgress(FrontWS, StartWS, EndWS);
        const float BackU = PIK_ProjectProgress(BackWS, StartWS, EndWS);
        if (U <= FrontU + PIK::ProgressEpsilon || U >= BackU - PIK::ProgressEpsilon) return false;
        if (bFromFront) { Front.Add(NewPointWS); FrontWS=NewPointWS; }
        else { Back.Add(NewPointWS); BackWS=NewPointWS; }
        return true;
    };

    bool bClear = false;
    for (int32 Iteration=0; Iteration<FMath::Clamp(PIK_MaxPathIterations,1,12); ++Iteration)
    {
        FHitResult FrontHit;
        if (!PIK_TraceSegment(FrontWS+Lift, BackWS+Lift, FrontHit)) { bClear=true; break; }
        if (!Advance(true, FrontHit)) return false;
        FHitResult BackHit;
        if (!PIK_TraceSegment(BackWS+Lift, FrontWS+Lift, BackHit)) { bClear=true; break; }
        if (!Advance(false, BackHit)) return false;
    }
    if (!bClear)
    {
        FHitResult RemainingHit;
        bClear = !PIK_TraceSegment(FrontWS+Lift, BackWS+Lift, RemainingHit);
    }
    if (!bClear) return false;
    OutPointsWS = MoveTemp(Front);
    for (int32 Index=Back.Num()-1; Index>=0; --Index)
        if (!OutPointsWS.Last().Equals(Back[Index], PIK::PositionEpsilonCm)) OutPointsWS.Add(Back[Index]);
    return OutPointsWS.Num() >= 2;
}

bool UPIKAnimInstance::PIK_UpdateFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds, const FVector& VelocityWS, float PlayRate)
{
    USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    const FTransform MeshToWorld = Mesh->GetComponentTransform();
    const FName ReferenceBone = bLeft ? FName(TEXT("ik_foot_l")) : FName(TEXT("ik_foot_r"));
    // These reference bones are deliberately NEVER modified by the PredictIK animation layer.
    const FVector RawFootCS = Mesh->GetSocketTransform(ReferenceBone, RTS_Component).GetLocation();
    const FVector RawFootWS = MeshToWorld.TransformPosition(RawFootCS);
    const float HeightCurve = GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
    const float RawTimeSec = GetCurveValue(bLeft ? TEXT("FootTimeToLand_L") : TEXT("FootTimeToLand_R"));
    if (!FMath::IsFinite(RawTimeSec) || RawTimeSec < -0.001f || RawTimeSec > 1.2f) return false;
    State.TimeToLandSec = FMath::Clamp(RawTimeSec/FMath::Max(PlayRate,0.05f), 0.f, 1.5f);
    State.TimeSinceBuildSec += DeltaSeconds;
    const bool bWasPlanted = State.bPlanted;
    const float Threshold = bWasPlanted ? FMath::Max(PIK_ReleaseThresholdCm,PIK_PlantThresholdCm) : PIK_PlantThresholdCm;
    const bool bPlantedNow = HeightCurve <= Threshold;

    FHitResult CurrentGround;
    if (!PIK_TraceGround(RawFootWS,CurrentGround)) return false;
    const bool bJustInitialized = !State.bInitialized;
    if (bJustInitialized || (bPlantedNow && !bWasPlanted))
    {
        State.PlantContactWS = CurrentGround.ImpactPoint;
        State.ContactNormalWS = CurrentGround.ImpactNormal;
        State.PathStartWS = State.PlantContactWS;
        State.bInitialized = true;
        State.bPathValid = false;
        State.PathPointsWS.Reset();
    }
    // Update previous-state storage only AFTER deciding the edge.
    State.bPlanted = bPlantedNow;
    if (bWasPlanted && !bPlantedNow)
    {
        State.PathStartWS = State.PlantContactWS;
        State.bPathValid = false;
    }

    FVector ReferenceCS = bLeft ? PIK_LandingReferenceCS_L : PIK_LandingReferenceCS_R;
    ReferenceCS.Y *= FMath::Clamp(PIK_ReadALSFloat(TEXT("StrideBlend"),1.f),0.25f,2.f);
    ReferenceCS.Z = 0.f;
    const FVector PredictionWS = MeshToWorld.TransformPosition(ReferenceCS) + VelocityWS*State.TimeToLandSec;
    FHitResult PredictedGround;
    State.bPredictionValid = PIK_TraceGround(PredictionWS, PredictedGround);
    if (State.bPredictionValid)
    {
        State.PredictedContactWS = PredictedGround.ImpactPoint;
        State.bPredictionValid = FMath::Abs(State.PredictedContactWS.Z-State.PathStartWS.Z) <= PIK_MaxTerrainDeltaCm;
    }

    const float WorldAnkleHeightCm = PIK_AnkleHeightCm * FMath::Abs(MeshToWorld.GetScale3D().Z);
    if (State.bPlanted)
    {
        State.TargetAnkleWS = State.PlantContactWS + FVector::UpVector*WorldAnkleHeightCm;
        State.PathProgress = 0.f;
        return true; // A planted foot does not follow the NEXT cycle's prediction.
    }
    if (!State.bPredictionValid) return false;
    const bool bTargetMoved = FVector::DistSquared(State.LastBuildTargetWS,State.PredictedContactWS) > FMath::Square(PIK_RebuildDistanceCm);
    if (!State.bPathValid || (State.TimeSinceBuildSec >= PIK_RebuildIntervalSec && bTargetMoved))
    {
        State.bPathValid = PIK_BuildFootPath(State.PathStartWS,State.PredictedContactWS,State.PathPointsWS);
        State.LastBuildTargetWS = State.PredictedContactWS;
        State.TimeSinceBuildSec = 0.f;
        ++State.PathBuildCount;
    }
    if (!State.bPathValid) return false;
    State.PathProgress = PIK_ProjectProgress(RawFootWS,State.PathPointsWS[0],State.PathPointsWS.Last());
    const float PathHeightCm = PIK_SamplePathHeight(State.PathPointsWS,RawFootWS);
    const float AnimatedLiftCm = FMath::Max(0.f,static_cast<float>(RawFootCS.Z)-PIK_AnkleHeightCm) * FMath::Abs(MeshToWorld.GetScale3D().Z);
    State.TargetAnkleWS = RawFootWS;
    State.TargetAnkleWS.Z = FMath::Max(PathHeightCm,static_cast<float>(CurrentGround.ImpactPoint.Z)) + WorldAnkleHeightCm + AnimatedLiftCm;
    State.ContactNormalWS = CurrentGround.ImpactNormal;
    return !State.TargetAnkleWS.ContainsNaN();
}

void UPIKAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f) return;
    PIK_TraceCount = 0;
    FString Reason;
    const bool bEligible = PIK_IsTestPoseEligible(Reason);
    const float Dt = FMath::Min(DeltaSeconds,0.1f);
    bool bValid = false;
    if (bEligible)
    {
        const FVector Velocity = PIK::Horizontal(TryGetPawnOwner()->GetVelocity());
        const float PlayRate = PIK_ReadALSFloat(TEXT("StandingPlayRate"),1.f);
        const bool bLeftValid = PIK_UpdateFoot(PIK_FootState_L,true,Dt,Velocity,PlayRate);
        const bool bRightValid = PIK_UpdateFoot(PIK_FootState_R,false,Dt,Velocity,PlayRate);
        bValid = bLeftValid && bRightValid;
        Reason = bValid ? TEXT("Active: forward walk") : TEXT("No valid support/path; fading out");
    }
    PIK_Status = Reason;
    const float BlendSec = bValid ? PIK_BlendInSec : PIK_BlendOutSec;
    PIK_Alpha = FMath::FInterpConstantTo(PIK_Alpha,bValid?1.f:0.f,Dt,1.f/FMath::Max(BlendSec,0.01f));
    if (!bValid)
    {
        if (PIK_Alpha <= KINDA_SMALL_NUMBER)
        {
            PIK_FootState_L = FPIKFootState();
            PIK_FootState_R = FPIKFootState();
            PIK_PelvisOffsetWorldCm = 0.f;
            bPIK_PelvisPivotInitialized = false;
            PIK_PelvisOffsetCS = FVector::ZeroVector;
        }
        return;
    }

    const FTransform MeshToWorld = GetSkelMeshComponent()->GetComponentTransform();
    const float BaseFloorZ = MeshToWorld.GetLocation().Z;
    auto SupportZ = [&](const FPIKFootState& State)
    {
        float Z = State.PlantContactWS.Z;
        // Anticipate descending terrain only while the swing advances. Upward transfer waits for contact.
        if (!State.bPlanted && State.bPredictionValid && State.PredictedContactWS.Z < Z)
        {
            const float T = FMath::SmoothStep(0.2f,0.9f,State.PathProgress);
            Z = FMath::Lerp(Z,static_cast<float>(State.PredictedContactWS.Z),T);
        }
        return Z;
    };
    const float DesiredOffset = FMath::Clamp(FMath::Min(SupportZ(PIK_FootState_L),SupportZ(PIK_FootState_R))-BaseFloorZ,
        -PIK_MaxPelvisOffsetCm,PIK_MaxPelvisOffsetCm);
    if (!bPIK_PelvisPivotInitialized)
    {
        PIK_PelvisPivotWorldZCm = BaseFloorZ + PIK_PelvisOffsetWorldCm;
        bPIK_PelvisPivotInitialized = true;
    }
    // Smooth absolute support height, NOT the relative offset. When the capsule steps up/down,
    // subtract its new height immediately; smoothing that compensation again causes body pops.
    const double TargetPivotWorldZCm = BaseFloorZ + DesiredOffset;
    const double PivotBlend = 1.0 - FMath::Exp(-FMath::Max(PIK_PelvisInterpSpeed,0.f)*Dt);
    PIK_PelvisPivotWorldZCm = FMath::Lerp(PIK_PelvisPivotWorldZCm,TargetPivotWorldZCm,PivotBlend);
    PIK_PelvisOffsetWorldCm = FMath::Clamp(static_cast<float>(PIK_PelvisPivotWorldZCm-BaseFloorZ),
        -PIK_MaxPelvisOffsetCm,PIK_MaxPelvisOffsetCm);
    // Keep the pivot consistent with reach limits; do not accumulate an unreachable height.
    PIK_PelvisPivotWorldZCm = BaseFloorZ + PIK_PelvisOffsetWorldCm;
    PIK_PelvisOffsetCS = MeshToWorld.InverseTransformVector(FVector::UpVector*PIK_PelvisOffsetWorldCm);
    // Absolute CS targets compensate pelvis immediately; no second interpolation introduces foot lag.
    PIK_FootTargetCS_L = MeshToWorld.InverseTransformPosition(PIK_FootState_L.TargetAnkleWS);
    PIK_FootTargetCS_R = MeshToWorld.InverseTransformPosition(PIK_FootState_R.TargetAnkleWS);
    const FVector ForwardCS = MeshToWorld.InverseTransformVectorNoScale(TryGetPawnOwner()->GetActorForwardVector());
    PIK_KneeTargetCS_L = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_foot_l"),RTS_Component).GetLocation() + ForwardCS*70.f + FVector(0,0,50) + PIK_PelvisOffsetCS;
    PIK_KneeTargetCS_R = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_foot_r"),RTS_Component).GetLocation() + ForwardCS*70.f + FVector(0,0,50) + PIK_PelvisOffsetCS;
    auto FootRotationCS = [&](const FPIKFootState& State, FName Bone)
    {
        FVector Normal = State.ContactNormalWS.GetSafeNormal();
        FQuat Slope = FQuat::FindBetweenNormals(FVector::UpVector,Normal);
        FVector Axis; float Angle;
        Slope.ToAxisAndAngle(Axis,Angle);
        Slope = FQuat(Axis,FMath::Min(Angle,FMath::DegreesToRadians(35.f)));
        if (!State.bPlanted) Slope = FQuat::Slerp(FQuat::Identity,Slope,FMath::SmoothStep(0.7f,1.f,State.PathProgress));
        const FQuat RawWS = GetSkelMeshComponent()->GetSocketQuaternion(Bone);
        return (MeshToWorld.GetRotation().Inverse()*Slope*RawWS).Rotator();
    };
    PIK_FootRotationCS_L = FootRotationCS(PIK_FootState_L,TEXT("ik_foot_l"));
    PIK_FootRotationCS_R = FootRotationCS(PIK_FootState_R,TEXT("ik_foot_r"));
    if (bPIK_DrawDebug)
    {
        PIK_DrawFoot(PIK_FootState_L,FColor::Cyan);
        PIK_DrawFoot(PIK_FootState_R,FColor::Orange);
    }
}

void UPIKAnimInstance::PIK_DrawFoot(const FPIKFootState& State, const FColor& Color) const
{
    if (!State.bInitialized) return;
    DrawDebugBox(GetWorld(),State.PlantContactWS,FVector(3),FColor::Green,false,0.f,0,0.7f);
    if (State.bPredictionValid) DrawDebugBox(GetWorld(),State.PredictedContactWS,FVector(3),FColor::Red,false,0.f,0,0.7f);
    for (int32 Index=0; Index<State.PathPointsWS.Num(); ++Index)
    {
        DrawDebugBox(GetWorld(),State.PathPointsWS[Index],FVector(3),Color,false,0.f,0,1.f);
        if (Index) DrawDebugLine(GetWorld(),State.PathPointsWS[Index-1],State.PathPointsWS[Index],Color,false,0.f,0,1.f);
    }
    DrawDebugBox(GetWorld(),State.TargetAnkleWS,FVector(2),FColor::White,false,0.f,0,0.7f);
}
