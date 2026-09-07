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
    bPIK_ReferencePoseValid = false;
    PIK_ResetPrediction();
}

void UPIKAnimInstance::NativePostEvaluateAnimation()
{
    Super::NativePostEvaluateAnimation();
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    if (!Mesh) return;
    // In UE 5.7 post-evaluation runs before the component pose buffers are flipped.
    // Read the newly evaluated editable buffer, not last frame's socket transform.
    const TArray<FTransform>& Pose = Mesh->GetEditableComponentSpaceTransforms();
    const int32 LeftIndex = Mesh->GetBoneIndex(TEXT("ik_foot_l"));
    const int32 RightIndex = Mesh->GetBoneIndex(TEXT("ik_foot_r"));
    bPIK_ReferencePoseValid = Pose.IsValidIndex(LeftIndex) && Pose.IsValidIndex(RightIndex);
    if (!bPIK_ReferencePoseValid) return;
    PIK_ReferencePoseCS[0] = Pose[LeftIndex];
    PIK_ReferencePoseCS[1] = Pose[RightIndex];
    for (int32 Side=0; Side<2; ++Side)
    {
        PIK_ReferencePoseWS[Side] = PIK_ReferencePoseCS[Side] * Mesh->GetComponentTransform();
        const int32 Hip = Mesh->GetBoneIndex(Side==0 ? TEXT("thigh_l") : TEXT("thigh_r"));
        const int32 Knee = Mesh->GetBoneIndex(Side==0 ? TEXT("calf_l") : TEXT("calf_r"));
        const int32 Ankle = Mesh->GetBoneIndex(Side==0 ? TEXT("foot_l") : TEXT("foot_r"));
        PIK_LegLengthCS[Side] = 0.f;
        if (Pose.IsValidIndex(Hip) && Pose.IsValidIndex(Knee) && Pose.IsValidIndex(Ankle))
        {
            // Remove the applied pelvis translation to avoid feeding IK back into its reach limit.
            PIK_UnshiftedHipCS[Side] = Pose[Hip].GetLocation()-PIK_PelvisOffsetCS*PIK_Alpha;
            PIK_LegLengthCS[Side] = FVector::Distance(Pose[Hip].GetLocation(),Pose[Knee].GetLocation())+
                FVector::Distance(Pose[Knee].GetLocation(),Pose[Ankle].GetLocation());
        }
    }
}

void UPIKAnimInstance::PIK_GetReferenceFoot(bool bLeft, FVector& OutCS, FVector& OutWS) const
{
    if (bPIK_ReferencePoseValid)
    {
        const int32 Side = bLeft ? 0 : 1;
        OutCS = PIK_ReferencePoseCS[Side].GetLocation();
        OutWS = PIK_ReferencePoseWS[Side].GetLocation();
        return;
    }
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    OutCS = Mesh->GetSocketTransform(bLeft ? TEXT("ik_foot_l") : TEXT("ik_foot_r"),RTS_Component).GetLocation();
    OutWS = Mesh->GetComponentTransform().TransformPosition(OutCS);
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

bool UPIKAnimInstance::PIK_IsGroundPoseEligible(FString& Reason) const
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
    if (PIK_ReadALSEnum(TEXT("MovementState"), -1) != 1 ||
        PIK_ReadALSEnum(TEXT("MovementAction"), -1) != 0)
    { Reason = TEXT("Excluded: movement action or not grounded"); return false; }
    if (!Mesh->DoesSocketExist(TEXT("ik_foot_l")) || !Mesh->DoesSocketExist(TEXT("ik_foot_r")))
    { Reason = TEXT("Missing reference IK bones"); return false; }
    Reason = TEXT("Ground contact");
    return true;
}

bool UPIKAnimInstance::PIK_AreFootCurvesValid(bool bLeft) const
{
    const float Height = GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
    const float Time = GetCurveValue(bLeft ? TEXT("FootTimeToLand_L") : TEXT("FootTimeToLand_R"));
    // Missing curves evaluate to zero; never interpret that as a landing event.
    return FMath::IsFinite(Height) && Height >= 10.f && Height <= 40.f &&
        FMath::IsFinite(Time) && Time >= -0.001f && Time <= 1.2f;
}

