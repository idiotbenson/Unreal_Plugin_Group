#pragma once

#include "CoreMinimal.h"

class AActor;
class USelection;
class UWorld;

namespace EditModelToolSelectionUtils
{
USelection* GetSelectedActors();
UWorld* GetEditorWorld();
/** Resolves actors from World Outliner / viewport selection (actors and mesh components). */
void GatherSelectedActors(TArray<AActor*>& OutActors, bool bRequireEditorVisible = true);
void ApplyActorSelection(const TArray<AActor*>& ActorsToSelect, bool bRequireEditorVisible = true);
}
