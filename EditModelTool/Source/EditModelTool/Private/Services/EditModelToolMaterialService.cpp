#include "Services/EditModelToolMaterialService.h"

#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"

namespace EditModelToolMaterialService
{
FEditModelToolMaterialCopyResult CopyMaterialsByElement(UStaticMeshComponent* Source, UStaticMeshComponent* Target)
{
    FEditModelToolMaterialCopyResult Result;
    if (!Source || !Target)
    {
        return Result;
    }

    const int32 SourceMaterialCount = Source->GetNumMaterials();
    const int32 TargetMaterialCount = Target->GetNumMaterials();
    const int32 SlotCountToCopy = FMath::Min(SourceMaterialCount, TargetMaterialCount);
    if (SlotCountToCopy <= 0)
    {
        return Result;
    }

    bool bComponentChanged = false;
    for (int32 SlotIndex = 0; SlotIndex < SlotCountToCopy; ++SlotIndex)
    {
        UMaterialInterface* SourceMaterial = Source->GetMaterial(SlotIndex);
        if (Target->GetMaterial(SlotIndex) != SourceMaterial)
        {
            if (!bComponentChanged)
            {
                Target->Modify();
                bComponentChanged = true;
            }
            Target->SetMaterial(SlotIndex, SourceMaterial);
            ++Result.UpdatedSlots;
        }
    }

    if (bComponentChanged)
    {
        Target->MarkRenderStateDirty();
        Result.UpdatedComponents = 1;
        Result.bAnyChange = true;
    }

    return Result;
}
}
