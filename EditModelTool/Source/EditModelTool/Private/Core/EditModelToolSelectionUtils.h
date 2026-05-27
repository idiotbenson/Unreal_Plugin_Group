#pragma once

#include "CoreMinimal.h"

class AActor;
class USelection;
class UWorld;

namespace EditModelToolSelectionUtils
{
USelection* GetSelectedActors();
UWorld* GetEditorWorld();
void GatherSelectedActors(TArray<AActor*>& OutActors);
void ApplyActorSelection(const TArray<AActor*>& ActorsToSelect);
}
