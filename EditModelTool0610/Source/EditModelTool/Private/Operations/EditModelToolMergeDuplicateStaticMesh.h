#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"

/** Anchor choice for sequential pool merge (see RunMergeDuplicateStaticMeshes). */
enum class EEditModelToolMergeDuplicateMasterPolicy : uint8
{
    /** Each round: first mesh still in the pool is anchor; compared to the rest; matches merge into anchor then all leave the pool. */
    FirstInSelectionOrder,
    /** Each round: last mesh still in the pool is anchor; compared to all others in pool order. */
    LastInSelectionOrder,
    /** User-picked asset is moved to the front of the pool, then the same sequential-first logic runs. */
    PickMasterFromDialog,
};

namespace EditModelToolMergeDuplicateStaticMesh
{
    /** Sequential pool merge by LOD0 fingerprint, then SetStaticMesh on matching components (single undo transaction). */
    void RunMergeDuplicateStaticMeshes(EEditModelToolMergeDuplicateMasterPolicy Policy);

    /** Slate section for the batch-ops dialog (three buttons). */
    TSharedRef<SWidget> BuildMergeDuplicateStaticMeshSection();
}
