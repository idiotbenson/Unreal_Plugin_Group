#pragma once

#include "CoreMinimal.h"

#include "Core/EditModelToolFilterPolicy.h"

class AActor;

enum class EEditModelToolTagMutation : uint8
{
    Add,
    Remove
};

/** Which tag arrays to mutate: actor Advanced tags and/or component Tags. */
struct FEditModelToolTagTargetFlags
{
    bool bActorTags = true;
    bool bComponentTags = false;
};

struct FEditModelToolTagMutationResult
{
    int32 UpdatedActors = 0;
    int32 UnchangedActors = 0;
    int32 UpdatedComponents = 0;
    int32 UnchangedComponents = 0;
    int32 TagAssignmentsChanged = 0;
};

namespace EditModelToolTagOperations
{
bool ParseTagList(const FString& TagInput, TArray<FName>& OutTags);
FEditModelToolTagMutationResult ApplyTagMutation(
    const TArray<AActor*>& Actors,
    const TArray<FName>& Tags,
    EEditModelToolTagMutation Mutation,
    const FEditModelToolTagTargetFlags& TargetFlags,
    const FEditModelToolTypeFilter& TypeFilter,
    const TFunctionRef<bool(AActor*)>& Predicate);
}
