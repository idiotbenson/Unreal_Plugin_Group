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
};
