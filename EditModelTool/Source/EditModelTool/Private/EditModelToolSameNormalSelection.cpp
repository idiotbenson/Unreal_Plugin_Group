#include "EditModelToolModule.h"
#include "EditModelToolSession.h"

#include "Core/EditModelToolSelectionUtils.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdModeInteractiveToolsContext.h"
#include "EditorModeManager.h"
#include "Engine/StaticMesh.h"
#include "InteractiveToolManager.h"
#include "GameFramework/Actor.h"
#include "MeshDescription.h"
#include "MeshOpPreviewHelpers.h"
#include "Misc/MessageDialog.h"
#include "ModelingToolsEditorMode.h"
#include "PreviewMesh.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selection/MeshTopologySelector.h"
#include "Selection/PolygonSelectionMechanic.h"
#include "SelectionSet.h"
#include "Selections/GeometrySelection.h"
#include "Tools/UEdMode.h"
#include "UDynamicMesh.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "FEditModelToolModule"

namespace
{
	bool TryGetTriangleWorldNormal(const UStaticMeshComponent* MeshComponent, const int32 TriangleIDValue, FVector& OutWorldNormal)
	{
		if (!MeshComponent)
		{
			return false;
		}
		UStaticMesh* StaticMesh = MeshComponent->GetStaticMesh();
		if (!StaticMesh)
		{
			return false;
		}

		const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			return false;
		}

		const FTriangleID TriangleID(TriangleIDValue);
		if (!MeshDescription->Triangles().IsValid(TriangleID))
		{
			return false;
		}

		const TArrayView<const FVertexInstanceID> VertexInstances = MeshDescription->GetTriangleVertexInstances(TriangleID);
		if (VertexInstances.Num() < 3)
		{
			return false;
		}

		const TVertexAttributesConstRef<FVector3f> VertexPositions = MeshDescription->GetVertexPositions();
		const FVector P0 = (FVector)VertexPositions[MeshDescription->GetVertexInstanceVertex(VertexInstances[0])];
		const FVector P1 = (FVector)VertexPositions[MeshDescription->GetVertexInstanceVertex(VertexInstances[1])];
		const FVector P2 = (FVector)VertexPositions[MeshDescription->GetVertexInstanceVertex(VertexInstances[2])];

		FVector LocalNormal = FVector::CrossProduct(P1 - P0, P2 - P0);
		if (!LocalNormal.Normalize())
		{
			return false;
		}

