#include "EditModelToolModule.h"
#include "EditModelToolSession.h"

#include "Async/Async.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"
#include "Misc/ScopedSlowTask.h"

#define LOCTEXT_NAMESPACE "FEditModelToolModule"

namespace
{
	struct FMeshPOD
	{
		FString MeshKey;
		FBox Bounds;
		FVector Center;
		float Volume;
	};

	struct FIndexPair
	{
		int32 A;
		int32 B;
	};

	constexpr float C_RotationToleranceDeg = 0.15f;
	constexpr float C_ScaleTolerance = 0.001f;
	constexpr float C_BoundsToleranceMin = 0.1f;

	FBox GetWorldBoxForMeshComponent(const UStaticMeshComponent* InMesh);

	float AdaptiveLinearTolWorldAABBs(const FBox& B0, const FBox& B1)
	{
		auto MaxCornerAbs = [](const FBox& B) -> float
		{
			return FMath::Max(B.Min.GetAbs().GetMax(), B.Max.GetAbs().GetMax());
		};
		const float CoordRef = FMath::Max(FMath::Max(MaxCornerAbs(B0), MaxCornerAbs(B1)), 1.f);
		const float ExtRef = FMath::Max(B0.GetExtent().GetMax(), B1.GetExtent().GetMax());
		/* Large-world float noise at ~2e5 coordinates can exceed 0.1; scale tol with magnitude/size. */
		return FMath::Max(C_BoundsToleranceMin, FMath::Max(CoordRef * 5e-6f, ExtRef * 5e-5f));
	}

	bool BoundsMinMaxNearlyEqualAdaptive(const FBox& B0, const FBox& B1)
	{
		const float T = AdaptiveLinearTolWorldAABBs(B0, B1);
		return FMath::IsNearlyEqual(B0.Min.X, B1.Min.X, T)
			&& FMath::IsNearlyEqual(B0.Min.Y, B1.Min.Y, T)
			&& FMath::IsNearlyEqual(B0.Min.Z, B1.Min.Z, T)
			&& FMath::IsNearlyEqual(B0.Max.X, B1.Max.X, T)
			&& FMath::IsNearlyEqual(B0.Max.Y, B1.Max.Y, T)
			&& FMath::IsNearlyEqual(B0.Max.Z, B1.Max.Z, T);
	}

	bool MeshComponentsPassGeometryOverlap(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
	{
		if (!M0 || !M1)
		{
			return false;
		}

		const FBox B0 = GetWorldBoxForMeshComponent(M0);
		const FBox B1 = GetWorldBoxForMeshComponent(M1);
		if (!B0.IsValid || !B1.IsValid)
		{
			return false;
		}
		return B0.Intersect(B1);
	}

	bool MeshComponentsPassCoincidence(
		UStaticMeshComponent* M0,
		UStaticMeshComponent* M1,
		const FBox& B0,
		const FBox& B1)
	{
		if (!M0 || !M1)
		{
			return false;
		}
		/* Do not compare pivots — identical overlapping hulls can differ in component origins. */
		const bool bSameRotation =
			M0->GetComponentRotation().Equals(M1->GetComponentRotation(), C_RotationToleranceDeg);
		const bool bSameScale = M0->GetComponentScale().Equals(M1->GetComponentScale(), C_ScaleTolerance);
		return bSameRotation && bSameScale && BoundsMinMaxNearlyEqualAdaptive(B0, B1);
	}

	void MakeActorMovable(AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}
		TArray<USceneComponent*> SceneComponents;
		Actor->GetComponents<USceneComponent>(SceneComponents);
		for (USceneComponent* SceneComp : SceneComponents)
		{
			if (SceneComp && SceneComp->Mobility != EComponentMobility::Movable)
			{
				SceneComp->Modify();
				SceneComp->SetMobility(EComponentMobility::Movable);
			}
		}
	}

	FBox GetWorldBoxForMeshComponent(const UStaticMeshComponent* InMesh)
	{
		check(InMesh);
		return InMesh->CalcBounds(InMesh->GetComponentTransform()).GetBox();
	}

