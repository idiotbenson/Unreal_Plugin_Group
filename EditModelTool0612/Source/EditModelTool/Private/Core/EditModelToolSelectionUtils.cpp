#include "EditModelToolPrivatePCH.h"

#include "Core/EditModelToolSelectionUtils.h"

#include "Core/EditModelToolFilterPolicy.h"
#include "Components/ActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Elements/Actor/ActorElementData.h"
#include "Elements/Component/ComponentElementData.h"
#include "Elements/Framework/TypedElementRegistry.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Elements/Interfaces/TypedElementObjectInterface.h"
#include "Elements/SMInstance/SMInstanceElementData.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "LevelEditorSubsystem.h"
#include "Modules/ModuleManager.h"

namespace EditModelToolSelectionUtils
{
namespace
{
	static AActor* ResolveObjectToActor(UObject* SelectedObject)
	{
		if (AActor* Actor = Cast<AActor>(SelectedObject))
		{
			return Actor;
		}

		if (const UActorComponent* Component = Cast<UActorComponent>(SelectedObject))
		{
			return Component->GetOwner();
		}

		return nullptr;
	}

	static AActor* ResolveActorFromElementHandle(const FTypedElementHandle& Handle)
	{
		if (!Handle)
		{
			return nullptr;
		}

		if (AActor* Actor = ActorElementDataUtil::GetActorFromHandle(Handle, /*bSilent*/ true))
		{
			return Actor;
		}

		if (UActorComponent* Component = ComponentElementDataUtil::GetComponentFromHandle(Handle, /*bSilent*/ true))
		{
			return Component->GetOwner();
		}

		if (const FSMInstanceManager SMInstance = SMInstanceElementDataUtil::GetSMInstanceFromHandle(Handle, /*bSilent*/ true))
		{
			if (UInstancedStaticMeshComponent* ISMComponent = SMInstance.GetISMComponent())
			{
				return ISMComponent->GetOwner();
			}
		}

		if (const UTypedElementRegistry* Registry = UTypedElementRegistry::GetInstance())
		{
			if (TTypedElement<ITypedElementObjectInterface> ObjectElement =
					Registry->GetElement<ITypedElementObjectInterface>(Handle))
			{
				return ResolveObjectToActor(ObjectElement.GetObject());
			}
		}

		return nullptr;
	}

	static void TryAddActor(
		AActor* Actor,
		const bool bRequireEditorVisible,
		TSet<AActor*>& SeenActors,
		TArray<AActor*>& OutActors)
	{
		if (!Actor || SeenActors.Contains(Actor))
		{
			return;
		}

		if (bRequireEditorVisible && !EditModelToolFilterPolicy::ActorIsEditorVisibleForSelection(Actor))
		{
			return;
		}

		SeenActors.Add(Actor);
		OutActors.Add(Actor);
	}

	static void GatherFromTypedElementSelectionSet(
		UTypedElementSelectionSet* SelectionSet,
		const bool bRequireEditorVisible,
		TSet<AActor*>& SeenActors,
		TArray<AActor*>& OutActors)
	{
		if (!SelectionSet)
		{
			return;
		}

		const TArray<FTypedElementHandle> SelectedHandles = SelectionSet->GetSelectedElementHandles();
		for (const FTypedElementHandle& Handle : SelectedHandles)
		{
			if (!Handle)
			{
				continue;
			}

			TryAddActor(
				ResolveActorFromElementHandle(Handle),
				bRequireEditorVisible,
				SeenActors,
				OutActors);
		}

		const TArray<UObject*> SelectedObjects = SelectionSet->GetSelectedObjects();
		for (UObject* SelectedObject : SelectedObjects)
		{
			TryAddActor(
				ResolveObjectToActor(SelectedObject),
				bRequireEditorVisible,
				SeenActors,
				OutActors);
		}
	}

	static void GatherFromSelectionSet(
		USelection* Selection,
		const bool bRequireEditorVisible,
		TSet<AActor*>& SeenActors,
		TArray<AActor*>& OutActors)
	{
		if (!Selection)
		{
			return;
		}

		for (int32 Index = 0; Index < Selection->Num(); ++Index)
		{
			TryAddActor(
				ResolveObjectToActor(Selection->GetSelectedObject(Index)),
				bRequireEditorVisible,
				SeenActors,
				OutActors);
		}

		if (UTypedElementSelectionSet* ElementSelectionSet = Selection->GetElementSelectionSet())
		{
			GatherFromTypedElementSelectionSet(
				ElementSelectionSet,
				bRequireEditorVisible,
				SeenActors,
				OutActors);
		}
	}