		OutWorldNormal = MeshComponent->GetComponentTransform().TransformVectorNoScale(LocalNormal);
		return OutWorldNormal.Normalize();
	}

	bool TryGetTriangleWorldNormal(UDynamicMeshComponent* MeshComponent, const int32 TriangleIDValue, FVector& OutWorldNormal)
	{
		if (!MeshComponent)
		{
			return false;
		}
		UDynamicMesh* DynamicMesh = MeshComponent->GetDynamicMesh();
		if (!DynamicMesh)
		{
			return false;
		}

		bool bOk = false;
		DynamicMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			if (!Mesh.IsTriangle(TriangleIDValue))
			{
				return;
			}
			UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriangleIDValue);
			const FVector P0 = (FVector)Mesh.GetVertex(Tri.A);
			const FVector P1 = (FVector)Mesh.GetVertex(Tri.B);
			const FVector P2 = (FVector)Mesh.GetVertex(Tri.C);

			FVector LocalNormal = FVector::CrossProduct(P1 - P0, P2 - P0);
			if (!LocalNormal.Normalize())
			{
				return;
			}

			OutWorldNormal = MeshComponent->GetComponentTransform().TransformVectorNoScale(LocalNormal);
			bOk = OutWorldNormal.Normalize();
		});

		return bOk;
	}

	uint64 EncodeSelectionIDForStaticMesh(
		const FMeshDescription* MeshDescription,
		const FTriangleID TriangleID,
		const UE::Geometry::EGeometryTopologyType TopologyType)
	{
		if (!MeshDescription)
		{
			return UE::Geometry::FGeoSelectionID::MeshTriangle(TriangleID.GetValue()).Encoded();
		}

		if (TopologyType == UE::Geometry::EGeometryTopologyType::Polygroup)
		{
			const FPolygonGroupID PolygroupID = MeshDescription->GetTrianglePolygonGroup(TriangleID);
			return UE::Geometry::FGeoSelectionID::GroupFace(TriangleID.GetValue(), PolygroupID.GetValue()).Encoded();
		}

		return UE::Geometry::FGeoSelectionID::MeshTriangle(TriangleID.GetValue()).Encoded();
	}

	uint64 EncodeSelectionIDForDynamicMesh(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const int32 TriangleID,
		const UE::Geometry::EGeometryTopologyType TopologyType)
	{
		if (TopologyType == UE::Geometry::EGeometryTopologyType::Polygroup)
		{
			return UE::Geometry::FGeoSelectionID::GroupFace(TriangleID, Mesh.GetTriangleGroup(TriangleID)).Encoded();
		}
		return UE::Geometry::FGeoSelectionID::MeshTriangle(TriangleID).Encoded();
	}

	UInteractiveTool* ResolveActiveModelingInteractiveTool(UEdMode* ModelingEdMode, FString& OutDiag)
	{
		OutDiag += TEXT("Active tool lookup:\n");
		auto TryFetchActiveTool = [&](UInteractiveToolManager* ToolManager, const TCHAR* SourceLabel) -> UInteractiveTool*
		{
			if (!ToolManager)
			{
				return nullptr;
			}

			static const EToolSide Sides[] = {EToolSide::Mouse, EToolSide::Right};
			for (const EToolSide Side : Sides)
			{
				if (!ToolManager->HasActiveTool(Side))
				{
					continue;
				}

				if (UInteractiveTool* Candidate = ToolManager->GetActiveTool(Side))
				{
					OutDiag += FString::Printf(
						TEXT("- Resolved from %s (side=%d): %s\n"),
						SourceLabel,
						static_cast<int32>(Side),
						*Candidate->GetClass()->GetName());
					return Candidate;
				}
			}

			return nullptr;
		};

		UInteractiveTool* ActiveTool = nullptr;

		if (ModelingEdMode)
		{
			ActiveTool =
				TryFetchActiveTool(ModelingEdMode->GetToolManager(EToolsContextScope::EdMode), TEXT("EdMode scope"));
			if (!ActiveTool)
			{
				ActiveTool =
					TryFetchActiveTool(ModelingEdMode->GetToolManager(EToolsContextScope::Default), TEXT("Default scope"));
			}
			if (!ActiveTool)
			{
				ActiveTool = TryFetchActiveTool(
					ModelingEdMode->GetToolManager(EToolsContextScope::Editor),
					TEXT("Editor-wide scope"));
			}
		}

		if (!ActiveTool)
		{
			UModeManagerInteractiveToolsContext* ModeManagerITC = GLevelEditorModeTools().GetInteractiveToolsContext();
			ActiveTool = TryFetchActiveTool(
				ModeManagerITC ? ModeManagerITC->ToolManager : nullptr,
				TEXT("ModeManager (fallback)"));
		}

		if (!ActiveTool)
		{
			OutDiag += TEXT("- No active tool (Mouse/Right) on Modeling or ModeManager ToolManagers.\n");
			return nullptr;
		}

		return ActiveTool;
	}

	bool TriangleWorldUnitNormalForPreviewXform(
		const FTransform& PreviewXform,
		const UE::Geometry::FDynamicMesh3& Mesh,
		const int32 Tid,
		FVector& OutUnitNormal)
	{
		if (!Mesh.IsTriangle(Tid))
		{
			return false;
		}

		const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(Tid);
		const FVector P0 = (FVector)Mesh.GetVertex(Tri.A);
		const FVector P1 = (FVector)Mesh.GetVertex(Tri.B);
		const FVector P2 = (FVector)Mesh.GetVertex(Tri.C);
		FVector LocalN = FVector::CrossProduct(P1 - P0, P2 - P0);
		if (!LocalN.Normalize())
		{
			return false;
		}

		OutUnitNormal = PreviewXform.TransformVectorNoScale(LocalN);
		return OutUnitNormal.Normalize();
	}

	void CollectReferencedUObjectChildren(UObject* Root, TArray<UObject*>& OutReachable, const int32 MaxHops)
	{
		OutReachable.Reset();
		if (!Root || MaxHops <= 0)
		{
			return;
		}

		TSet<UObject*> Seen;
		TArray<UObject*> Frontier;
		Frontier.Add(Root);
		Seen.Add(Root);
		OutReachable.Add(Root);

		for (int32 Hop = 0; Hop < MaxHops && Frontier.Num() > 0; ++Hop)
		{
			TArray<UObject*> NextFrontier;

			for (UObject* Obj : Frontier)
			{
				for (TFieldIterator<FProperty> PropIt(Obj->GetClass(), EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
				{
					const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*PropIt);
					if (!ObjProp)
					{
						continue;
					}

					UObject* SubObj = ObjProp->GetObjectPropertyValue_InContainer(Obj);
					if (!SubObj || SubObj == Obj)
					{
						continue;
					}

					if (!Seen.Contains(SubObj))
					{
						Seen.Add(SubObj);
						OutReachable.Add(SubObj);
						NextFrontier.Add(SubObj);
					}
				}
			}

			Frontier = MoveTemp(NextFrontier);
			if (OutReachable.Num() > 140)
			{
				break;
			}
		}
	}

	bool TryExpandSameNormalFacesViaActiveMeshSelectionBrushTool(
		UInteractiveTool* ActiveTool,
		const float DotThreshold,
		FString& OutLog,
		int32& OutExpandedFaceCount)
	{
		OutExpandedFaceCount = 0;
		OutLog += TEXT("Mesh brush tool path:\n");

		if (!ActiveTool)
		{
			OutLog += TEXT("- Skipped (no active tool).\n");
			return false;
		}

		const FName SelName(TEXT("Selection"));
		const FName PrevName(TEXT("PreviewMesh"));
		FObjectProperty* SelProp = nullptr;
		FObjectProperty* PrevProp = nullptr;

		for (UClass* Cls = ActiveTool->GetClass(); Cls; Cls = Cls->GetSuperClass())
		{
			if (!SelProp)
			{
				if (FProperty* P = Cls->FindPropertyByName(SelName))
				{
					SelProp = CastField<FObjectProperty>(P);
				}
			}
			if (!PrevProp)
			{
				if (FProperty* P = Cls->FindPropertyByName(PrevName))
				{
					PrevProp = CastField<FObjectProperty>(P);
				}
			}
			if (SelProp && PrevProp)
			{
				break;
			}
		}

		if (!SelProp || !PrevProp)
		{
			OutLog += TEXT("- Missing Selection or PreviewMesh property (not a mesh brush / selection tool).\n");
			return false;
		}

		UMeshSelectionSet* MeshSelSet = Cast<UMeshSelectionSet>(SelProp->GetObjectPropertyValue_InContainer(ActiveTool));
		UPreviewMesh* PreviewMesh = Cast<UPreviewMesh>(PrevProp->GetObjectPropertyValue_InContainer(ActiveTool));
		if (!MeshSelSet || !PreviewMesh)
		{
			OutLog += TEXT("- Selection / PreviewMesh did not cast to expected types.\n");
			return false;
		}

		const TArray<int>& FaceElements = MeshSelSet->GetElements(EMeshSelectionElementType::Face);
		TArray<int32> OldFacesCopy;
		OldFacesCopy.Reserve(FaceElements.Num());
		for (const int FaceIdx : FaceElements)
		{
			OldFacesCopy.Add(static_cast<int32>(FaceIdx));
		}

		if (OldFacesCopy.Num() == 0)
		{
			OutLog += TEXT("- Tool face selection is empty.\n");
			return false;
		}

		const FTransform PreviewXform = PreviewMesh->GetTransform();

		FVector SeedNormal = FVector::ZeroVector;
		int32 SeedCount = 0;
		PreviewMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (const int32 Tid : OldFacesCopy)
			{
				FVector N;
				if (TriangleWorldUnitNormalForPreviewXform(PreviewXform, Mesh, Tid, N))
				{
					SeedNormal += N;
					++SeedCount;
				}
			}
		});

		if (SeedCount == 0 || !SeedNormal.Normalize())
		{
			OutLog += TEXT("- Could not compute seed normal from selected faces.\n");
			return false;
		}

		TSet<int32> NewFaceIndices;
		PreviewMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (int32 Tid : Mesh.TriangleIndicesItr())
			{
				FVector FN;
				if (!TriangleWorldUnitNormalForPreviewXform(PreviewXform, Mesh, Tid, FN))
				{
					continue;
				}

				if (FVector::DotProduct(SeedNormal, FN) >= DotThreshold)
				{
					NewFaceIndices.Add(Tid);
				}
			}
		});

		if (NewFaceIndices.Num() == 0)
		{
			OutLog += TEXT("- Expanded set is empty.\n");
			return false;
		}

		MeshSelSet->RemoveIndices(EMeshSelectionElementType::Face, OldFacesCopy);
		const TArray<int32> NewFacesArr = NewFaceIndices.Array();
		MeshSelSet->AddIndices(EMeshSelectionElementType::Face, NewFacesArr);
		MeshSelSet->NotifySelectionSetModified();

		OutExpandedFaceCount = NewFacesArr.Num();
		OutLog += FString::Printf(
			TEXT("- OK. Seed tris considered=%d, expanded face count=%d.\n"),
			SeedCount,
			OutExpandedFaceCount);
		return true;
	}

	bool TryExpandSameNormalFacesViaPolygonSelectionMechanic(
		UInteractiveTool* ActiveTool,
		const float DotThreshold,
		FString& OutLog,
		int32& OutExpandedFaceCount)
	{
		OutExpandedFaceCount = 0;
		OutLog += TEXT("Polygon / tri-edit topology path:\n");

		using FDynamicMesh3 = UE::Geometry::FDynamicMesh3;
		using FGroupTopologySelection = UE::Geometry::FGroupTopologySelection;

		if (!ActiveTool)
		{
			OutLog += TEXT("- Skipped (no active tool).\n");
			return false;
		}

		TArray<UObject*> Reachable;
		CollectReferencedUObjectChildren(ActiveTool, Reachable, 5);

		UPolygonSelectionMechanic* PolyMech = nullptr;
		UPreviewMesh* PreviewMesh = nullptr;

		for (UObject* Obj : Reachable)
		{
			if (!PolyMech && Obj->IsA(UPolygonSelectionMechanic::StaticClass()))
			{
				PolyMech = Cast<UPolygonSelectionMechanic>(Obj);
			}
			if (!PreviewMesh && Obj->IsA(UPreviewMesh::StaticClass()))
			{
				PreviewMesh = Cast<UPreviewMesh>(Obj);
			}

			UMeshOpPreviewWithBackgroundCompute* MeshOpPrev = Cast<UMeshOpPreviewWithBackgroundCompute>(Obj);
			if (!PreviewMesh && MeshOpPrev && MeshOpPrev->PreviewMesh)
			{
				PreviewMesh = MeshOpPrev->PreviewMesh;
			}
		}

		if (!PolyMech)
		{
			OutLog += TEXT("- No UPolygonSelectionMechanic on tool subgraph.\n");
			return false;
		}

		OutLog += FString::Printf(TEXT("- Polygon mechanic: %s\n"), *PolyMech->GetName());

		if (!PreviewMesh)
		{
			OutLog += TEXT("- No UPreviewMesh (direct or MeshOpPreview) — cannot evaluate normals.\n");
			return false;
		}

		const FGroupTopologySelection& SeedSel = PolyMech->GetActiveSelection();
		if (SeedSel.SelectedGroupIDs.Num() == 0)
		{
			OutLog += TEXT("- No polygon/triangle faces selected (need face selection, not edge/vertex-only).\n");
			return false;
		}

		const TSharedPtr<FMeshTopologySelector, ESPMode::ThreadSafe> TopologySelector = PolyMech->GetTopologySelector();
		if (!TopologySelector.IsValid())
		{
			OutLog += TEXT("- Mesh topology selector is not initialized.\n");
			return false;
		}

		const FTopologyProvider* TopologyProvider = TopologySelector->GetTopologyProvider();
		if (!TopologyProvider)
		{
			OutLog += TEXT("- Mesh topology provider is missing.\n");
			return false;
		}

		const FTransform PreviewXform = PreviewMesh->GetTransform();

		FVector SeedNormal = FVector::ZeroVector;
		int32 SeedsUsed = 0;

		TSet<int32> GroupsPending(SeedSel.SelectedGroupIDs);

		PreviewMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
		{
			if (GroupsPending.Num() == 0)
			{
				return;
			}

			for (int32 Tid : Mesh.TriangleIndicesItr())
			{
				if (GroupsPending.Num() == 0)
				{
					break;
				}

				if (!Mesh.IsTriangle(Tid))
				{
					continue;
				}

				const int32 TopologyGroupId = TopologyProvider->GetGroupIDForTriangle(Tid);
				if (!GroupsPending.Contains(TopologyGroupId))
				{
					continue;
				}

				FVector N;
				if (!TriangleWorldUnitNormalForPreviewXform(PreviewXform, Mesh, Tid, N))
				{
					continue;
				}

				SeedNormal += N;
				++SeedsUsed;
				GroupsPending.Remove(TopologyGroupId);
			}
		});

		if (SeedsUsed == 0 || !SeedNormal.Normalize())
		{
			OutLog += TEXT("- Could not build seed normals from selected groups/mesh correspondence.\n");
			return false;
		}

		TSet<int32> NewGroups;
		int32 MatchTriCount = 0;

		PreviewMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
		{
			for (int32 Tid : Mesh.TriangleIndicesItr())
			{
				FVector Fn;
				if (!TriangleWorldUnitNormalForPreviewXform(PreviewXform, Mesh, Tid, Fn))
				{
					continue;
				}

				if (FVector::DotProduct(SeedNormal, Fn) < DotThreshold)
				{
					continue;
				}

				++MatchTriCount;
				NewGroups.Add(TopologyProvider->GetGroupIDForTriangle(Tid));
			}
		});

		if (NewGroups.Num() == 0)
		{
			OutLog += TEXT("- Expanded topology groups empty.\n");
			return false;
		}

		FGroupTopologySelection NewSel;
		NewSel.SelectedGroupIDs = MoveTemp(NewGroups);
		PolyMech->SetSelection(NewSel, true);

		OutExpandedFaceCount = MatchTriCount;

		OutLog += FString::Printf(
			TEXT("- OK. Seed faces (groups) used=%d, matching triangles=%d, output groups=%d.\n"),
			SeedsUsed,
			MatchTriCount,
			NewSel.SelectedGroupIDs.Num());
		return true;
	}
}

