// Fill out your copyright notice in the Description page of Project Settings.


#include "ALSRagdoll.h"
#include <Net/UnrealNetwork.h>
#include <Kismet/KismetSystemLibrary.h>
#include "Components/CapsuleComponent.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include <GameFramework/Character.h>
#include <Engine/NavigationObjectBase.h>
#include "GameFramework/Actor.h"

const FName NAME_Pelvis(TEXT("Pelvis"));
const FName NAME_RagdollPose(TEXT("RagdollPose"));
const FName NAME_root(TEXT("root"));
const FName NAME_pelvis(TEXT("pelvis"));
const FName NAME_spine_03(TEXT("spine_03"));

// Sets default values for this component's properties
UALSRagdoll::UALSRagdoll()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;
}

void UALSRagdoll::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UALSRagdoll, TargetRagdollLocation);
}


// Called when the game starts
void UALSRagdoll::BeginPlay()
{
	Super::BeginPlay();

	CharacterOwner = Cast<ACharacter>(GetOwner());
}


// Called every frame
void UALSRagdoll::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bIsRagdoll)
	{
		RagdollUpdate(DeltaTime);
	}
}

void UALSRagdoll::RagdollStart()
{
	if (!IsValid(CharacterOwner)) return;

	if (RagdollStateChangedDelegate.IsBound())
	{
		RagdollStateChangedDelegate.Broadcast(true);
	}

	/** When Networked, disables replicate movement reset TargetRagdollLocation and ServerRagdollPull variable
	and if the host is a dedicated server, change character mesh optimisation option to avoid z-location bug*/
	//MyCharacterMovementComponent->bIgnoreClientMovementErrorChecksAndCorrection = 1;

	if (UKismetSystemLibrary::IsDedicatedServer(GetWorld()))
	{
		DefVisBasedTickOp = CharacterOwner->GetMesh()->VisibilityBasedAnimTickOption;
		CharacterOwner->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}
	TargetRagdollLocation = CharacterOwner->GetMesh()->GetSocketLocation(NAME_Pelvis);
	ServerRagdollPull = 0;

	// Disable URO
	bPreRagdollURO = CharacterOwner->GetMesh()->bEnableUpdateRateOptimizations;
	CharacterOwner->GetMesh()->bEnableUpdateRateOptimizations = false;

	// Step 1: Clear the Character Movement Mode and set the Movement State to Ragdoll
	CharacterOwner->GetCharacterMovement()->SetMovementMode(MOVE_None);
	//SetMovementState(EALSMovementState::Ragdoll);
	bIsRagdoll = true;

	// Step 2: Disable capsule collision and enable mesh physics simulation starting from the pelvis.
	CharacterOwner->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CharacterOwner->GetMesh()->SetCollisionObjectType(ECC_PhysicsBody);
	CharacterOwner->GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CharacterOwner->GetMesh()->SetAllBodiesBelowSimulatePhysics(NAME_Pelvis, true, true);

	// Step 3: Stop any active montages.
	if (CharacterOwner->GetMesh()->GetAnimInstance())
	{
		CharacterOwner->GetMesh()->GetAnimInstance()->Montage_Stop(0.2f);
	}

	// Fixes character mesh is showing default A pose for a split-second just before ragdoll ends in listen server games
	CharacterOwner->GetMesh()->bOnlyAllowAutonomousTickPose = true;

	CharacterOwner->SetReplicateMovement(false);
}

