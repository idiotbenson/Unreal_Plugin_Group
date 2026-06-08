#include "EditModelToolPrivatePCH.h"

#include "Operations/EditModelToolTagOperations.h"

#include "Camera/CameraComponent.h"
#include "Components/ActorComponent.h"
#include "Components/LightComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Light.h"
#include "GameFramework/Actor.h"

namespace EditModelToolTagOperations
{
namespace
{
void GatherComponentsForTypeFilter(AActor* Actor, const FEditModelToolTypeFilter& Filter, TArray<UActorComponent*>& OutComponents)
{
    OutComponents.Reset();
    if (!Actor)
    {
        return;
    }

    const auto AddUniqueComponent = [&OutComponents](UActorComponent* Component)
    {
        if (Component && IsValid(Component))
        {
            OutComponents.AddUnique(Component);
        }
    };

    if (Filter.bAllTypes)
    {
        TInlineComponentArray<UActorComponent*> AllComponents(Actor);
        for (UActorComponent* Component : AllComponents)
        {
            AddUniqueComponent(Component);
        }
        return;
    }

    if (!Filter.bMesh && !Filter.bSpline && !Filter.bBlueprint && !Filter.bLight && !Filter.bCamera)
    {
        TInlineComponentArray<UActorComponent*> AllComponents(Actor);
        for (UActorComponent* Component : AllComponents)
        {
            AddUniqueComponent(Component);
        }
        return;
    }

    if (Filter.bMesh)
    {
        TInlineComponentArray<UStaticMeshComponent*> MeshComponents(Actor);
        for (UStaticMeshComponent* Component : MeshComponents)
        {
            AddUniqueComponent(Component);
        }
    }
    if (Filter.bSpline)
    {
        TInlineComponentArray<USplineComponent*> SplineComponents(Actor);
        for (USplineComponent* Component : SplineComponents)
        {
            AddUniqueComponent(Component);
        }
    }
    if (Filter.bBlueprint && Cast<UBlueprintGeneratedClass>(Actor->GetClass()) != nullptr)
    {
        TInlineComponentArray<UActorComponent*> AllComponents(Actor);
        for (UActorComponent* Component : AllComponents)
        {
            AddUniqueComponent(Component);
        }
    }
    if (Filter.bLight)
    {
        TInlineComponentArray<ULightComponent*> LightComponents(Actor);
        for (ULightComponent* Component : LightComponents)
        {
            AddUniqueComponent(Component);
        }
    }
    if (Filter.bCamera)
    {
        TInlineComponentArray<UCameraComponent*> CameraComponents(Actor);
        for (UCameraComponent* Component : CameraComponents)
        {
            AddUniqueComponent(Component);
        }
    }
}

bool MutateNameArray(
    TArray<FName>& TagArray,
    const TArray<FName>& Tags,
    const EEditModelToolTagMutation Mutation,
    int32& OutAssignmentsChanged)
{
    bool bChanged = false;
    for (const FName& Tag : Tags)
    {
        if (Mutation == EEditModelToolTagMutation::Add)
        {
            if (!TagArray.Contains(Tag))
            {
                TagArray.Add(Tag);
                ++OutAssignmentsChanged;
                bChanged = true;
            }
        }
        else if (TagArray.Contains(Tag))
        {
            TagArray.RemoveSingle(Tag);
            ++OutAssignmentsChanged;
            bChanged = true;
        }
    }
    return bChanged;
}
}

bool ParseTagList(const FString& TagInput, TArray<FName>& OutTags)
{
    OutTags.Reset();
    FString Work = TagInput;
    Work.ReplaceInline(TEXT(";"), TEXT(","));
    TArray<FString> Parts;
    Work.ParseIntoArray(Parts, TEXT(","), true);

    for (FString& Part : Parts)
    {
        Part.TrimStartAndEndInline();
        if (!Part.IsEmpty())
        {
            OutTags.AddUnique(FName(*Part));
        }
    }

    return OutTags.Num() > 0;
}

FEditModelToolTagMutationResult ApplyTagMutation(
    const TArray<AActor*>& Actors,
    const TArray<FName>& Tags,
    const EEditModelToolTagMutation Mutation,
    const FEditModelToolTagTargetFlags& TargetFlags,
    const FEditModelToolTypeFilter& TypeFilter,
    const TFunctionRef<bool(AActor*)>& Predicate)
{
    FEditModelToolTagMutationResult Result;

    for (AActor* Actor : Actors)
    {
        if (!Actor || !IsValid(Actor) || !Predicate(Actor))
        {
            continue;
        }

        bool bActorChanged = false;
        if (TargetFlags.bActorTags)
        {
            if (MutateNameArray(Actor->Tags, Tags, Mutation, Result.TagAssignmentsChanged))
            {
                Actor->Modify();
                bActorChanged = true;
            }
        }

        bool bAnyComponentChanged = false;
        if (TargetFlags.bComponentTags)
        {
            TArray<UActorComponent*> Components;
            GatherComponentsForTypeFilter(Actor, TypeFilter, Components);

            for (UActorComponent* Component : Components)
            {
                if (MutateNameArray(Component->ComponentTags, Tags, Mutation, Result.TagAssignmentsChanged))
                {
                    Component->Modify();
                    Component->PostEditChange();
                    bAnyComponentChanged = true;
                    ++Result.UpdatedComponents;
                }
                else
                {
                    ++Result.UnchangedComponents;
                }
            }
        }

        if (bActorChanged || bAnyComponentChanged)
        {
            if (bActorChanged)
            {
                Actor->PostEditChange();
            }
            Actor->MarkPackageDirty();
            ++Result.UpdatedActors;
        }
        else
        {
            ++Result.UnchangedActors;
        }
    }

    return Result;
}
}
