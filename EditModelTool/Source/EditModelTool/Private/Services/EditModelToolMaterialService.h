#pragma once

#include "CoreMinimal.h"

class UStaticMeshComponent;

struct FEditModelToolMaterialCopyResult
{
    int32 UpdatedComponents = 0;
    int32 UpdatedSlots = 0;
    bool bAnyChange = false;
};

namespace EditModelToolMaterialService
{
FEditModelToolMaterialCopyResult CopyMaterialsByElement(UStaticMeshComponent* Source, UStaticMeshComponent* Target);
}