void UALSRagdoll::RagdollEnd()
{
	if (!IsValid(CharacterOwner)) return;

	/** Re-enable Replicate Movement and if the host is a dedicated server set mesh visibility based anim
	tick option back to default*/

	if (UKismetSystemLibrary::IsDedicatedServer(GetWorld()))
	{
		CharacterOwner->GetMesh()->VisibilityBasedAnimTickOption = DefVisBasedTickOp;
	}

	CharacterOwner->GetMesh()->bEnableUpdateRateOptimizations = bPreRagdollURO;

	// Revert back to default settings
	//MyCharacterMovementComponent->bIgnoreClientMovementErrorChecksAndCorrection = 0;
	CharacterOwner->GetMesh()->bOnlyAllowAutonomousTickPose = false;
	CharacterOwner->SetReplicateMovement(true);

	// Step 1: Save a snapshot of the current Ragdoll Pose for use in AnimGraph to blend out of the ragdoll
	if (CharacterOwner->GetMesh()->GetAnimInstance())
	{
		CharacterOwner->GetMesh()->GetAnimInstance()->SavePoseSnapshot(NAME_RagdollPose);
	}

	// Step 2: If the ragdoll is on the ground, set the movement mode to walking and play a Get Up animation.
	// If not, set the movement mode to falling and update the character movement velocity to match the last ragdoll velocity.
	if (bRagdollOnGround)
	{
		CharacterOwner->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		if (CharacterOwner->GetMesh()->GetAnimInstance())
		{
			CharacterOwner->GetMesh()->GetAnimInstance()->Montage_Play(bRagdollFaceUp ? BackMontage : FrontMontage, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, true);
		}
	}
	else
	{
		CharacterOwner->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		CharacterOwner->GetCharacterMovement()->Velocity = LastRagdollVelocity;
	}

	// Step 3: Re-Enable capsule collision, and disable physics simulation on the mesh.
	CharacterOwner->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CharacterOwner->GetMesh()->SetCollisionObjectType(ECC_PhysicsBody);
	CharacterOwner->GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CharacterOwner->GetMesh()->SetAllBodiesSimulatePhysics(false);
	bIsRagdoll = false;

	if (RagdollStateChangedDelegate.IsBound())
	{
		RagdollStateChangedDelegate.Broadcast(false);
	}
}

void UALSRagdoll::Server_SetMeshLocationDuringRagdoll_Implementation(FVector MeshLocation)
{
	TargetRagdollLocation = MeshLocation;
}

void UALSRagdoll::Server_RagdollStart_Implementation()
{
	Multicast_RagdollStart();
}

void UALSRagdoll::Multicast_RagdollStart_Implementation()
{
	RagdollStart();
}

void UALSRagdoll::Server_RagdollEnd_Implementation(FVector CharacterLocation)
{
	Multicast_RagdollEnd(CharacterLocation);
}

void UALSRagdoll::Multicast_RagdollEnd_Implementation(FVector CharacterLocation)
{
	RagdollEnd();
}

void UALSRagdoll::RagdollUpdate(float DeltaTime)
{
	if (!IsValid(CharacterOwner)) return;

	CharacterOwner->GetMesh()->bOnlyAllowAutonomousTickPose = false;

	// Set the Last Ragdoll Velocity.
	const FVector NewRagdollVel = CharacterOwner->GetMesh()->GetPhysicsLinearVelocity(NAME_root);
	LastRagdollVelocity = (NewRagdollVel != FVector::ZeroVector || CharacterOwner->IsLocallyControlled())
		? NewRagdollVel
		: LastRagdollVelocity / 2;

	// Use the Ragdoll Velocity to scale the ragdoll's joint strength for physical animation.
	const float SpringValue = FMath::GetMappedRangeValueClamped<float, float>({ 0.0f, 1000.0f }, { 0.0f, 25000.0f },
		LastRagdollVelocity.Size());
	CharacterOwner->GetMesh()->SetAllMotorsAngularDriveParams(SpringValue, 0.0f, 0.0f, false);

	// Disable Gravity if falling faster than -4000 to prevent continual acceleration.
	// This also prevents the ragdoll from going through the floor.
	const bool bEnableGrav = LastRagdollVelocity.Z > -4000.0f;
	CharacterOwner->GetMesh()->SetEnableGravity(bEnableGrav);

	// Update the Actor location to follow the ragdoll.
	SetActorLocationDuringRagdoll(DeltaTime);
}

