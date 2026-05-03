#pragma once

#include "CoreMinimal.h"

class AActor;

enum class EEditModelToolTagMutation : uint8
{
    Add,
    Remove
};

struct FEditModelToolTagMutationResult
{
    int32 UpdatedActors = 0;
    int32 UnchangedActors = 0;
    int32 TagAssignmentsChanged = 0;
};

namespace EditModelToolTagOperations
{
bool ParseTagList(const FString& TagInput, TArray<FName>& OutTags);
FEditModelToolTagMutationResult ApplyTagMutation(
    const TArray<AActor*>& Actors,
    const TArray<FName>& Tags,
    EEditModelToolTagMutation Mutation,
    const TFunctionRef<bool(AActor*)>& Predicate);
}
