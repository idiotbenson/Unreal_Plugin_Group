#include "Core/EditModelToolFilterPolicy.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Light.h"
#include "GameFramework/Actor.h"

namespace
{
constexpr int32 C_DefaultChunkSize = 32;
}

namespace EditModelToolFilterPolicy
{
bool ActorPassesTypeFilter(AActor* Actor, const FEditModelToolTypeFilter& Filter)
{
    if (!Actor)
    {
        return false;
    }
    if (Filter.bAllTypes)
    {
        return true;
    }
    if (!Filter.bMesh && !Filter.bSpline && !Filter.bBlueprint && !Filter.bLight && !Filter.bCamera)
    {
        return true;
    }
    if (Filter.bMesh && Actor->FindComponentByClass<UStaticMeshComponent>())
    {
        return true;
    }
    if (Filter.bSpline && Actor->FindComponentByClass<USplineComponent>())
    {
        return true;
    }
    if (Filter.bBlueprint && Cast<UBlueprintGeneratedClass>(Actor->GetClass()) != nullptr)
    {
        return true;
    }
    if (Filter.bLight && (Actor->IsA(ALight::StaticClass()) || Actor->FindComponentByClass<ULightComponent>()))
    {
        return true;
    }
    if (Filter.bCamera && (Actor->IsA(ACameraActor::StaticClass()) || Actor->FindComponentByClass<UCameraComponent>()))
    {
        return true;
    }
    return false;
}

bool ActorMatchesRule(AActor* Actor, const FString& NameContains, const FName& RequiredTag)
{
    if (!Actor || !IsValid(Actor))
    {
        return false;
    }

    if (!NameContains.IsEmpty())
    {
        const FString Label = Actor->GetActorLabel();
        if (!Label.Contains(NameContains, ESearchCase::IgnoreCase))
        {
            return false;
        }
    }

    if (RequiredTag != NAME_None && !Actor->Tags.Contains(RequiredTag))
    {
        return false;
    }

    return true;
}

bool ActorMatchesGlobalRule(AActor* Actor, const FEditModelToolFilterSettings& Settings)
{
    const FString TrimmedTag = Settings.RequiredTag.TrimStartAndEnd();
    const FName RequiredTagName = TrimmedTag.IsEmpty() ? NAME_None : FName(*TrimmedTag);
    return ActorMatchesRule(Actor, Settings.NameContains, RequiredTagName);
}

int32 GetEffectiveChunkSize(const FEditModelToolFilterSettings& Settings)
{
    return Settings.ChunkSize > 0 ? Settings.ChunkSize : C_DefaultChunkSize;
}
}
