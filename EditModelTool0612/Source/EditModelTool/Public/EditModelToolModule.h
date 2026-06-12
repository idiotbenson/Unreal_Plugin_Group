#pragma once

#include "CoreMinimal.h"
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
    void CopyMaterialsFromFirstSelectedMeshByElement();
    void CopyMaterialsFromLastSelectedMeshByElement();
    void CopyMaterialsFromSelectedMeshByElement(int32 SourceActorIndex, const FText& NotEnoughSelectionMessage, const FText& SourceSelectorLabel, const FText& SourceUsedLabel, const FText& TransactionText, const FText& NoChangesText, const FText& ResultTextFormat);
    void RunRuleBasedMovableChunked(const FString& NameContains, const FString& RequiredTag, int32 BatchSize);
    void RunRuleBasedCopyMaterialsChunked(const FString& NameContains, const FString& RequiredTag, int32 BatchSize);
    void GroupSelectedActorsUnderNewActor();
    void LiftLowerMeshIfTwoOverlap(float DeltaZWorld);
    void ReportLocationDeltaBetweenTwoSelectedActors();
    void AutoScanAllStaticMeshesLiftZAsync(float DeltaZWorld);
    void SelectSameNormalFacesFromModelingSelection();
    void SelectStaticMeshActorsByDatasmithMetadataFromSelection(const FString& ExactValue);
    void SelectStaticMeshActorsByDatasmithMetadataFromLevel(const FString& ExactValue);
    /** Keeps StaticMeshActor instances in the current outliner selection; drops plain Actor and other types. */
    void KeepStaticMeshActorsInCurrentSelection();
    void SelectEmptySceneOnlyActorsFromLevel();
    /** Substring match on assigned StaticMesh asset object name (case-insensitive) for any UStaticMeshComponent on the actor. */
    void SelectActorsWithStaticMeshAssetNameInLevel(const FString& Substring);
    /** Substring match on StaticMesh asset material slot names (case-insensitive) for any UStaticMeshComponent on the actor. */
    void SelectActorsWithMaterialSlotNameInLevel(const FString& Substring);
    /** Assigned materials (GetMaterial per slot): exact match on UObject name only (not path); walks instance parent chain. */
    void SelectActorsWithAssignedMaterialExactNameInLevel(const FString& ExactName);
    /** Select actors that have at least one static mesh component using a Nanite-enabled static mesh asset. */
    void SelectActorsUsingNaniteInLevel();

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
        bool bActorTags,
        bool bComponentTags,
        bool bAllTypes,
        bool bMesh,
        bool bSpline,
        bool bBlueprint,
        bool bLight,
        bool bCamera);
    void RemoveTagsFromSelectedActors(
        const FString& TagInput,
        bool bActorTags,
        bool bComponentTags,
        bool bAllTypes,
        bool bMesh,
        bool bSpline,
        bool bBlueprint,
        bool bLight,
        bool bCamera);

    bool TryBeginBatchJob(const FText& JobName);
    void EndBatchJob();

    bool bBatchJobRunning = false;
    FString ActiveBatchJobName;
};
