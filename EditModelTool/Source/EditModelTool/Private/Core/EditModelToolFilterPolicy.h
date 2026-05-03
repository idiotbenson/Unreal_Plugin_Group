#pragma once

#include "CoreMinimal.h"

class AActor;

struct FEditModelToolTypeFilter
{
    bool bAllTypes = true;
    bool bMesh = false;
    bool bSpline = false;
    bool bBlueprint = false;
    bool bLight = false;
    bool bCamera = false;
};

struct FEditModelToolFilterSettings
{
    FString NameContains;
    FString RequiredTag;
    int32 ChunkSize = 32;
};

namespace EditModelToolFilterPolicy
{
bool ActorPassesTypeFilter(AActor* Actor, const FEditModelToolTypeFilter& Filter);
bool ActorMatchesRule(AActor* Actor, const FString& NameContains, const FName& RequiredTag);
bool ActorMatchesGlobalRule(AActor* Actor, const FEditModelToolFilterSettings& Settings);
int32 GetEffectiveChunkSize(const FEditModelToolFilterSettings& Settings);
}
