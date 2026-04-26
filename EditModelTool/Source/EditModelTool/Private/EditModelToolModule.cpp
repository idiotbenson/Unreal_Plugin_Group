#include "EditModelToolModule.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Internationalization/Regex.h"
#include "Misc/DefaultValueHelper.h"
#include "Async/Async.h"
#include "Misc/ScopedSlowTask.h"
#include "Templates/SharedPointer.h"
#include "CollisionQueryParams.h"
#include "Math/IntVector.h"
#include "Engine/StaticMesh.h"

#define LOCTEXT_NAMESPACE "FEditModelToolModule"

namespace
{
    struct FMeshPOD
    {
        FString MeshKey;
        FBox Bounds;
        FVector Center;
        float Volume;
    };

    struct FIndexPair
    {
        int32 A;
        int32 B;
    };

    constexpr float C_PositionTolerance = 0.1f;
    constexpr float C_RotationToleranceDeg = 0.1f;
    constexpr float C_ScaleTolerance = 0.001f;
    constexpr float C_BoundsTolerance = 0.1f;

    bool MeshComponentsPassGeometryOverlap(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
    {
        if (!M0 || !M1)
        {
            return false;
        }
        const FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EditModelToolGeometryOverlap), false);
        return M0->ComponentOverlapComponent(
            M1,
            M1->GetComponentLocation(),
            M1->GetComponentQuat(),
            QueryParams);
    }

    bool MeshComponentsPassCoincidence(
        UStaticMeshComponent* M0,
        UStaticMeshComponent* M1,
        const FBox& B0,
        const FBox& B1)
    {
        if (!M0 || !M1)
        {
            return false;
        }
        const UStaticMesh* A0 = M0->GetStaticMesh();
        const UStaticMesh* A1 = M1->GetStaticMesh();
        const bool bSameMeshAsset = (A0 != nullptr) && (A0 == A1);
        const bool bSameLocation = M0->GetComponentLocation().Equals(M1->GetComponentLocation(), C_PositionTolerance);
        const bool bSameRotation = M0->GetComponentRotation().Equals(M1->GetComponentRotation(), C_RotationToleranceDeg);
        const bool bSameScale = M0->GetComponentScale().Equals(M1->GetComponentScale(), C_ScaleTolerance);
        const bool bSameBounds =
            FMath::IsNearlyEqual(B0.Min.X, B1.Min.X, C_BoundsTolerance) &&
            FMath::IsNearlyEqual(B0.Min.Y, B1.Min.Y, C_BoundsTolerance) &&
            FMath::IsNearlyEqual(B0.Min.Z, B1.Min.Z, C_BoundsTolerance) &&
            FMath::IsNearlyEqual(B0.Max.X, B1.Max.X, C_BoundsTolerance) &&
            FMath::IsNearlyEqual(B0.Max.Y, B1.Max.Y, C_BoundsTolerance) &&
            FMath::IsNearlyEqual(B0.Max.Z, B1.Max.Z, C_BoundsTolerance);

        return bSameMeshAsset && bSameLocation && bSameRotation && bSameScale && bSameBounds;
    }

    void MakeActorMovable(AActor* Actor)
    {
        if (!Actor)
        {
            return;
        }
        TArray<USceneComponent*> SceneComponents;
        Actor->GetComponents<USceneComponent>(SceneComponents);
        for (USceneComponent* SceneComp : SceneComponents)
        {
            if (SceneComp && SceneComp->Mobility != EComponentMobility::Movable)
            {
                SceneComp->Modify();
                SceneComp->SetMobility(EComponentMobility::Movable);
            }
        }
    }

    FBox GetWorldBoxForMeshComponent(const UStaticMeshComponent* InMesh)
    {
        check(InMesh);
        return InMesh->CalcBounds(InMesh->GetComponentTransform()).GetBox();
    }

    void ApplyZLiftToActor(AActor* ToMove, const float DeltaZWorld)
    {
        if (!ToMove)
        {
            return;
        }
        ToMove->Modify();
        MakeActorMovable(ToMove);
        ToMove->AddActorWorldOffset(FVector(0.f, 0.f, DeltaZWorld), false, nullptr, ETeleportType::None);
    }

    TArray<FIndexPair> BuildBroadPhaseCandidatePairs(const TArray<FMeshPOD>& PODs)
    {
        TArray<FIndexPair> Out;
        if (PODs.Num() < 2)
        {
            return Out;
        }

        constexpr int32 CellSize = 200;
        TMap<FString, TArray<int32>> GroupByMesh;
        for (int32 I = 0; I < PODs.Num(); ++I)
        {
            GroupByMesh.FindOrAdd(PODs[I].MeshKey).Add(I);
        }

        TSet<uint64> Seen;
        for (const auto& Kvp : GroupByMesh)
        {
            const TArray<int32>& Group = Kvp.Value;
            if (Group.Num() < 2)
            {
                continue;
            }

            TMap<FString, TArray<int32>> CellMap;
            for (int32 Id : Group)
            {
                const FMeshPOD& P = PODs[Id];
                const FIntVector C(
                    FMath::FloorToInt(P.Center.X / CellSize),
                    FMath::FloorToInt(P.Center.Y / CellSize),
                    FMath::FloorToInt(P.Center.Z / CellSize));
                const FString CKey = FString::Printf(TEXT("%d_%d_%d"), C.X, C.Y, C.Z);
                CellMap.FindOrAdd(CKey).Add(Id);
            }

            auto TryPair = [&Out, &PODs, &Seen](int32 I, int32 J)
            {
                if (I == J)
                {
                    return;
                }
                const int32 A = I < J ? I : J;
                const int32 B = I < J ? J : I;
                const uint64 Key = (static_cast<uint64>(static_cast<uint32>(A)) << 32) | static_cast<uint32>(B);
                if (Seen.Contains(Key))
                {
                    return;
                }

                const FMeshPOD& P0 = PODs[A];
                const FMeshPOD& P1 = PODs[B];
                if (!P0.Bounds.IsValid || !P1.Bounds.IsValid)
                {
                    return;
                }

                const bool bBoundsIntersects = P0.Bounds.Intersect(P1.Bounds);
                const float CentDist = FVector::Distance(P0.Center, P1.Center);
                const float Denom = FMath::Max3(P0.Volume, P1.Volume, 1.f);
                const float VolRel = FMath::Abs(P0.Volume - P1.Volume) / Denom;
                const bool bCentersTight = CentDist < 5.f;
                const bool bVolTight = VolRel < 0.2f;
                if (!bBoundsIntersects && !(bCentersTight && bVolTight && CentDist < 25.f))
                {
                    return;
                }

                constexpr float BroadBoundsTol = 2.f;
                const bool bBoundsSimilar =
                    FMath::IsNearlyEqual(P0.Bounds.Min.X, P1.Bounds.Min.X, BroadBoundsTol) &&
                    FMath::IsNearlyEqual(P0.Bounds.Min.Y, P1.Bounds.Min.Y, BroadBoundsTol) &&
                    FMath::IsNearlyEqual(P0.Bounds.Min.Z, P1.Bounds.Min.Z, BroadBoundsTol) &&
                    FMath::IsNearlyEqual(P0.Bounds.Max.X, P1.Bounds.Max.X, BroadBoundsTol) &&
                    FMath::IsNearlyEqual(P0.Bounds.Max.Y, P1.Bounds.Max.Y, BroadBoundsTol) &&
                    FMath::IsNearlyEqual(P0.Bounds.Max.Z, P1.Bounds.Max.Z, BroadBoundsTol);
                if (!bBoundsIntersects && !bBoundsSimilar)
                {
                    return;
                }

                Seen.Add(Key);
                Out.Add(FIndexPair{A, B});
            };

            for (int32 Id : Group)
            {
                const FMeshPOD& P = PODs[Id];
                for (int32 Dxi = -1; Dxi <= 1; ++Dxi)
                {
                    for (int32 Dyi = -1; Dyi <= 1; ++Dyi)
                    {
                        for (int32 Dzi = -1; Dzi <= 1; ++Dzi)
                        {
                            const FIntVector C(
                                FMath::FloorToInt(P.Center.X / CellSize) + Dxi,
                                FMath::FloorToInt(P.Center.Y / CellSize) + Dyi,
                                FMath::FloorToInt(P.Center.Z / CellSize) + Dzi);
                            const FString CKey = FString::Printf(TEXT("%d_%d_%d"), C.X, C.Y, C.Z);
                            const TArray<int32>* InCell = CellMap.Find(CKey);
                            if (!InCell)
                            {
                                continue;
                            }
                            for (int32 Other : *InCell)
                            {
                                if (Other > Id)
                                {
                                    TryPair(Id, Other);
                                }
                            }
                        }
                    }
                }
            }
        }

        return Out;
    }

    void RunBroadBCLiftForMeshSetAsync(
        TSharedPtr<TArray<TWeakObjectPtr<UStaticMeshComponent>>> SharedComps,
        TArray<FMeshPOD> PODs,
        const float DeltaZWorld,
        const FText& ProgressText,
        const FText& StageText)
    {
        if (!SharedComps.IsValid() || PODs.Num() < 2)
        {
            return;
        }

        Async(EAsyncExecution::ThreadPool, [PODs = MoveTemp(PODs)]() mutable
        {
            return BuildBroadPhaseCandidatePairs(PODs);
        })
            .Next([SharedComps, DeltaZWorld, ProgressText, StageText](TArray<FIndexPair> Cands)
            {
                AsyncTask(
                    ENamedThreads::GameThread,
                    [SharedComps, Cands, DeltaZWorld, ProgressText, StageText]()
                    {
                        const int32 WorkUnits = Cands.Num() + 1;
                        FScopedSlowTask Progress(
                            FMath::Max(1.f, static_cast<float>(WorkUnits)),
                            ProgressText);
                        Progress.EnterProgressFrame(1.f, StageText);

                        if (Cands.Num() == 0)
                        {
                            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoBroadCandidates", "No broad-phase candidate pairs found."));
                            return;
                        }

                        int32 Applied = 0;
                        {
                            const FScopedTransaction Transaction(LOCTEXT("BroadBCLiftTx", "B+C Z lift for mesh pairs"));
                            for (const FIndexPair& Pr : Cands)
                            {
                                Progress.EnterProgressFrame(1.f);
                                if (!SharedComps->IsValidIndex(Pr.A) || !SharedComps->IsValidIndex(Pr.B))
                                {
                                    continue;
                                }
                                UStaticMeshComponent* M0 = (*SharedComps)[Pr.A].Get();
                                UStaticMeshComponent* M1 = (*SharedComps)[Pr.B].Get();
                                if (!M0 || !M1 || M0 == M1)
                                {
                                    continue;
                                }
                                AActor* A0 = M0->GetOwner();
                                AActor* A1 = M1->GetOwner();
                                if (!A0 || !A1 || A0 == A1)
                                {
                                    continue;
                                }
                                const FBox B0 = GetWorldBoxForMeshComponent(M0);
                                const FBox B1 = GetWorldBoxForMeshComponent(M1);
                                if (!B0.IsValid || !B1.IsValid)
                                {
                                    continue;
                                }
                                if (!MeshComponentsPassGeometryOverlap(M0, M1))
                                {
                                    continue;
                                }
                                if (!MeshComponentsPassCoincidence(M0, M1, B0, B1))
                                {
                                    continue;
                                }
                                AActor* const ToMove = (B0.Min.Z < B1.Min.Z) ? A0 : A1;
                                ApplyZLiftToActor(ToMove, DeltaZWorld);
                                ++Applied;
                            }
                        }

                        FMessageDialog::Open(
                            EAppMsgType::Ok,
                            FText::Format(
                                LOCTEXT("BroadBCLiftDoneFmt", "Z offset per lift: {0} (world).  Candidate pairs: {1}.  Applied Z lift: {2}."),
                                FText::AsNumber(DeltaZWorld),
                                FText::AsNumber(Cands.Num()),
                                FText::AsNumber(Applied)));
                    });
            });
    }

    FString EscapeRegexLiteral(const FString& Input)
    {
        FString Escaped = Input;
        Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Escaped.ReplaceInline(TEXT("."), TEXT("\\."));
        Escaped.ReplaceInline(TEXT("^"), TEXT("\\^"));
        Escaped.ReplaceInline(TEXT("$"), TEXT("\\$"));
        Escaped.ReplaceInline(TEXT("|"), TEXT("\\|"));
        Escaped.ReplaceInline(TEXT("("), TEXT("\\("));
        Escaped.ReplaceInline(TEXT(")"), TEXT("\\)"));
        Escaped.ReplaceInline(TEXT("["), TEXT("\\["));
        Escaped.ReplaceInline(TEXT("]"), TEXT("\\]"));
        Escaped.ReplaceInline(TEXT("{"), TEXT("\\{"));
        Escaped.ReplaceInline(TEXT("}"), TEXT("\\}"));
        Escaped.ReplaceInline(TEXT("*"), TEXT("\\*"));
        Escaped.ReplaceInline(TEXT("+"), TEXT("\\+"));
        Escaped.ReplaceInline(TEXT("?"), TEXT("\\?"));
        return Escaped;
    }
}