bool UPIKAnimInstance::PIK_IsTestPoseEligible(FString& Reason) const
{
    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    if (!Character || PIK_ReadALSEnum(TEXT("Gait"),-1) != 0) return false;
    const FVector Velocity = PIK::Horizontal(Character->GetVelocity());
    if (Velocity.Size() < PIK_MinSpeedCmPerSec)
    { Reason = TEXT("Excluded: idle"); return false; }
    if (FVector::DotProduct(Velocity.GetSafeNormal(), PIK::Horizontal(Character->GetActorForwardVector()).GetSafeNormal()) < PIK_MinForwardDot)
    { Reason = TEXT("Excluded: not straight forward"); return false; }
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
    if (FMath::Abs(EndWS.Z-StartWS.Z) > PIK_MaxTerrainDeltaCm)
    {
        // A long stride along one walkable slope can rise more than the maximum step height.
        // Accept only coplanar walkable supports with an unobstructed connecting segment.
        FHitResult StartGround,EndGround,Obstruction;
        if (!PIK_TraceGround(StartWS,StartGround) || !PIK_TraceGround(EndWS,EndGround)) return false;
        if (!StartGround.ImpactPoint.Equals(StartWS,2.f) || !EndGround.ImpactPoint.Equals(EndWS,2.f) ||
            FVector::DotProduct(StartGround.ImpactNormal,EndGround.ImpactNormal)<0.99f ||
            FMath::Abs(FVector::DotProduct(EndWS-StartWS,StartGround.ImpactNormal))>2.f) return false;
        const FVector Lift=FVector::UpVector*PIK_PathSurfaceLiftCm;
        if (PIK_TraceSegment(StartWS+Lift,EndWS+Lift,Obstruction)) return false;
        OutPointsWS={StartWS,EndWS};
        return true;
    }
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

bool UPIKAnimInstance::PIK_UpdateGroundFoot(FPIKFootState& State, bool bLeft, bool bCurvesValid, float DeltaSeconds)
{
    const FTransform MeshToWorld = GetSkelMeshComponent()->GetComponentTransform();
    FVector RawCS,RawWS;
    PIK_GetReferenceFoot(bLeft,RawCS,RawWS);
    FHitResult Ground;
    if (!PIK_TraceGround(RawWS,Ground)) return false;
    const float ScaleZ = FMath::Abs(MeshToWorld.GetScale3D().Z);
    const bool bMoving = PIK::Horizontal(TryGetPawnOwner()->GetVelocity()).Size() >= PIK_MinSpeedCmPerSec;
    // Standing poses need not share the walk clip's reference ankle height. Do not preserve
    // their small vertical pose offsets as a permanent gap between sole and slope.
    State.GroundLiftCm = bMoving ? FMath::Max(0.f,static_cast<float>(RawCS.Z)-PIK_AnkleHeightCm)*ScaleZ :
        FMath::FInterpTo(State.GroundLiftCm,0.f,DeltaSeconds,PIK_ContactInterpSpeed);
    // Rotate the ankle-to-sole offset with the surface; a vertical offset alone sinks the toe on slopes.
    State.TargetAnkleWS = Ground.ImpactPoint + Ground.ImpactNormal * PIK_AnkleHeightCm * ScaleZ +
        FVector::UpVector * State.GroundLiftCm;
    State.PlantContactWS = Ground.ImpactPoint;
    State.PathStartWS = Ground.ImpactPoint;
    State.ContactNormalWS = Ground.ImpactNormal;
    const float Height = bCurvesValid ? GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R")) : RawCS.Z;
    State.bPlanted = Height <= (State.bPlanted ? PIK_ReleaseThresholdCm : PIK_PlantThresholdCm);
    State.bInitialized = true;
    State.bPathValid = false;
    State.bPredictionValid = false;
    State.PathPointsWS.Reset();
    State.TimeToLandSec = 0.f;
    State.PathProgress = 0.f;
    return true;
}

bool UPIKAnimInstance::PIK_UpdateFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds,
    const FVector& VelocityWS, float PlayRate, bool bRequestPrediction)
{
    const bool bWasInitialized = State.bInitialized;
    const bool bWasPredicting = State.bUsingPrediction;
    const FVector PreviousTargetWS = State.TargetAnkleWS;
    const bool bCurvesValid = PIK_AreFootCurvesValid(bLeft);
    const float Height = GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
    if (!State.bUsingPrediction)
    {
        // Each foot enters prediction at liftoff, never by switching both feet at a speed threshold.
        if (bRequestPrediction && bCurvesValid && State.bInitialized && State.bPlanted && Height > PIK_ReleaseThresholdCm)
            State.bUsingPrediction = true;
    }
    State.bExitPredictionPending = State.bUsingPrediction && !bRequestPrediction;
    State.ExitPredictionTimeSec = State.bExitPredictionPending ? State.ExitPredictionTimeSec + DeltaSeconds : 0.f;
    if (State.bExitPredictionPending && (!bCurvesValid || Height <= PIK_PlantThresholdCm ||
        State.ExitPredictionTimeSec >= PIK_MaxStopHandoffSec))
        State.bUsingPrediction = false;

    bool bValid = false;
    if (State.bUsingPrediction)
        bValid = PIK_UpdatePredictiveFoot(State,bLeft,DeltaSeconds,VelocityWS,PlayRate);
    if (!bValid)
    {
        State.bUsingPrediction = false;
        State.bExitPredictionPending = false;
        bValid = PIK_UpdateGroundFoot(State,bLeft,bCurvesValid && bRequestPrediction,DeltaSeconds);
    }
    if (!bValid) return false;
    if (bWasInitialized && bWasPredicting != State.bUsingPrediction)
        State.HandoffCorrectionWS = PreviousTargetWS - State.TargetAnkleWS;
    // Preserve the last output on the handoff frame, then release the correction smoothly.
    State.TargetAnkleWS += State.HandoffCorrectionWS;
    const float Decay = FMath::Exp(-FMath::Max(PIK_ContactInterpSpeed,0.1f)*DeltaSeconds);
    State.HandoffCorrectionWS *= Decay;
    State.SmoothedNormalWS = bWasInitialized ? FMath::Lerp(State.SmoothedNormalWS,State.ContactNormalWS,1.f-Decay).GetSafeNormal() : State.ContactNormalWS;
    return !State.TargetAnkleWS.ContainsNaN();
}