	static void CollectEditorSelectionSets(TArray<UTypedElementSelectionSet*>& OutSelectionSets)
	{
		TSet<UTypedElementSelectionSet*> SeenSelectionSets;

		const auto AddSelectionSet = [&](UTypedElementSelectionSet* SelectionSet)
		{
			if (SelectionSet && !SeenSelectionSets.Contains(SelectionSet))
			{
				SeenSelectionSets.Add(SelectionSet);
				OutSelectionSets.Add(SelectionSet);
			}
		};

		if (!GEditor)
		{
			return;
		}

		if (ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
		{
			AddSelectionSet(LevelEditorSubsystem->GetSelectionSet());
		}

		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			if (const TSharedPtr<ILevelEditor> LevelEditor = LevelEditorModule.GetFirstLevelEditor())
			{
				AddSelectionSet(LevelEditor->GetMutableElementSelectionSet());
			}
		}

		if (USelection* SelectedActors = GEditor->GetSelectedActors())
		{
			AddSelectionSet(SelectedActors->GetElementSelectionSet());
		}

		if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
		{
			AddSelectionSet(SelectedComponents->GetElementSelectionSet());
		}

		if (USelection* SelectedObjects = GEditor->GetSelectedObjects())
		{
			AddSelectionSet(SelectedObjects->GetElementSelectionSet());
		}
	}

	static void GatherFromEditorWorldSelectionFlags(
		const bool bRequireEditorVisible,
		TSet<AActor*>& SeenActors,
		TArray<AActor*>& OutActors)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor->IsPendingKillPending())
			{
				continue;
			}

			if (Actor->IsSelectedInEditor())
			{
				TryAddActor(Actor, bRequireEditorVisible, SeenActors, OutActors);
			}

			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (Component && Component->IsSelectedInEditor())
				{
					TryAddActor(Component->GetOwner(), bRequireEditorVisible, SeenActors, OutActors);
				}
			}
		}
	}
}

USelection* GetSelectedActors()
{
	return GEditor ? GEditor->GetSelectedActors() : nullptr;
}

UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

void GatherSelectedActors(TArray<AActor*>& OutActors, const bool bRequireEditorVisible)
{
	OutActors.Reset();
	if (!GEditor)
	{
		return;
	}

	TSet<AActor*> SeenActors;

	TArray<UTypedElementSelectionSet*> SelectionSets;
	CollectEditorSelectionSets(SelectionSets);
	for (UTypedElementSelectionSet* SelectionSet : SelectionSets)
	{
		GatherFromTypedElementSelectionSet(
			SelectionSet,
			bRequireEditorVisible,
			SeenActors,
			OutActors);
	}

	GatherFromSelectionSet(GEditor->GetSelectedActors(), bRequireEditorVisible, SeenActors, OutActors);
	GatherFromSelectionSet(GEditor->GetSelectedComponents(), bRequireEditorVisible, SeenActors, OutActors);
	GatherFromSelectionSet(GEditor->GetSelectedObjects(), bRequireEditorVisible, SeenActors, OutActors);

	if (OutActors.Num() == 0)
	{
		GatherFromEditorWorldSelectionFlags(bRequireEditorVisible, SeenActors, OutActors);
	}
}

void ApplyActorSelection(const TArray<AActor*>& ActorsToSelect, const bool bRequireEditorVisible)
{
	if (!GEditor)
	{
		return;
	}

	GEditor->SelectNone(true, true, false);
	for (AActor* Actor : ActorsToSelect)
	{
		if (!Actor)
		{
			continue;
		}

		if (bRequireEditorVisible && !EditModelToolFilterPolicy::ActorIsEditorVisibleForSelection(Actor))
		{
			continue;
		}

		GEditor->SelectActor(Actor, true, false, /*bSelectEvenIfHidden*/ true);
	}
	GEditor->NoteSelectionChange();
}
}