	void ApplyZLiftToActor(AActor* ToMove, const float DeltaZWorld)
	{
		if (!ToMove)
		{
			return;
		}
		ToMove->Modify();
		MakeActorMovable(ToMove);
		ToMove->AddActorWorldOffset(FVector(0.f, 0.f, DeltaZWorld), false, nullptr, ETeleportType::None);
	}

	TArray<FIndexPair> BuildBroadPhaseCandidatePairs(const TArray<FMeshPOD>& PODs)
	{
		TArray<FIndexPair> Out;
		if (PODs.Num() < 2)
		{
			return Out;
		}

		constexpr int32 CellSize = 200;
		TSet<uint64> Seen;
		TMap<FIntVector, TArray<int32>> CellMap;
		for (int32 Id = 0; Id < PODs.Num(); ++Id)
		{
			const FMeshPOD& P = PODs[Id];
			const FIntVector C(
				FMath::FloorToInt(P.Center.X / CellSize),
				FMath::FloorToInt(P.Center.Y / CellSize),
				FMath::FloorToInt(P.Center.Z / CellSize));
			CellMap.FindOrAdd(C).Add(Id);
		}

		auto TryPair = [&Out, &PODs, &Seen](int32 I, int32 J)
		{
			if (I == J)
			{
				return;
			}
			const int32 A = I < J ? I : J;
			const int32 B = I < J ? J : I;
			const uint64 Key = (static_cast<uint64>(static_cast<uint32>(A)) << 32) | static_cast<uint32>(B);
			if (Seen.Contains(Key))
			{
				return;
			}

			const FMeshPOD& P0 = PODs[A];
			const FMeshPOD& P1 = PODs[B];
			if (!P0.Bounds.IsValid || !P1.Bounds.IsValid)
			{
				return;
			}

			const bool bBoundsIntersects = P0.Bounds.Intersect(P1.Bounds);
			const float CentDist = FVector::Distance(P0.Center, P1.Center);
			const float Denom = FMath::Max3(P0.Volume, P1.Volume, 1.f);
			const float VolRel = FMath::Abs(P0.Volume - P1.Volume) / Denom;
			const bool bCentersTight = CentDist < 5.f;
			const bool bVolTight = VolRel < 0.2f;
			if (!bBoundsIntersects && !(bCentersTight && bVolTight && CentDist < 25.f))
			{
				return;
			}

			const float BroadBoundsTol =
				FMath::Max(2.f, AdaptiveLinearTolWorldAABBs(P0.Bounds, P1.Bounds));
			const bool bBoundsSimilar =
				FMath::IsNearlyEqual(P0.Bounds.Min.X, P1.Bounds.Min.X, BroadBoundsTol) &&
				FMath::IsNearlyEqual(P0.Bounds.Min.Y, P1.Bounds.Min.Y, BroadBoundsTol) &&
				FMath::IsNearlyEqual(P0.Bounds.Min.Z, P1.Bounds.Min.Z, BroadBoundsTol) &&
				FMath::IsNearlyEqual(P0.Bounds.Max.X, P1.Bounds.Max.X, BroadBoundsTol) &&
				FMath::IsNearlyEqual(P0.Bounds.Max.Y, P1.Bounds.Max.Y, BroadBoundsTol) &&
				FMath::IsNearlyEqual(P0.Bounds.Max.Z, P1.Bounds.Max.Z, BroadBoundsTol);
			if (!bBoundsIntersects && !bBoundsSimilar)
			{
				return;
			}

			Seen.Add(Key);
			Out.Add(FIndexPair{A, B});
		};

		for (int32 Id = 0; Id < PODs.Num(); ++Id)
		{
			const FMeshPOD& P = PODs[Id];
			const FIntVector BaseCell(
				FMath::FloorToInt(P.Center.X / CellSize),
				FMath::FloorToInt(P.Center.Y / CellSize),
				FMath::FloorToInt(P.Center.Z / CellSize));
			for (int32 Dxi = -1; Dxi <= 1; ++Dxi)
			{
				for (int32 Dyi = -1; Dyi <= 1; ++Dyi)
				{
					for (int32 Dzi = -1; Dzi <= 1; ++Dzi)
					{
						const FIntVector C(BaseCell.X + Dxi, BaseCell.Y + Dyi, BaseCell.Z + Dzi);
						const TArray<int32>* InCell = CellMap.Find(C);
						if (!InCell)
						{
							continue;
						}
						for (int32 Other : *InCell)
						{
							if (Other > Id)
							{
								TryPair(Id, Other);
							}
						}
					}
				}
			}
		}
		return Out;
	}

