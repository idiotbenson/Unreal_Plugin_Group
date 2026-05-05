#include "EditModelToolPrivatePCH.h"

#include "Core/EditModelToolSelectionUtils.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"

namespace EditModelToolSelectionUtils
{
USelection* GetSelectedActors()
{
    return GEditor ? GEditor->GetSelectedActors() : nullptr;
}

UWorld* GetEditorWorld()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

void GatherSelectedActors(TArray<AActor*>& OutActors)
{
    OutActors.Reset();
    USelection* SelectedActors = GetSelectedActors();
    if (!SelectedActors)
    {
        return;
    }

    OutActors.Reserve(SelectedActors->Num());
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (AActor* Actor = Cast<AActor>(*It))
        {
            OutActors.Add(Actor);
        }
    }
}

void ApplyActorSelection(const TArray<AActor*>& ActorsToSelect)
{
    if (!GEditor)
    {
        return;
    }

    GEditor->SelectNone(true, true, false);
    for (AActor* Actor : ActorsToSelect)
    {
        if (Actor)
        {
            GEditor->SelectActor(Actor, true, false, true);
        }
    }
    GEditor->NoteSelectionChange();
}
}
