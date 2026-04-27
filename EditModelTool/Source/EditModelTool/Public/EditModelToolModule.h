#pragma once

#include "Modules/ModuleManager.h"

class FEditModelToolModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    void OpenRenameDialog();
    void RenameSelectedActors(const FString& BaseName);
    void SetSelectedMeshesMovable();
    void GroupSelectedActorsUnderNewActor();
    void LiftLowerMeshIfTwoOverlap(float DeltaZWorld);
    void AutoScanAllStaticMeshesLiftZAsync(float DeltaZWorld);
    void SelectStaticMeshActorsByDatasmithMetadataFromSelection(const FString& ExactValue);
    void SelectStaticMeshActorsByDatasmithMetadataFromLevel(const FString& ExactValue);

    /** bAllTypes: ignore per-type checkboxes. Otherwise OR of mesh/spline/blueprint/light/camera. */
    void SearchLevelByTagsAndSelect(
        const FString& Query,
        bool bAllTypes,
        bool bMesh,
        bool bSpline,
        bool bBlueprint,
        bool bLight,
        bool bCamera);

    void AddTagsToSelectedActors(
        const FString& TagInput,
        bool bAllTypes,
        bool bMesh,
        bool bSpline,
        bool bBlueprint,
        bool bLight,
        bool bCamera);
};