	void RunBroadBCLiftForMeshSetAsync(
		TSharedPtr<TArray<TWeakObjectPtr<UStaticMeshComponent>>> SharedComps,
		TArray<FMeshPOD> PODs,
		const float DeltaZWorld,
		const FText& ProgressText,
		const FText& StageText)
	{
		if (!SharedComps.IsValid() || PODs.Num() < 2)
		{
			return;
		}

		Async(EAsyncExecution::ThreadPool, [PODs = MoveTemp(PODs)]() mutable
		{
			return BuildBroadPhaseCandidatePairs(PODs);
		})
			.Next([SharedComps, DeltaZWorld, ProgressText, StageText](TArray<FIndexPair> Cands)
			{
				AsyncTask(
					ENamedThreads::GameThread,
					[SharedComps, Cands, DeltaZWorld, ProgressText, StageText]()
					{
						const int32 WorkUnits = Cands.Num() + 1;
						FScopedSlowTask Progress(
							FMath::Max(1.f, static_cast<float>(WorkUnits)),
							ProgressText);
						Progress.EnterProgressFrame(1.f, StageText);

						if (Cands.Num() == 0)
						{
							FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoBroadCandidates", "No broad-phase candidate pairs found."));
							return;
						}

						int32 Applied = 0;
						{
							const FScopedTransaction Transaction(LOCTEXT("BroadBCLiftTx", "B+C Z lift for mesh pairs"));
							for (const FIndexPair& Pr : Cands)
							{
								Progress.EnterProgressFrame(1.f);
								if (!SharedComps->IsValidIndex(Pr.A) || !SharedComps->IsValidIndex(Pr.B))
								{
									continue;
								}
								UStaticMeshComponent* M0 = (*SharedComps)[Pr.A].Get();
								UStaticMeshComponent* M1 = (*SharedComps)[Pr.B].Get();
								if (!M0 || !M1 || M0 == M1)
								{
									continue;
								}
								AActor* A0 = M0->GetOwner();
								AActor* A1 = M1->GetOwner();
								if (!A0 || !A1 || A0 == A1)
								{
									continue;
								}
								const FBox B0 = GetWorldBoxForMeshComponent(M0);
								const FBox B1 = GetWorldBoxForMeshComponent(M1);
								if (!B0.IsValid || !B1.IsValid)
								{
									continue;
								}
								if (!MeshComponentsPassGeometryOverlap(M0, M1))
								{
									continue;
								}
								if (!MeshComponentsPassCoincidence(M0, M1, B0, B1))
								{
									continue;
								}
								AActor* const ToMove = (B0.Min.Z < B1.Min.Z) ? A0 : A1;
								ApplyZLiftToActor(ToMove, DeltaZWorld);
								++Applied;
							}
						}

						FMessageDialog::Open(
							EAppMsgType::Ok,
							FText::Format(
								LOCTEXT("BroadBCLiftDoneFmt", "Z offset per lift: {0} (world).  Candidate pairs: {1}.  Applied Z lift: {2}."),
								FText::AsNumber(DeltaZWorld),
								FText::AsNumber(Cands.Num()),
								FText::AsNumber(Applied)));
					});
			});
	}

	void LiftLowerMeshIfTwoOverlapImpl(const float DeltaZWorld)
	{
		USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
		if (!SelectedActors)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoSelectionForLiftZ", "No selection."));
			return;
		}