void FEditModelToolModule::StartupModule()
{
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FEditModelToolModule::RegisterMenus));
}

void FEditModelToolModule::ShutdownModule()
{
    if (UToolMenus::TryGet())
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
    }
}

void FEditModelToolModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);

    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
    FToolMenuSection& Section = Menu->FindOrAddSection("EditModelToolSection");

    Section.AddMenuEntry(
        "OpenEditModelToolDialog",
        LOCTEXT("EditModelToolMenuLabel", "Batch Rename Selected"),
        LOCTEXT("EditModelToolMenuTooltip", "Rename selected outliner actors to Name + Number (000000)."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(this, &FEditModelToolModule::OpenRenameDialog)));
}

void FEditModelToolModule::OpenRenameDialog()
{
    TSharedPtr<SEditableTextBox> InputBox;
    TSharedPtr<SEditableTextBox> ZOffsetBox;
    TSharedPtr<SWindow> DialogWindow;
    bool bConfirmed = false;
    FString EnteredName;

    DialogWindow = SNew(SWindow)
        .Title(LOCTEXT("DialogTitle", "Batch Rename Selected Actors"))
        .SizingRule(ESizingRule::Autosized)
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    DialogWindow->SetContent(
        SNew(SBox)
        .Padding(12.f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 8.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("InputHint", "Input base name (ex: window):"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SAssignNew(InputBox, SEditableTextBox)
                .HintText(LOCTEXT("InputPlaceholder", "window"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ZOffsetHint", "Z offset (world units) — used by both: [Z offset: selection (broad+BC)] and [Auto: all meshes (broad+BC)]:"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SAssignNew(ZOffsetBox, SEditableTextBox)
                .Text(FText::FromString(TEXT("50")))
                .HintText(LOCTEXT("ZOffsetPlaceholder", "50"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 12.f, 0.f, 0.f)
            .HAlign(HAlign_Right)
            [
                SNew(SUniformGridPanel)
                .SlotPadding(4.f)
                + SUniformGridPanel::Slot(0, 0)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CancelButton", "Cancel"))
                    .OnClicked_Lambda([DialogWindow]()
                    {
                        if (DialogWindow.IsValid())
                        {
                            DialogWindow->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ]
                + SUniformGridPanel::Slot(1, 0)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("OkButton", "OK"))
                    .OnClicked_Lambda([&bConfirmed, &EnteredName, &InputBox, DialogWindow]()
                    {
                        bConfirmed = true;
                        EnteredName = InputBox.IsValid() ? InputBox->GetText().ToString() : FString();
                        if (DialogWindow.IsValid())
                        {
                            DialogWindow->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ]
                + SUniformGridPanel::Slot(2, 0)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SetMovableButton", "Set Meshes Movable"))
                    .OnClicked_Lambda([this, DialogWindow]()
                    {
                        SetSelectedMeshesMovable();
                        if (DialogWindow.IsValid())
                        {
                            DialogWindow->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ]
                + SUniformGridPanel::Slot(3, 0)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("GroupUnderNewActorButton", "Movable + Group To New Actor"))
                    .OnClicked_Lambda([this, DialogWindow]()
                    {
                        GroupSelectedActorsUnderNewActor();
                        if (DialogWindow.IsValid())
                        {
                            DialogWindow->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ]
                + SUniformGridPanel::Slot(0, 1)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("LiftOverlapZButton", "Z offset: selection (broad+BC)"))
                    .ToolTipText(LOCTEXT("LiftOverlapZTooltip", "Gathers all static mesh components on the selected outliner actor(s) (you can select many). Uses the same broad-phase + B+C + Z as Auto, but only pairs within that set. The Z value above is used for each successful lift. Need at least two mesh parts in the selection in total."))
                    .OnClicked_Lambda([this, &ZOffsetBox, DialogWindow]()
                    {
                        const FString ZText = ZOffsetBox.IsValid() ? ZOffsetBox->GetText().ToString() : FString();
                        float DeltaZ = 0.f;
                        if (!FDefaultValueHelper::ParseFloat(ZText, DeltaZ))
                        {
                            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("BadZValue", "Please enter a valid number for Z offset."));
                            return FReply::Handled();
                        }
                        LiftLowerMeshIfTwoOverlap(DeltaZ);
                        if (DialogWindow.IsValid())
                        {
                            DialogWindow->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ]
                + SUniformGridPanel::Slot(1, 1)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("AutoScanZButton", "Auto: all meshes (broad+BC)"))
                    .ToolTipText(LOCTEXT("AutoScanZTooltip", "Scans the whole current level. Broad filter on a background thread, then B+C on the game thread. The Z value is taken from the field above (same as the two-mesh button); e.g. 50 = +50 on Z. Change that number, then press this button. May take a while on very large levels."))
                    .OnClicked_Lambda([this, &ZOffsetBox, DialogWindow]()
                    {
                        const FString ZText = ZOffsetBox.IsValid() ? ZOffsetBox->GetText().ToString() : FString();
                        float DeltaZ = 0.f;
                        if (!FDefaultValueHelper::ParseFloat(ZText, DeltaZ))
                        {
                            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("BadZValue2", "Please enter a valid number for Z offset."));
                            return FReply::Handled();
                        }
                        AutoScanAllStaticMeshesLiftZAsync(DeltaZ);
                        if (DialogWindow.IsValid())
                        {
                            DialogWindow->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ]
            ]
        ]);

    FSlateApplication::Get().AddModalWindow(DialogWindow.ToSharedRef(), nullptr);

    if (bConfirmed)
    {
        RenameSelectedActors(EnteredName);
    }
}

void FEditModelToolModule::GroupSelectedActorsUnderNewActor()
{
    USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
    if (!SelectedActors || SelectedActors->Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoSelectionForGroup", "No actors selected in outliner."));
        return;
    }

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!EditorWorld)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoEditorWorldForGroup", "Could not get editor world."));
        return;
    }

    TArray<AActor*> ActorsToGroup;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (AActor* Actor = Cast<AActor>(*It))
        {
            ActorsToGroup.Add(Actor);
        }
    }

    if (ActorsToGroup.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoValidActorsForGroup", "No valid actors selected."));
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("GroupSelectedActorsTransaction", "Group Selected Actors Under New Actor"));

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = MakeUniqueObjectName(EditorWorld, AActor::StaticClass(), TEXT("GroupRoot"));
    AActor* ParentActor = EditorWorld->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
    if (!ParentActor)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("CreateGroupActorFailed", "Failed to create new actor at origin."));
        return;
    }

    ParentActor->Modify();
    ParentActor->SetActorLabel(TEXT("GroupRoot"));
    if (!ParentActor->GetRootComponent())
    {
        USceneComponent* RootScene = NewObject<USceneComponent>(ParentActor, TEXT("Root"));
        if (RootScene)
        {
            RootScene->Mobility = EComponentMobility::Movable;
            ParentActor->SetRootComponent(RootScene);
            RootScene->RegisterComponent();
        }
    }

    int32 MobilityChangedCount = 0;
    int32 AttachedCount = 0;
    for (AActor* Actor : ActorsToGroup)
    {
        if (!Actor || Actor == ParentActor)
        {
            continue;
        }

        Actor->Modify();

        TArray<USceneComponent*> SceneComponents;
        Actor->GetComponents<USceneComponent>(SceneComponents);
        for (USceneComponent* SceneComp : SceneComponents)
        {
            if (!SceneComp)
            {
                continue;
            }

            if (SceneComp->Mobility != EComponentMobility::Movable)
            {
                SceneComp->Modify();
                SceneComp->SetMobility(EComponentMobility::Movable);
                ++MobilityChangedCount;
            }
        }

        if (GEditor->CanParentActors(ParentActor, Actor))
        {
            GEditor->ParentActors(ParentActor, Actor, NAME_None);
            ++AttachedCount;
        }
    }

    GEditor->SelectNone(true, true, false);
    GEditor->SelectActor(ParentActor, true, true, true);

    if (AttachedCount == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoActorAttached", "No selected actors could be attached to the new actor."));
        return;
    }

    if (MobilityChangedCount == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("GroupedNoMobilityChange", "Grouped selected actors under new actor at (0,0,0). Mobility was already movable."));
    }
}

void FEditModelToolModule::LiftLowerMeshIfTwoOverlap(const float DeltaZWorld)
{
    USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
    if (!SelectedActors)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoSelectionForLiftZ", "No selection."));
        return;
    }

    TArray<TWeakObjectPtr<UStaticMeshComponent>> MeshComps;
    TArray<FMeshPOD> PODs;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        AActor* Actor = Cast<AActor>(*It);
        if (!Actor)
        {
            continue;
        }
        TArray<UStaticMeshComponent*> Sms;
        Actor->GetComponents<UStaticMeshComponent>(Sms);
        for (UStaticMeshComponent* Sm : Sms)
        {
            if (!Sm)
            {
                continue;
            }
            UStaticMesh* Asset = Sm->GetStaticMesh();
            if (!Asset)
            {
                continue;
            }
            FMeshPOD Pod;
            Pod.MeshKey = Asset->GetPathName();
            Pod.Bounds = GetWorldBoxForMeshComponent(Sm);
            if (!Pod.Bounds.IsValid)
            {
                continue;
            }
            Pod.Center = Pod.Bounds.GetCenter();
            Pod.Volume = Pod.Bounds.GetVolume();
            MeshComps.Add(Sm);
            PODs.Add(MoveTemp(Pod));
        }
    }

    if (PODs.Num() < 2)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            LOCTEXT("SelectionNeedMeshParts", "Select at least one actor with static mesh(es). Need at least two static mesh parts total (one actor with two meshes, or more actors) for overlap search."));
        return;
    }

    const TSharedPtr<TArray<TWeakObjectPtr<UStaticMeshComponent>>> SharedComps =
        MakeShared<TArray<TWeakObjectPtr<UStaticMeshComponent>>>(MoveTemp(MeshComps));
    RunBroadBCLiftForMeshSetAsync(
        SharedComps,
        MoveTemp(PODs),
        DeltaZWorld,
        LOCTEXT("SelectScanProgress", "Selected meshes: broad+BC+Z (game thread)"),
        LOCTEXT("SelectScanStage", "Starting B+C on selection candidates..."));
}

