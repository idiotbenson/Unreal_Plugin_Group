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
/** False when actor (or attach parent / root) is hidden via editor outliner eye. */
bool ActorIsEditorVisibleForSelection(AActor* Actor);
/** False only when this actor (not attach parents) is hidden in the editor. */
bool ActorIsDirectlyEditorVisibleForSelection(AActor* Actor);
bool ActorPassesTypeFilter(AActor* Actor, const FEditModelToolTypeFilter& Filter);
bool ActorMatchesRule(AActor* Actor, const FString& NameContains, const FName& RequiredTag);
bool ActorMatchesGlobalRule(AActor* Actor, const FEditModelToolFilterSettings& Settings);
int32 GetEffectiveChunkSize(const FEditModelToolFilterSettings& Settings);
}