bool UPIKAnimInstance::PIK_UpdatePredictiveFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds, const FVector& VelocityWS, float PlayRate)
{
    USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    const FTransform MeshToWorld = Mesh->GetComponentTransform();
    // These reference bones are deliberately NEVER modified by the PredictIK animation layer.
    FVector RawFootCS,RawFootWS;
    PIK_GetReferenceFoot(bLeft,RawFootCS,RawFootWS);
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
    if (!State.bExitPredictionPending)
    {
        State.bPredictionValid = PIK_TraceGround(PredictionWS, PredictedGround);
        if (State.bPredictionValid)
        {
            State.PredictedContactWS = PredictedGround.ImpactPoint;
            const bool bSameSlope = FVector::DotProduct(State.ContactNormalWS,PredictedGround.ImpactNormal)>0.99f &&
                FMath::Abs(FVector::DotProduct(State.PredictedContactWS-State.PathStartWS,PredictedGround.ImpactNormal))<2.f;
            State.bPredictionValid = bSameSlope || FMath::Abs(State.PredictedContactWS.Z-State.PathStartWS.Z) <= PIK_MaxTerrainDeltaCm;
        }
    }

    const float WorldAnkleHeightCm = PIK_AnkleHeightCm * FMath::Abs(MeshToWorld.GetScale3D().Z);
    if (State.bPlanted)
    {
        State.TargetAnkleWS = State.PlantContactWS + State.ContactNormalWS*WorldAnkleHeightCm;
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
    // Close the swing/contact gap BEFORE the discrete landing event. At the plant threshold
    // this evaluates to precisely the same ground point + normal offset as the support branch.
    // This is pose-driven, with no held frame or time-lag filter on the moving foot target.
    const float SwingWeight = FMath::SmoothStep(PIK_PlantThresholdCm,
        PIK_PlantThresholdCm+FMath::Max(PIK_ContactBlendHeightCm,0.1f),HeightCurve);
    const FVector ContactAnkleWS = CurrentGround.ImpactPoint + CurrentGround.ImpactNormal*WorldAnkleHeightCm;
    const FVector SwingAnkleWS(RawFootWS.X,RawFootWS.Y,
        FMath::Max(PathHeightCm,static_cast<float>(CurrentGround.ImpactPoint.Z))+WorldAnkleHeightCm+AnimatedLiftCm);
    State.TargetAnkleWS = FMath::Lerp(ContactAnkleWS,SwingAnkleWS,SwingWeight);
    State.ContactNormalWS = CurrentGround.ImpactNormal;
    return !State.TargetAnkleWS.ContainsNaN();
}

void UPIKAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f) return;
    PIK_TraceCount = 0;
    FString Reason;
    const bool bEligible = PIK_IsGroundPoseEligible(Reason);
    const float Dt = FMath::Min(DeltaSeconds,0.1f);
    bool bValid = false;
    if (bEligible)
    {
        const FVector Velocity = PIK::Horizontal(TryGetPawnOwner()->GetVelocity());
        const float PlayRate = PIK_ReadALSFloat(TEXT("StandingPlayRate"),1.f);
        FString PredictionReason;
        const bool bRequestPrediction = PIK_IsTestPoseEligible(PredictionReason);
        const bool bLeftValid = PIK_UpdateFoot(PIK_FootState_L,true,Dt,Velocity,PlayRate,bRequestPrediction);
        const bool bRightValid = PIK_UpdateFoot(PIK_FootState_R,false,Dt,Velocity,PlayRate,bRequestPrediction);
        bValid = bLeftValid && bRightValid;
        Reason = bValid ? ((PIK_FootState_L.bUsingPrediction || PIK_FootState_R.bUsingPrediction) ? TEXT("Active: forward prediction/contact") : TEXT("Active: ground contact")) : TEXT("No valid ground; fading out");
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
    float SupportHeightCm = FMath::Min(SupportZ(PIK_FootState_L),SupportZ(PIK_FootState_R));
    FHitResult BodyGround, LeftGround, RightGround;
    // On a continuous plane, evaluate support beneath the body. A rear planted anchor
    // is a foot constraint, not the height the body must keep for the whole stride.
    if (PIK_TraceGround(MeshToWorld.GetLocation(),BodyGround) &&
        PIK_TraceGround(PIK_FootState_L.TargetAnkleWS,LeftGround) &&
        PIK_TraceGround(PIK_FootState_R.TargetAnkleWS,RightGround))
    {
        auto PlaneConfidence = [&](const FHitResult& FootGround)
        {
            const float NormalDot = FVector::DotProduct(BodyGround.ImpactNormal,FootGround.ImpactNormal);
            const float PlaneErrorCm = FMath::Abs(FVector::DotProduct(
                FootGround.ImpactPoint-BodyGround.ImpactPoint,BodyGround.ImpactNormal));
            return FMath::SmoothStep(0.97f,0.995f,NormalDot)*(1.f-FMath::SmoothStep(0.5f,2.f,PlaneErrorCm));
        };
        const float PlaneWeight = FMath::Min(PlaneConfidence(LeftGround),PlaneConfidence(RightGround));
        SupportHeightCm = FMath::Lerp(SupportHeightCm,static_cast<float>(BodyGround.ImpactPoint.Z),PlaneWeight);
    }
    float DesiredOffset = FMath::Clamp(SupportHeightCm-BaseFloorZ,
        -PIK_MaxPelvisOffsetCm,PIK_MaxPelvisOffsetCm);
    float MaxReachOffsetCm = PIK_MaxPelvisOffsetCm;
    for (int32 Side=0; Side<2; ++Side)
    {
        if (!bPIK_ReferencePoseValid || PIK_LegLengthCS[Side]<=0.f) continue;
        const FVector HipWS = MeshToWorld.TransformPosition(PIK_UnshiftedHipCS[Side]);
        const FVector TargetWS = Side==0 ? PIK_FootState_L.TargetAnkleWS : PIK_FootState_R.TargetAnkleWS;
        const float ReachCm = PIK_LegLengthCS[Side]*MeshToWorld.GetScale3D().GetAbsMin()*0.995f;
        const float VerticalReachCm = FMath::Sqrt(FMath::Max(0.f,
            FMath::Square(ReachCm)-static_cast<float>(FVector::DistSquaredXY(HipWS,TargetWS))));
        MaxReachOffsetCm = FMath::Min(MaxReachOffsetCm,static_cast<float>(TargetWS.Z-HipWS.Z)+VerticalReachCm);
    }
    MaxReachOffsetCm = FMath::Max(MaxReachOffsetCm,-PIK_MaxPelvisOffsetCm);
    DesiredOffset = FMath::Min(DesiredOffset,MaxReachOffsetCm);
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
        -PIK_MaxPelvisOffsetCm,MaxReachOffsetCm);
    // Keep the pivot consistent with reach limits; do not accumulate an unreachable height.
    PIK_PelvisPivotWorldZCm = BaseFloorZ + PIK_PelvisOffsetWorldCm;
    PIK_PelvisOffsetCS = MeshToWorld.InverseTransformVector(FVector::UpVector*PIK_PelvisOffsetWorldCm);
    // Absolute CS targets compensate pelvis immediately; no second interpolation introduces foot lag.
    PIK_FootTargetCS_L = MeshToWorld.InverseTransformPosition(PIK_FootState_L.TargetAnkleWS);
    PIK_FootTargetCS_R = MeshToWorld.InverseTransformPosition(PIK_FootState_R.TargetAnkleWS);
    const FVector ForwardCS = MeshToWorld.InverseTransformVectorNoScale(TryGetPawnOwner()->GetActorForwardVector());
    PIK_KneeTargetCS_L = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_foot_l"),RTS_Component).GetLocation() + ForwardCS*70.f + FVector(0,0,50) + PIK_PelvisOffsetCS;
    PIK_KneeTargetCS_R = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_foot_r"),RTS_Component).GetLocation() + ForwardCS*70.f + FVector(0,0,50) + PIK_PelvisOffsetCS;
    auto FootRotationCS = [&](FPIKFootState& State, FName Bone)
    {
        FVector Normal = State.SmoothedNormalWS.GetSafeNormal();
        FQuat Slope = FQuat::FindBetweenNormals(FVector::UpVector,Normal);
        FVector Axis; float Angle;
        Slope.ToAxisAndAngle(Axis,Angle);
        Slope = FQuat(Axis,FMath::Min(Angle,FMath::DegreesToRadians(35.f)));
        if (State.bUsingPrediction && !State.bPlanted)
        {
            const float FootHeight = GetCurveValue(Bone==TEXT("ik_foot_l") ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
            const float ContactWeight = 1.f-FMath::SmoothStep(PIK_PlantThresholdCm,
                PIK_PlantThresholdCm+FMath::Max(PIK_ContactBlendHeightCm,0.1f),FootHeight);
            // Path progress may stop short of 1 at physical contact; height closes that gap.
            Slope = FQuat::Slerp(FQuat::Identity,Slope,FMath::Max(ContactWeight,FMath::SmoothStep(0.7f,1.f,State.PathProgress)));
        }
        State.SmoothedSlopeWS = FQuat::Slerp(State.SmoothedSlopeWS,Slope,
            1.f-FMath::Exp(-FMath::Max(PIK_ContactInterpSpeed,0.1f)*Dt)).GetNormalized();
        const FQuat RawWS = bPIK_ReferencePoseValid ? PIK_ReferencePoseWS[Bone==TEXT("ik_foot_l") ? 0 : 1].GetRotation() : GetSkelMeshComponent()->GetSocketQuaternion(Bone);
        return (MeshToWorld.GetRotation().Inverse()*State.SmoothedSlopeWS*RawWS).Rotator();
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