void UALSRagdoll::SetActorLocationDuringRagdoll(float DeltaTime)
{
	if (!IsValid(CharacterOwner)) return;

	if (CharacterOwner->IsLocallyControlled())
	{
		// Set the pelvis as the target location.
		TargetRagdollLocation = CharacterOwner->GetMesh()->GetSocketLocation(NAME_Pelvis);
		if (!CharacterOwner->HasAuthority())
		{
			Server_SetMeshLocationDuringRagdoll(TargetRagdollLocation);
		}
	}

	// Determine whether the ragdoll is facing up or down and set the target rotation accordingly.
	const FRotator PelvisRot = CharacterOwner->GetMesh()->GetSocketRotation(NAME_Pelvis);

	if (bReversedPelvis) {
		bRagdollFaceUp = PelvisRot.Roll > 0.0f;
	}
	else
	{
		bRagdollFaceUp = PelvisRot.Roll < 0.0f;
	}


	const FRotator TargetRagdollRotation(0.0f, bRagdollFaceUp ? PelvisRot.Yaw - 180.0f : PelvisRot.Yaw, 0.0f);

	// Trace downward from the target location to offset the target location,
	// preventing the lower half of the capsule from going through the floor when the ragdoll is laying on the ground.
	const FVector TraceVect(TargetRagdollLocation.X, TargetRagdollLocation.Y,
		TargetRagdollLocation.Z - CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());

	UWorld* World = GetWorld();
	check(World);

	FCollisionQueryParams Params;
	Params.AddIgnoredActor(CharacterOwner);

	FHitResult HitResult;
	const bool bHit = World->LineTraceSingleByChannel(HitResult, TargetRagdollLocation, TraceVect,
		ECC_Visibility, Params);

	/*if (ALSDebugComponent && ALSDebugComponent->GetShowTraces())
	{
		UALSDebugComponent::DrawDebugLineTraceSingle(World,
			TargetRagdollLocation,
			TraceVect,
			EDrawDebugTrace::Type::ForOneFrame,
			bHit,
			HitResult,
			FLinearColor::Red,
			FLinearColor::Green,
			1.0f);
	}*/

	bRagdollOnGround = HitResult.IsValidBlockingHit();
	FVector NewRagdollLoc = TargetRagdollLocation;

	if (bRagdollOnGround)
	{
		const float ImpactDistZ = FMath::Abs(HitResult.ImpactPoint.Z - HitResult.TraceStart.Z);
		NewRagdollLoc.Z += CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - ImpactDistZ + 2.0f;
	}
	if (!CharacterOwner->IsLocallyControlled())
	{
		ServerRagdollPull = FMath::FInterpTo(ServerRagdollPull, 750.0f, DeltaTime, 0.6f);
		float RagdollSpeed = FVector(LastRagdollVelocity.X, LastRagdollVelocity.Y, 0).Size();
		FName RagdollSocketPullName = RagdollSpeed > 300 ? NAME_spine_03 : NAME_pelvis;
		CharacterOwner->GetMesh()->AddForce(
			(TargetRagdollLocation - CharacterOwner->GetMesh()->GetSocketLocation(RagdollSocketPullName)) * ServerRagdollPull,
			RagdollSocketPullName, true);
	}
	SetActorLocationAndTargetRotation(bRagdollOnGround ? NewRagdollLoc : TargetRagdollLocation, TargetRagdollRotation);
}

void UALSRagdoll::SetActorLocationAndTargetRotation(FVector NewLocation, FRotator NewRotation)
{
	if (!IsValid(CharacterOwner)) return;

	CharacterOwner->SetActorLocationAndRotation(NewLocation, NewRotation);
	TargetRotation = NewRotation;
}

void UALSRagdoll::ReplicatedRagdollStart()
{
	if (!IsValid(CharacterOwner)) return;

	if (CharacterOwner->HasAuthority())
	{
		Multicast_RagdollStart();
	}
	else
	{
		Server_RagdollStart();
	}
}

void UALSRagdoll::ReplicatedRagdollEnd()
{
	if (!IsValid(CharacterOwner)) return;

	if (CharacterOwner->HasAuthority())
	{
		Multicast_RagdollEnd(CharacterOwner->GetActorLocation());
	}
	else
	{
		Server_RagdollEnd(CharacterOwner->GetActorLocation());
	}
}

