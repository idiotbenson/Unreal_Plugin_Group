#include "EditModelToolPrivatePCH.h"

#include "EditModelToolModule.h"
#include "EditModelToolSession.h"

#include "Core/EditModelToolSelectionUtils.h"
#include "Core/EditModelToolStaticMeshGeometryFingerprint.h"

#include "Async/Async.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "StaticMeshResources.h"
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
		int32 Lod0TriCount = INDEX_NONE;
		int32 Lod0VertexCount = INDEX_NONE;
		uint64 Lod0GeometryHash = 0;
		FString AttachParentActorPath;
		FString DerivedDataKey;
	};

	struct FIndexPair
	{
		int32 A;
		int32 B;
	};

	struct FValidatedPair
	{
		int32 A = 0;
		int32 B = 0;
		int32 MatchScore = 0;
	};

	constexpr float C_RotationToleranceDeg = 0.15f;
	constexpr float C_ScaleTolerance = 0.001f;
	constexpr float C_BoundsToleranceMin = 0.1f;
	/** Coincidence min/max compare capped vs mesh size so large-world coord slack cannot admit shifted hulls. */
	constexpr float C_MaxCoincidenceTolExtentFraction = 0.005f;

	FBox GetWorldBoxForMeshComponent(const UStaticMeshComponent* InMesh);

	int32 TryGetLod0TriangleCount(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return INDEX_NONE;
		}
		const int32 N = Mesh->GetNumTriangles(0);
		return (N >= 0) ? N : INDEX_NONE;
	}

	int32 TryGetLod0VertexCount(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return INDEX_NONE;
		}
		const int32 N = Mesh->GetNumVertices(0);
		return (N >= 0) ? N : INDEX_NONE;
	}

	bool Lod0GeometryCountsMatch(const int32 VertA, const int32 VertB, const int32 TriA, const int32 TriB)
	{
		return VertA >= 0 && VertB >= 0 && TriA != INDEX_NONE && TriB != INDEX_NONE && VertA == VertB && TriA == TriB;
	}

	FString TryGetStaticMeshDerivedDataKey(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return FString();
		}
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData)
		{
			return FString();
		}
		return RenderData->DerivedDataKey;
	}

	bool StaticMeshDerivedDataKeysMatch(const FString& KeyA, const FString& KeyB)
	{
		return !KeyA.IsEmpty() && KeyA == KeyB;
	}

	bool StaticMeshesShareEquivalentLod0Geometry(const UStaticMesh* MeshA, const UStaticMesh* MeshB)
	{
		const int32 VertA = TryGetLod0VertexCount(MeshA);
		const int32 VertB = TryGetLod0VertexCount(MeshB);
		const int32 TriA = TryGetLod0TriangleCount(MeshA);
		const int32 TriB = TryGetLod0TriangleCount(MeshB);
		if (!Lod0GeometryCountsMatch(VertA, VertB, TriA, TriB))
		{
			return false;
		}

		uint64 HashA = 0;
		uint64 HashB = 0;
		if (!EditModelToolStaticMeshGeometry::TryComputeLod0Fingerprint(MeshA, HashA)
			|| !EditModelToolStaticMeshGeometry::TryComputeLod0Fingerprint(MeshB, HashB))
		{
			return false;
		}
		if (!EditModelToolStaticMeshGeometry::Lod0FingerprintsMatch(HashA, HashB))
		{
			return false;
		}
		return EditModelToolStaticMeshGeometry::IsStrongLod0Fingerprint(HashA);
	}

	bool PodMeshesShareEquivalentLod0Geometry(const FMeshPOD& P0, const FMeshPOD& P1)
	{
		if (!Lod0GeometryCountsMatch(P0.Lod0VertexCount, P1.Lod0VertexCount, P0.Lod0TriCount, P1.Lod0TriCount))
		{
			return false;
		}
		if (!EditModelToolStaticMeshGeometry::Lod0FingerprintsMatch(P0.Lod0GeometryHash, P1.Lod0GeometryHash))
		{
			return false;
		}
		return EditModelToolStaticMeshGeometry::IsStrongLod0Fingerprint(P0.Lod0GeometryHash);
	}

	bool PodMeshesShareEquivalentCookedGeometry(const FMeshPOD& P0, const FMeshPOD& P1)
	{
		if (P0.MeshKey == P1.MeshKey)
		{
			return true;
		}
		if (StaticMeshDerivedDataKeysMatch(P0.DerivedDataKey, P1.DerivedDataKey))
		{
			return true;
		}
		return PodMeshesShareEquivalentLod0Geometry(P0, P1);
	}

	bool StaticMeshesShareEquivalentShape(const UStaticMesh* MeshA, const UStaticMesh* MeshB)
	{
		if (!MeshA || !MeshB)
		{
			return false;
		}
		if (MeshA == MeshB)
		{
			return true;
		}
		const FString KeyA = TryGetStaticMeshDerivedDataKey(MeshA);
		const FString KeyB = TryGetStaticMeshDerivedDataKey(MeshB);
		if (StaticMeshDerivedDataKeysMatch(KeyA, KeyB))
		{
			return true;
		}
		return StaticMeshesShareEquivalentLod0Geometry(MeshA, MeshB);
	}

	bool PodMeshesShareEquivalentShape(const FMeshPOD& P0, const FMeshPOD& P1)
	{
		return PodMeshesShareEquivalentCookedGeometry(P0, P1);
	}

	void FillMeshPODFields(FMeshPOD& Pod, UStaticMeshComponent* Sm, const UStaticMesh* Asset)
	{
		Pod.MeshKey = Asset->GetPathName();
		Pod.Lod0TriCount = TryGetLod0TriangleCount(Asset);
		Pod.Lod0VertexCount = TryGetLod0VertexCount(Asset);
		Pod.Lod0GeometryHash = 0;
		EditModelToolStaticMeshGeometry::TryComputeLod0Fingerprint(Asset, Pod.Lod0GeometryHash);
		Pod.DerivedDataKey = TryGetStaticMeshDerivedDataKey(Asset);
		Pod.AttachParentActorPath.Reset();
		if (const AActor* Owner = Sm->GetOwner())
		{
			if (const AActor* AttachParent = Owner->GetAttachParentActor())
			{
				Pod.AttachParentActorPath = AttachParent->GetPathName();
			}
		}
	}

	bool ActorsShareSameAttachParent(const AActor* A0, const AActor* A1)
	{
		if (!A0 || !A1 || A0 == A1)
		{
			return false;
		}
		const AActor* P0 = A0->GetAttachParentActor();
		const AActor* P1 = A1->GetAttachParentActor();
		return P0 && P1 && P0 == P1;
	}

	/**
	 * Same attach host + different mesh assets with different cooked geometry = BIM layers (wall + plasterboard).
	 * Same host + same mesh asset, or same DerivedDataKey (Path/Path2), remains eligible.
	 */
	bool PodPairBlockedAsCoLocatedHostLayers(const FMeshPOD& P0, const FMeshPOD& P1)
	{
		if (P0.AttachParentActorPath.IsEmpty() || P0.AttachParentActorPath != P1.AttachParentActorPath)
		{
			return false;
		}
		if (P0.MeshKey == P1.MeshKey)
		{
			return false;
		}
		if (StaticMeshDerivedDataKeysMatch(P0.DerivedDataKey, P1.DerivedDataKey))
		{
			return false;
		}
		if (PodMeshesShareEquivalentLod0Geometry(P0, P1))
		{
			return false;
		}
		return true;
	}

	bool MeshComponentsBlockedAsCoLocatedHostLayers(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
	{
		if (!M0 || !M1)
		{
			return false;
		}
		AActor* A0 = M0->GetOwner();
		AActor* A1 = M1->GetOwner();
		if (!ActorsShareSameAttachParent(A0, A1))
		{
			return false;
		}
		const UStaticMesh* Asset0 = M0->GetStaticMesh();
		const UStaticMesh* Asset1 = M1->GetStaticMesh();
		if (!Asset0 || !Asset1 || Asset0 == Asset1)
		{
			return false;
		}
		if (StaticMeshesShareEquivalentLod0Geometry(Asset0, Asset1))
		{
			return false;
		}
		return !StaticMeshDerivedDataKeysMatch(
			TryGetStaticMeshDerivedDataKey(Asset0),
			TryGetStaticMeshDerivedDataKey(Asset1));
	}

	bool MeshComponentsShareSceneAttachSubtree(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
	{
		if (!M0 || !M1 || M0 == M1)
		{
			return false;
		}
		for (const USceneComponent* Walk = M0->GetAttachParent(); Walk; Walk = Walk->GetAttachParent())
		{
			if (Walk == M1)
			{
				return true;
			}
		}
		for (const USceneComponent* Walk = M1->GetAttachParent(); Walk; Walk = Walk->GetAttachParent())
		{
			if (Walk == M0)
			{
				return true;
			}
		}
		const USceneComponent* Root0 = M0->GetAttachmentRoot();
		const USceneComponent* Root1 = M1->GetAttachmentRoot();
		if (Root0 && Root1 && Root0 == Root1 && M0->GetOwner() != M1->GetOwner())
		{
			const UStaticMesh* Asset0 = M0->GetStaticMesh();
			const UStaticMesh* Asset1 = M1->GetStaticMesh();
			if (Asset0 && Asset1 && Asset0 != Asset1
				&& !StaticMeshesShareEquivalentShape(Asset0, Asset1))
			{
				return true;
			}
		}
		return false;
	}

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

	float CoincidenceLinearTolWorldAABBs(const FBox& B0, const FBox& B1)
	{
		const float ExtRef = FMath::Max(B0.GetExtent().GetMax(), B1.GetExtent().GetMax());
		const float ExtCap = FMath::Max(ExtRef * C_MaxCoincidenceTolExtentFraction, C_BoundsToleranceMin);
		return FMath::Min(AdaptiveLinearTolWorldAABBs(B0, B1), ExtCap);
	}

	bool BoundsMinMaxNearlyEqualAdaptive(const FBox& B0, const FBox& B1)
	{
		const float T = CoincidenceLinearTolWorldAABBs(B0, B1);
		return FMath::IsNearlyEqual(B0.Min.X, B1.Min.X, T)
			&& FMath::IsNearlyEqual(B0.Min.Y, B1.Min.Y, T)
			&& FMath::IsNearlyEqual(B0.Min.Z, B1.Min.Z, T)
			&& FMath::IsNearlyEqual(B0.Max.X, B1.Max.X, T)
			&& FMath::IsNearlyEqual(B0.Max.Y, B1.Max.Y, T)
			&& FMath::IsNearlyEqual(B0.Max.Z, B1.Max.Z, T);
	}

	/**
	 * True duplicate = coincident world hull (min/max match), not merely high AABB intersection volume.
	 * Rejects adjacent segments whose elongated boxes share volume but differ in placement.
	 */
	bool WorldAabbsAreCoincidentDuplicates(const FBox& B0, const FBox& B1)
	{
		if (!B0.IsValid || !B1.IsValid || !B0.Intersect(B1))
		{
			return false;
		}
		if (!BoundsMinMaxNearlyEqualAdaptive(B0, B1))
		{
			return false;
		}
		const float CentDist = FVector::Distance(B0.GetCenter(), B1.GetCenter());
		return CentDist <= CoincidenceLinearTolWorldAABBs(B0, B1);
	}

	bool ActorsAreInAttachHierarchy(const AActor* A0, const AActor* A1)
	{
		if (!A0 || !A1)
		{
			return false;
		}
		for (const AActor* P = A0->GetAttachParentActor(); P; P = P->GetAttachParentActor())
		{
			if (P == A1)
			{
				return true;
			}
		}
		for (const AActor* P = A1->GetAttachParentActor(); P; P = P->GetAttachParentActor())
		{
			if (P == A0)
			{
				return true;
			}
		}
		return false;
	}

	bool MeshComponentsShareAttachmentHierarchy(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
	{
		if (!M0 || !M1)
		{
			return false;
		}
		AActor* A0 = M0->GetOwner();
		AActor* A1 = M1->GetOwner();
		if (ActorsAreInAttachHierarchy(A0, A1))
		{
			return true;
		}
		if (MeshComponentsBlockedAsCoLocatedHostLayers(M0, M1))
		{
			return true;
		}
		if (MeshComponentsShareSceneAttachSubtree(M0, M1))
		{
			return true;
		}
		for (const USceneComponent* P = M0->GetAttachParent(); P; P = P->GetAttachParent())
		{
			if (P == M1 || P->GetOwner() == A1)
			{
				return true;
			}
		}
		for (const USceneComponent* P = M1->GetAttachParent(); P; P = P->GetAttachParent())
		{
			if (P == M0 || P->GetOwner() == A0)
			{
				return true;
			}
		}
		return false;
	}

	bool MeshComponentsPassGeometryOverlap(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
	{
		if (!M0 || !M1)
		{
			return false;
		}

		const FBox B0 = GetWorldBoxForMeshComponent(M0);
		const FBox B1 = GetWorldBoxForMeshComponent(M1);
		return WorldAabbsAreCoincidentDuplicates(B0, B1);
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
		if (!StaticMeshesShareEquivalentShape(M0->GetStaticMesh(), M1->GetStaticMesh()))
		{
			return false;
		}
		/* Do not compare pivots — identical overlapping hulls can differ in component origins. */
		const float AngleDeg = FMath::RadiansToDegrees(
			M0->GetComponentQuat().AngularDistance(M1->GetComponentQuat()));
		const bool bSameRotation = AngleDeg <= C_RotationToleranceDeg;
		const bool bSameScale = M0->GetComponentScale().Equals(M1->GetComponentScale(), C_ScaleTolerance);
		return bSameRotation && bSameScale && WorldAabbsAreCoincidentDuplicates(B0, B1);
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

	void ApplyPairSelectionAndHideUnpairedMeshes(
		const TArray<TWeakObjectPtr<UStaticMeshComponent>>& MeshComps,
		const TArray<FValidatedPair>& ExclusivePairs)
	{
		if (ExclusivePairs.Num() == 0)
		{
			return;
		}

		TSet<AActor*> VisibleActors;
		TSet<AActor*> ScannedActors;
		VisibleActors.Reserve(ExclusivePairs.Num() * 2);
		for (const FValidatedPair& Pr : ExclusivePairs)
		{
			if (MeshComps.IsValidIndex(Pr.A))
			{
				if (UStaticMeshComponent* M0 = MeshComps[Pr.A].Get())
				{
					if (AActor* A0 = M0->GetOwner())
					{
						VisibleActors.Add(A0);
					}
				}
			}
			if (MeshComps.IsValidIndex(Pr.B))
			{
				if (UStaticMeshComponent* M1 = MeshComps[Pr.B].Get())
				{
					if (AActor* A1 = M1->GetOwner())
					{
						VisibleActors.Add(A1);
					}
				}
			}
		}

		for (const TWeakObjectPtr<UStaticMeshComponent>& WeakComp : MeshComps)
		{
			if (UStaticMeshComponent* Comp = WeakComp.Get())
			{
				if (AActor* Owner = Comp->GetOwner())
				{
					ScannedActors.Add(Owner);
				}
			}
		}

		for (AActor* Actor : ScannedActors)
		{
			if (!Actor || !IsValid(Actor))
			{
				continue;
			}
			const bool bShowInEditor = VisibleActors.Contains(Actor);
			Actor->Modify();
			Actor->SetIsTemporarilyHiddenInEditor(!bShowInEditor);
		}

		if (GEditor && VisibleActors.Num() > 0)
		{
			GEditor->SelectNone(true, true, false);
			for (AActor* Actor : VisibleActors)
			{
				if (Actor && IsValid(Actor))
				{
					GEditor->SelectActor(Actor, true, false, true);
				}
			}
			GEditor->NoteSelectionChange();
		}
	}

	int32 ComputeMeshPairMatchScore(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
	{
		const UStaticMesh* Asset0 = M0 ? M0->GetStaticMesh() : nullptr;
		const UStaticMesh* Asset1 = M1 ? M1->GetStaticMesh() : nullptr;
		if (!Asset0 || !Asset1)
		{
			return 0;
		}
		if (Asset0 == Asset1)
		{
			return 3;
		}
		if (StaticMeshDerivedDataKeysMatch(
			TryGetStaticMeshDerivedDataKey(Asset0),
			TryGetStaticMeshDerivedDataKey(Asset1)))
		{
			return 2;
		}
		if (StaticMeshesShareEquivalentShape(Asset0, Asset1))
		{
			return 1;
		}
		return 0;
	}

	bool TryValidateMeshPairForLift(
		UStaticMeshComponent* M0,
		UStaticMeshComponent* M1,
		FBox& OutB0,
		FBox& OutB1)
	{
		if (!M0 || !M1 || M0 == M1)
		{
			return false;
		}
		AActor* A0 = M0->GetOwner();
		AActor* A1 = M1->GetOwner();
		if (!A0 || !A1 || A0 == A1)
		{
			return false;
		}
		if (MeshComponentsShareAttachmentHierarchy(M0, M1))
		{
			return false;
		}
		OutB0 = GetWorldBoxForMeshComponent(M0);
		OutB1 = GetWorldBoxForMeshComponent(M1);
		if (!OutB0.IsValid || !OutB1.IsValid)
		{
			return false;
		}
		if (!MeshComponentsPassGeometryOverlap(M0, M1))
		{
			return false;
		}
		return MeshComponentsPassCoincidence(M0, M1, OutB0, OutB1);
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

			if (!WorldAabbsAreCoincidentDuplicates(P0.Bounds, P1.Bounds))
			{
				return;
			}
			if (PodPairBlockedAsCoLocatedHostLayers(P0, P1))
			{
				return;
			}
			if (!PodMeshesShareEquivalentShape(P0, P1))
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

						TArray<FValidatedPair> ValidatedPairs;
						ValidatedPairs.Reserve(Cands.Num());
						for (const FIndexPair& Pr : Cands)
						{
							Progress.EnterProgressFrame(1.f);
							if (!SharedComps->IsValidIndex(Pr.A) || !SharedComps->IsValidIndex(Pr.B))
							{
								continue;
							}
							UStaticMeshComponent* M0 = (*SharedComps)[Pr.A].Get();
							UStaticMeshComponent* M1 = (*SharedComps)[Pr.B].Get();
							FBox B0;
							FBox B1;
							if (!TryValidateMeshPairForLift(M0, M1, B0, B1))
							{
								continue;
							}
							FValidatedPair Validated;
							Validated.A = Pr.A;
							Validated.B = Pr.B;
							Validated.MatchScore = ComputeMeshPairMatchScore(M0, M1);
							ValidatedPairs.Add(Validated);
						}

						ValidatedPairs.Sort([](const FValidatedPair& L, const FValidatedPair& R)
						{
							if (L.MatchScore != R.MatchScore)
							{
								return L.MatchScore > R.MatchScore;
							}
							if (L.A != R.A)
							{
								return L.A < R.A;
							}
							return L.B < R.B;
						});

						TSet<int32> UsedMeshSlots;
						TArray<FValidatedPair> ExclusivePairs;
						ExclusivePairs.Reserve(ValidatedPairs.Num());
						for (const FValidatedPair& Pr : ValidatedPairs)
						{
							if (UsedMeshSlots.Contains(Pr.A) || UsedMeshSlots.Contains(Pr.B))
							{
								continue;
							}
							UsedMeshSlots.Add(Pr.A);
							UsedMeshSlots.Add(Pr.B);
							ExclusivePairs.Add(Pr);
						}

						int32 Applied = 0;
						{
							const FScopedTransaction Transaction(LOCTEXT("BroadBCLiftTx", "B+C Z lift for mesh pairs"));
							for (const FValidatedPair& Pr : ExclusivePairs)
							{
								UStaticMeshComponent* M0 = (*SharedComps)[Pr.A].Get();
								UStaticMeshComponent* M1 = (*SharedComps)[Pr.B].Get();
								if (!M0 || !M1)
								{
									continue;
								}
								AActor* A0 = M0->GetOwner();
								AActor* A1 = M1->GetOwner();
								const FBox B0 = GetWorldBoxForMeshComponent(M0);
								const FBox B1 = GetWorldBoxForMeshComponent(M1);
								AActor* const ToMove = (B0.Min.Z < B1.Min.Z) ? A0 : A1;
								ApplyZLiftToActor(ToMove, DeltaZWorld);
								++Applied;
							}

							if (ExclusivePairs.Num() > 0)
							{
								ApplyPairSelectionAndHideUnpairedMeshes(*SharedComps, ExclusivePairs);
							}
						}

						FMessageDialog::Open(
							EAppMsgType::Ok,
							FText::Format(
								LOCTEXT(
									"BroadBCLiftDoneFmt",
									"Z offset per lift: {0} (world).  Broad candidates: {1}.  B+C valid pairs: {2}.  Exclusive 1:1 lifts applied: {3}.\n\n"
									"Shape match: same StaticMesh, same DerivedDataKey, or identical LOD0 vertex/triangle counts with strong geometry hash (quantized verts + canonical faces). Each part pairs with at most one partner. Both actors in each pair are selected; unpaired actors in the scan set use Outliner editor visibility (eye icon)."),
								FText::AsNumber(DeltaZWorld),
								FText::AsNumber(Cands.Num()),
								FText::AsNumber(ValidatedPairs.Num()),
								FText::AsNumber(Applied)));
					});
			});
	}

	void LiftLowerMeshIfTwoOverlapImpl(const float DeltaZWorld)
	{
		TArray<AActor*> SelectedActorList;
		EditModelToolSelectionUtils::GatherSelectedActors(SelectedActorList);
		if (SelectedActorList.Num() == 0)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoSelectionForLiftZ", "No selection."));
			return;
		}

		TArray<TWeakObjectPtr<UStaticMeshComponent>> MeshComps;
		TArray<FMeshPOD> PODs;
		for (AActor* Actor : SelectedActorList)
		{
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
				FillMeshPODFields(Pod, Sm, Asset);
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
				FillMeshPODFields(Pod, Sm, Asset);
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