		TArray<TWeakObjectPtr<UStaticMeshComponent>> MeshComps;
		TArray<FMeshPOD> PODs;
		for (FSelectionIterator It(*SelectedActors); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (!Actor)
			{
				continue;
			}
			if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
			{
				continue;
			}
			TArray<UStaticMeshComponent*> Sms;
			Actor->GetComponents<UStaticMeshComponent>(Sms);
			for (UStaticMeshComponent* Sm : Sms)
			{
				if (!Sm)
				{
					continue;
				}
				UStaticMesh* Asset = Sm->GetStaticMesh();
				if (!Asset)
				{
					continue;
				}
				FMeshPOD Pod;
				Pod.MeshKey = Asset->GetPathName();
				Pod.Bounds = GetWorldBoxForMeshComponent(Sm);
				if (!Pod.Bounds.IsValid)
				{
					continue;
				}
				Pod.Center = Pod.Bounds.GetCenter();
				Pod.Volume = Pod.Bounds.GetVolume();
				MeshComps.Add(Sm);
				PODs.Add(MoveTemp(Pod));
			}
		}

		if (PODs.Num() < 2)
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				LOCTEXT("SelectionNeedMeshParts", "Select at least one actor with static mesh(es). Need at least two static mesh parts total (one actor with two meshes, or more actors) for overlap search."));
			return;
		}

		const TSharedPtr<TArray<TWeakObjectPtr<UStaticMeshComponent>>> SharedComps =
			MakeShared<TArray<TWeakObjectPtr<UStaticMeshComponent>>>(MoveTemp(MeshComps));
		RunBroadBCLiftForMeshSetAsync(
			SharedComps,
			MoveTemp(PODs),
			DeltaZWorld,
			LOCTEXT("SelectScanProgress", "Selected meshes: broad+BC+Z (game thread)"),
			LOCTEXT("SelectScanStage", "Starting B+C on selection candidates..."));
	}

	void AutoScanAllStaticMeshesLiftZAsyncImpl(const float DeltaZWorld)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoWorldAuto", "Could not get editor world."));
			return;
		}

		TArray<TWeakObjectPtr<UStaticMeshComponent>> MeshComps;
		TArray<FMeshPOD> PODs;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !IsValid(Actor))
			{
				continue;
			}
			if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
			{
				continue;
			}
			TArray<UStaticMeshComponent*> Sms;
			Actor->GetComponents<UStaticMeshComponent>(Sms);
			for (UStaticMeshComponent* Sm : Sms)
			{
				if (!Sm)
				{
					continue;
				}
				UStaticMesh* Asset = Sm->GetStaticMesh();
				if (!Asset)
				{
					continue;
				}
				FMeshPOD Pod;
				Pod.MeshKey = Asset->GetPathName();
				Pod.Bounds = GetWorldBoxForMeshComponent(Sm);
				if (!Pod.Bounds.IsValid)
				{
					continue;
				}
				Pod.Center = Pod.Bounds.GetCenter();
				Pod.Volume = Pod.Bounds.GetVolume();
				MeshComps.Add(Sm);
				PODs.Add(MoveTemp(Pod));
			}
		}

		if (PODs.Num() < 2)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NotEnoughMeshes", "Not enough static mesh components in the level."));
			return;
		}

		const TSharedPtr<TArray<TWeakObjectPtr<UStaticMeshComponent>>> SharedComps =
			MakeShared<TArray<TWeakObjectPtr<UStaticMeshComponent>>>(MoveTemp(MeshComps));
		RunBroadBCLiftForMeshSetAsync(
			SharedComps,
			MoveTemp(PODs),
			DeltaZWorld,
			LOCTEXT("AutoScanProgress", "Static mesh: broad+BC+Z (game thread)"),
			LOCTEXT("AutoScanStage", "Starting B+C on candidates..."));
	}
}

void FEditModelToolModule::LiftLowerMeshIfTwoOverlap(const float DeltaZWorld)
{
	LiftLowerMeshIfTwoOverlapImpl(DeltaZWorld);
}

void FEditModelToolModule::AutoScanAllStaticMeshesLiftZAsync(const float DeltaZWorld)
{
	AutoScanAllStaticMeshesLiftZAsyncImpl(DeltaZWorld);
}

#undef LOCTEXT_NAMESPACE
