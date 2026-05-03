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
    if (SourceMaterialCount <= 0)
    {
        return Result;
    }

    bool bComponentChanged = false;
    for (int32 SlotIndex = 0; SlotIndex < SourceMaterialCount; ++SlotIndex)
    {
        UMaterialInterface* SourceMaterial = Source->GetMaterial(SlotIndex);

        UMaterialInterface* TargetMaterial = nullptr;
        if (SlotIndex >= 0 && SlotIndex < Target->GetNumMaterials())
        {
            TargetMaterial = Target->GetMaterial(SlotIndex);
        }

        if (TargetMaterial != SourceMaterial)
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