void FEditModelToolModule::AutoScanAllStaticMeshesLiftZAsync(const float DeltaZWorld)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoWorldAuto", "Could not get editor world."));
        return;
    }

    TArray<TWeakObjectPtr<UStaticMeshComponent>> MeshComps;
    TArray<FMeshPOD> PODs;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !IsValid(Actor))
        {
            continue;
        }
        TArray<UStaticMeshComponent*> Sms;
        Actor->GetComponents<UStaticMeshComponent>(Sms);
        for (UStaticMeshComponent* Sm : Sms)
        {
            if (!Sm)
            {
                continue;
            }
            UStaticMesh* Asset = Sm->GetStaticMesh();
            if (!Asset)
            {
                continue;
            }
            FMeshPOD Pod;
            Pod.MeshKey = Asset->GetPathName();
            Pod.Bounds = GetWorldBoxForMeshComponent(Sm);
            if (!Pod.Bounds.IsValid)
            {
                continue;
            }
            Pod.Center = Pod.Bounds.GetCenter();
            Pod.Volume = Pod.Bounds.GetVolume();
            MeshComps.Add(Sm);
            PODs.Add(MoveTemp(Pod));
        }
    }

    if (PODs.Num() < 2)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NotEnoughMeshes", "Not enough static mesh components in the level."));
        return;
    }

    const TSharedPtr<TArray<TWeakObjectPtr<UStaticMeshComponent>>> SharedComps =
        MakeShared<TArray<TWeakObjectPtr<UStaticMeshComponent>>>(MoveTemp(MeshComps));
    RunBroadBCLiftForMeshSetAsync(
        SharedComps,
        MoveTemp(PODs),
        DeltaZWorld,
        LOCTEXT("AutoScanProgress", "Static mesh: broad+BC+Z (game thread)"),
        LOCTEXT("AutoScanStage", "Starting B+C on candidates..."));
}

