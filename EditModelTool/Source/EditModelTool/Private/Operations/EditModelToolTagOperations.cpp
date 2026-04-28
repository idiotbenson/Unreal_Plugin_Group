#include "Operations/EditModelToolTagOperations.h"

#include "GameFramework/Actor.h"

namespace EditModelToolTagOperations
{
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
    const TFunctionRef<bool(AActor*)>& Predicate)
{
    FEditModelToolTagMutationResult Result;

    for (AActor* Actor : Actors)
    {
        if (!Actor || !IsValid(Actor) || !Predicate(Actor))
        {
            continue;
        }

        bool bChanged = false;
        for (const FName& Tag : Tags)
        {
            if (Mutation == EEditModelToolTagMutation::Add)
            {
                if (!Actor->Tags.Contains(Tag))
                {
                    if (!bChanged)
                    {
                        Actor->Modify();
                        bChanged = true;
                    }
                    Actor->Tags.Add(Tag);
                    ++Result.TagAssignmentsChanged;
                }
            }
            else
            {
                if (Actor->Tags.Contains(Tag))
                {
                    if (!bChanged)
                    {
                        Actor->Modify();
                        bChanged = true;
                    }
                    Actor->Tags.RemoveSingle(Tag);
                    ++Result.TagAssignmentsChanged;
                }
            }
        }

        if (bChanged)
        {
            Actor->PostEditChange();
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