void FEditModelToolModule::SelectSameNormalFacesFromModelingSelection()
{
	static const FEditorModeID ModelingModeID = TEXT("EM_ModelingToolsEditorMode");
	UEdMode* ActiveMode = GLevelEditorModeTools().GetActiveScriptableMode(ModelingModeID);
	if (!ActiveMode)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SameNormalNeedModelingMode", "Please switch to Modeling Mode first."));
		return;
	}

	const float AngleToleranceDeg =
		FMath::Clamp(EditModelTool::SessionSameNormalAngleToleranceDegrees(), 0.01f, 90.f);
	const float DotThreshold = FMath::Cos(FMath::DegreesToRadians(AngleToleranceDeg));

	FString ToolResolveLog;
	UInteractiveTool* ActiveModelingTool = ResolveActiveModelingInteractiveTool(ActiveMode, ToolResolveLog);

	FString MeshToolPathLog = ToolResolveLog;
	int32 MeshToolExpandedCount = 0;

	if (ActiveModelingTool != nullptr &&
		TryExpandSameNormalFacesViaActiveMeshSelectionBrushTool(
			ActiveModelingTool,
			DotThreshold,
			MeshToolPathLog,
			MeshToolExpandedCount))
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(FString::Printf(
				TEXT(
					"Same-normal expansion (Edit Materials / mesh selection tool).\nExpanded face count: %d\nAngle tolerance: %.2f deg.\n\n%s"),
				MeshToolExpandedCount,
				AngleToleranceDeg,
				*MeshToolPathLog)));
		return;
	}

	MeshToolPathLog += TEXT("\n");
	if (ActiveModelingTool != nullptr &&
		TryExpandSameNormalFacesViaPolygonSelectionMechanic(
			ActiveModelingTool,
			DotThreshold,
			MeshToolPathLog,
			MeshToolExpandedCount))
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(FString::Printf(
				TEXT(
					"Same-normal expansion (polygon / tri modeling tool).\nMatching triangles: %d\nAngle tolerance: %.2f deg.\n\n%s"),
				MeshToolExpandedCount,
				AngleToleranceDeg,
				*MeshToolPathLog)));
		return;
	}

	UModelingToolsEditorMode* ModelingMode = (UModelingToolsEditorMode*)ActiveMode;
	UGeometrySelectionManager* SelectionManager = ModelingMode ? ModelingMode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(FString::Printf(
				TEXT("Modeling geometry selection manager is unavailable.\n\n%s"),
				*MeshToolPathLog)));
		return;
	}

	using FGeometrySelection = UE::Geometry::FGeometrySelection;
	using EGeometryElementType = UE::Geometry::EGeometryElementType;
	using EGeometryTopologyType = UE::Geometry::EGeometryTopologyType;

	EGeometryTopologyType TopologyType;
	EGeometryElementType ElementType;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);

	if (bIsEmpty || NumTargets <= 0 || ElementType != EGeometryElementType::Face ||
		(TopologyType != EGeometryTopologyType::Triangle && TopologyType != EGeometryTopologyType::Polygroup))
	{
	}

	TArray<AActor*> SelectedActors;
	EditModelToolSelectionUtils::GatherSelectedActors(SelectedActors);
	if (SelectedActors.Num() == 0)
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(FString::Printf(
				TEXT(
					"For geometry-based expansion, select the target actor in Outliner.\n"
					"(Edit Materials uses the mesh-selection path above; Tri/Poly tools use the topology path — see log.)\n\n%s"),
				*MeshToolPathLog)));
		return;
	}

	int32 UpdatedComponents = 0;
	int32 TotalSelectedFaces = 0;
	int32 CheckedComponents = 0;
	int32 ComponentsWithSelection = 0;
	int32 ComponentsWithSeedNormal = 0;
	FString DebugLog;
	DebugLog += TEXT("Same-normal debug:\n");

	for (AActor* Actor : SelectedActors)
	{
		if (!Actor)
		{
			continue;
		}
		DebugLog += FString::Printf(TEXT("- Actor: %s\n"), *Actor->GetActorLabel());

		TArray<UPrimitiveComponent*> PrimitiveComponents;
		Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
		for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
		{
			if (!PrimitiveComponent)
			{
				continue;
			}
			++CheckedComponents;
			DebugLog += FString::Printf(TEXT("  * Component: %s\n"), *PrimitiveComponent->GetName());

			FGeometrySelection CurrentSelection;
			if (!SelectionManager->GetSelectionForComponent(PrimitiveComponent, CurrentSelection))
			{
				DebugLog += TEXT("    - GetSelectionForComponent: false\n");
				continue;
			}
			++ComponentsWithSelection;
			DebugLog += FString::Printf(
				TEXT("    - Selection: num=%d, element=%d, topology=%d\n"),
				CurrentSelection.Num(),
				static_cast<int32>(CurrentSelection.ElementType),
				static_cast<int32>(CurrentSelection.TopologyType));
			if (CurrentSelection.IsEmpty() || CurrentSelection.ElementType != EGeometryElementType::Face ||
				(CurrentSelection.TopologyType != EGeometryTopologyType::Triangle && CurrentSelection.TopologyType != EGeometryTopologyType::Polygroup))
			{
				DebugLog += TEXT("    - Selection rejected by type/topology/empty check\n");
				continue;
			}

			FVector SeedNormal = FVector::ZeroVector;
			UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(PrimitiveComponent);
			UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(PrimitiveComponent);
			if (StaticMeshComponent)
			{
				if (!StaticMeshComponent->GetStaticMesh())
				{
					DebugLog += TEXT("    - StaticMesh missing\n");
					continue;
				}

				int32 SeedCount = 0;
				for (const uint64 EncodedID : CurrentSelection.Selection)
				{
					const int32 SeedTriangleID = static_cast<int32>(UE::Geometry::FGeoSelectionID(EncodedID).GeometryID);
					FVector ThisSeedNormal;
					if (TryGetTriangleWorldNormal(StaticMeshComponent, SeedTriangleID, ThisSeedNormal))
					{
						SeedNormal += ThisSeedNormal;
						++SeedCount;
					}
				}
				if (SeedCount == 0 || !SeedNormal.Normalize())
				{
					DebugLog += TEXT("    - StaticMesh seed normal failed\n");
					continue;
				}
				++ComponentsWithSeedNormal;

				const FMeshDescription* MeshDescription = StaticMeshComponent->GetStaticMesh()->GetMeshDescription(0);
				if (!MeshDescription)
				{
					DebugLog += TEXT("    - MeshDescription missing\n");
					continue;
				}

				FGeometrySelection NewSelection;
				NewSelection.InitializeTypes(CurrentSelection);
				for (const FTriangleID TriangleID : MeshDescription->Triangles().GetElementIDs())
				{
					FVector FaceNormal;
					if (!TryGetTriangleWorldNormal(StaticMeshComponent, TriangleID.GetValue(), FaceNormal))
					{
						continue;
					}
					if (FVector::DotProduct(SeedNormal, FaceNormal) >= DotThreshold)
					{
						NewSelection.Selection.Add(
							EncodeSelectionIDForStaticMesh(MeshDescription, TriangleID, CurrentSelection.TopologyType));
					}
				}

				SelectionManager->SetSelectionForComponent(PrimitiveComponent, NewSelection);
				++UpdatedComponents;
				TotalSelectedFaces += NewSelection.Num();
				DebugLog += FString::Printf(TEXT("    - Updated static component, new faces=%d\n"), NewSelection.Num());
			}
			else if (DynamicMeshComponent)
			{
				int32 SeedCount = 0;
				for (const uint64 EncodedID : CurrentSelection.Selection)
				{
					const int32 SeedTriangleID = static_cast<int32>(UE::Geometry::FGeoSelectionID(EncodedID).GeometryID);
					FVector ThisSeedNormal;
					if (TryGetTriangleWorldNormal(DynamicMeshComponent, SeedTriangleID, ThisSeedNormal))
					{
						SeedNormal += ThisSeedNormal;
						++SeedCount;
					}
				}
				if (SeedCount == 0 || !SeedNormal.Normalize())
				{
					DebugLog += TEXT("    - DynamicMesh seed normal failed\n");
					continue;
				}
				++ComponentsWithSeedNormal;

				FGeometrySelection NewSelection;
				NewSelection.InitializeTypes(CurrentSelection);
				UDynamicMesh* DynamicMesh = DynamicMeshComponent->GetDynamicMesh();
				if (!DynamicMesh)
				{
					continue;
				}

				DynamicMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
				{
					for (int32 TriangleID : Mesh.TriangleIndicesItr())
					{
						if (!Mesh.IsTriangle(TriangleID))
						{
							continue;
						}

						const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriangleID);
						const FVector P0 = (FVector)Mesh.GetVertex(Tri.A);
						const FVector P1 = (FVector)Mesh.GetVertex(Tri.B);
						const FVector P2 = (FVector)Mesh.GetVertex(Tri.C);
						FVector LocalNormal = FVector::CrossProduct(P1 - P0, P2 - P0);
						if (!LocalNormal.Normalize())
						{
							continue;
						}
						FVector FaceNormal = DynamicMeshComponent->GetComponentTransform().TransformVectorNoScale(LocalNormal);
						if (!FaceNormal.Normalize())
						{
							continue;
						}

						if (FVector::DotProduct(SeedNormal, FaceNormal) >= DotThreshold)
						{
							NewSelection.Selection.Add(
								EncodeSelectionIDForDynamicMesh(Mesh, TriangleID, CurrentSelection.TopologyType));
						}
					}
				});

				SelectionManager->SetSelectionForComponent(PrimitiveComponent, NewSelection);
				++UpdatedComponents;
				TotalSelectedFaces += NewSelection.Num();
				DebugLog += FString::Printf(TEXT("    - Updated dynamic component, new faces=%d\n"), NewSelection.Num());
			}
			else
			{
				DebugLog += TEXT("    - Unsupported component type\n");
			}
		}
	}

	if (UpdatedComponents == 0)
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(FString::Printf(
				TEXT(
					"No component updated.\nChecked components: %d\nComponents with selection: %d\nComponents with seed normal: %d\n\nMesh tool attempt:\n%s\n\nGeometry manager path:\n%s"),
				CheckedComponents,
				ComponentsWithSelection,
				ComponentsWithSeedNormal,
				*MeshToolPathLog,
				*DebugLog)));
		return;
	}

	FMessageDialog::Open(
		EAppMsgType::Ok,
		FText::Format(
			LOCTEXT("SameNormalDoneFmt", "Same-normal face expansion complete.\nUpdated components: {0}\nTotal selected faces: {1}\nAngle tolerance: {2} deg."),
			FText::AsNumber(UpdatedComponents),
			FText::AsNumber(TotalSelectedFaces),
			FText::AsNumber(AngleToleranceDeg)));
}

#undef LOCTEXT_NAMESPACE