void FEditModelToolModule::SetSelectedMeshesMovable()
{
    USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
    if (!SelectedActors || SelectedActors->Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoSelectionForMobility", "No actors selected in outliner."));
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetMovableTransaction", "Set Selected Meshes Movable"));

    int32 UpdatedCount = 0;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        AActor* Actor = Cast<AActor>(*It);
        if (!Actor)
        {
            continue;
        }

        TArray<UStaticMeshComponent*> MeshComponents;
        Actor->GetComponents<UStaticMeshComponent>(MeshComponents);

        for (UStaticMeshComponent* MeshComponent : MeshComponents)
        {
            if (!MeshComponent)
            {
                continue;
            }

            if (MeshComponent->Mobility != EComponentMobility::Movable)
            {
                MeshComponent->Modify();
                MeshComponent->SetMobility(EComponentMobility::Movable);
                ++UpdatedCount;
            }
        }
    }

    if (UpdatedCount == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoMeshUpdated", "No static mesh component needed mobility changes."));
    }
}

void FEditModelToolModule::RenameSelectedActors(const FString& BaseName)
{
    const FString TrimmedName = BaseName.TrimStartAndEnd();
    if (TrimmedName.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("EmptyName", "Please input a valid base name."));
        return;
    }

    USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
    if (!SelectedActors || SelectedActors->Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoSelection", "No actors selected in outliner."));
        return;
    }

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!EditorWorld)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoEditorWorld", "Could not get editor world."));
        return;
    }

    int32 StartIndex = 0;
    const FString PatternString = FString::Printf(TEXT("^%s(\\d{6})$"), *EscapeRegexLiteral(TrimmedName));
    const FRegexPattern Pattern(PatternString);

    for (TActorIterator<AActor> It(EditorWorld); It; ++It)
    {
        const AActor* ExistingActor = *It;
        if (!ExistingActor)
        {
            continue;
        }

        const FString ExistingLabel = ExistingActor->GetActorLabel();
        FRegexMatcher Matcher(Pattern, ExistingLabel);
        if (Matcher.FindNext())
        {
            const FString Digits = Matcher.GetCaptureGroup(1);
            const int32 ExistingIndex = FCString::Atoi(*Digits);
            StartIndex = FMath::Max(StartIndex, ExistingIndex + 1);
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("RenameTransaction", "Batch Rename Actors"));

    int32 Index = StartIndex;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        AActor* Actor = Cast<AActor>(*It);
        if (!Actor)
        {
            continue;
        }

        Actor->Modify();
        const FString NewLabel = FString::Printf(TEXT("%s%06d"), *TrimmedName, Index++);
        Actor->SetActorLabel(NewLabel);
    }
}

IMPLEMENT_MODULE(FEditModelToolModule, EditModelTool)

#undef LOCTEXT_NAMESPACE
