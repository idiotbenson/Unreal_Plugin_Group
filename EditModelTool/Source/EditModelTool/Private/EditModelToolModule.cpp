#include "EditModelToolModule.h"
#include "Core/EditModelToolBatchRunner.h"
#include "Core/EditModelToolFilterPolicy.h"
#include "Core/EditModelToolSelectionUtils.h"
#include "Operations/EditModelToolTagOperations.h"
#include "Services/EditModelToolMaterialService.h"
#include "UI/EditModelToolNotifications.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditor.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/LightComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Light.h"
#include "Components/ActorComponent.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
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
#include "DatasmithAssetUserData.h"
#include "Math/IntVector.h"
#include "Engine/StaticMesh.h"
#include "Containers/Ticker.h"
#include "Misc/Guid.h"
#include "HAL/PlatformTime.h"

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
    FEditModelToolFilterSettings GFilterSettings;

    FString GetTrimmedText(const TSharedPtr<SEditableTextBox>& TextBox)
    {
        return TextBox.IsValid() ? TextBox->GetText().ToString().TrimStartAndEnd() : FString();
    }

    bool TryParseFloatTextBox(
        const TSharedPtr<SEditableTextBox>& TextBox,
        float& OutValue,
        const FText& InvalidInputMessage)
    {
        const FString TextValue = GetTrimmedText(TextBox);
        if (FDefaultValueHelper::ParseFloat(TextValue, OutValue))
        {
            return true;
        }
        FMessageDialog::Open(EAppMsgType::Ok, InvalidInputMessage);
        return false;
    }

    bool TryParseIntTextBox(
        const TSharedPtr<SEditableTextBox>& TextBox,
        int32& OutValue,
        const FText& InvalidInputMessage)
    {
        const FString TextValue = GetTrimmedText(TextBox);
        if (FDefaultValueHelper::ParseInt(TextValue, OutValue) && OutValue > 0)
        {
            return true;
        }
        FMessageDialog::Open(EAppMsgType::Ok, InvalidInputMessage);
        return false;
    }

    FBox GetWorldBoxForMeshComponent(const UStaticMeshComponent* InMesh);

    bool MeshComponentsPassGeometryOverlap(UStaticMeshComponent* M0, UStaticMeshComponent* M1)
    {
        if (!M0 || !M1)
        {
            return false;
        }

        // Avoid collision-profile dependency by using world-space bounds overlap.
        const FBox B0 = GetWorldBoxForMeshComponent(M0);
        const FBox B1 = GetWorldBoxForMeshComponent(M1);
        if (!B0.IsValid || !B1.IsValid)
        {
            return false;
        }
        return B0.Intersect(B1);
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

        return bSameLocation && bSameRotation && bSameScale && bSameBounds;
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
        TSet<uint64> Seen;
        TMap<FIntVector, TArray<int32>> CellMap;
        for (int32 Id = 0; Id < PODs.Num(); ++Id)
        {
            const FMeshPOD& P = PODs[Id];
            const FIntVector C(
                FMath::FloorToInt(P.Center.X / CellSize),
                FMath::FloorToInt(P.Center.Y / CellSize),
                FMath::FloorToInt(P.Center.Z / CellSize));
            CellMap.FindOrAdd(C).Add(Id);
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

        for (int32 Id = 0; Id < PODs.Num(); ++Id)
        {
            const FMeshPOD& P = PODs[Id];
            const FIntVector BaseCell(
                FMath::FloorToInt(P.Center.X / CellSize),
                FMath::FloorToInt(P.Center.Y / CellSize),
                FMath::FloorToInt(P.Center.Z / CellSize));
            for (int32 Dxi = -1; Dxi <= 1; ++Dxi)
            {
                for (int32 Dyi = -1; Dyi <= 1; ++Dyi)
                {
                    for (int32 Dzi = -1; Dzi <= 1; ++Dzi)
                    {
                        const FIntVector C(BaseCell.X + Dxi, BaseCell.Y + Dyi, BaseCell.Z + Dzi);
                        const TArray<int32>* InCell = CellMap.Find(C);
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

    bool StaticMeshActorHasDatasmithMetadataValue(AStaticMeshActor* StaticMeshActor, const FString& ExactValue)
    {
        if (!StaticMeshActor || ExactValue.IsEmpty())
        {
            return false;
        }

        UStaticMeshComponent* MeshComp = StaticMeshActor->GetStaticMeshComponent();
        if (!MeshComp)
        {
            return false;
        }

        const UDatasmithAssetUserData* DsUserData = Cast<UDatasmithAssetUserData>(
            MeshComp->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass()));
        if (!DsUserData)
        {
            return false;
        }

        for (const TPair<FName, FString>& Item : DsUserData->MetaData)
        {
            if (Item.Value == ExactValue)
            {
                return true;
            }
        }
        return false;
    }

    bool NameArrayContainsTagQuery(const TArray<FName>& InTags, const FString& QueryTrimmed)
    {
        if (QueryTrimmed.IsEmpty())
        {
            return false;
        }
        for (const FName& Tag : InTags)
        {
            if (Tag.ToString().Contains(QueryTrimmed, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    bool ActorOrComponentTagsMatchQuery(AActor* Actor, const FString& QueryTrimmed)
    {
        if (!Actor)
        {
            return false;
        }
        if (NameArrayContainsTagQuery(Actor->Tags, QueryTrimmed))
        {
            return true;
        }
        TArray<UActorComponent*> Comps;
        Actor->GetComponents<UActorComponent>(Comps);
        for (UActorComponent* Comp : Comps)
        {
            if (Comp && NameArrayContainsTagQuery(Comp->ComponentTags, QueryTrimmed))
            {
                return true;
            }
        }
        return false;
    }

    bool ActorPassesTypeFilter(
        AActor* Actor,
        const bool bAllTypes,
        const bool bMesh,
        const bool bSpline,
        const bool bBlueprint,
        const bool bLight,
        const bool bCamera)
    {
        const FEditModelToolTypeFilter TypeFilter{bAllTypes, bMesh, bSpline, bBlueprint, bLight, bCamera};
        return EditModelToolFilterPolicy::ActorPassesTypeFilter(Actor, TypeFilter);
    }

    bool IsEmptySceneOnlyActor(AActor* Actor)
    {
        if (!Actor || !IsValid(Actor))
        {
            return false;
        }

        // Exclude Blueprint-based actors explicitly.
        if (Cast<UBlueprintGeneratedClass>(Actor->GetClass()) != nullptr)
        {
            return false;
        }

        USceneComponent* RootComp = Actor->GetRootComponent();
        if (!RootComp || RootComp->IsEditorOnly())
        {
            return false;
        }

        // Root must be plain USceneComponent (no subclass with content behavior).
        if (RootComp->GetClass() != USceneComponent::StaticClass())
        {
            return false;
        }

        TArray<UActorComponent*> Components;
        Actor->GetComponents<UActorComponent>(Components);

        bool bHasRuntimeSceneComponent = false;
        for (UActorComponent* Comp : Components)
        {
            if (!Comp || Comp->IsEditorOnly())
            {
                continue;
            }

            USceneComponent* SceneComp = Cast<USceneComponent>(Comp);
            if (!SceneComp)
            {
                return false;
            }

            bHasRuntimeSceneComponent = true;

            // Strict mode: only plain USceneComponent is allowed.
            if (SceneComp->GetClass() != USceneComponent::StaticClass())
            {
                return false;
            }
        }

        return bHasRuntimeSceneComponent;
    }

    bool ActorHasContentComponents(AActor* Actor)
    {
        if (!Actor || !IsValid(Actor))
        {
            return false;
        }

        if (Cast<UBlueprintGeneratedClass>(Actor->GetClass()) != nullptr)
        {
            return true;
        }

        TArray<UActorComponent*> Components;
        Actor->GetComponents<UActorComponent>(Components);
        for (UActorComponent* Comp : Components)
        {
            if (!Comp || Comp->IsEditorOnly())
            {
                continue;
            }

            const USceneComponent* SceneComp = Cast<USceneComponent>(Comp);
            if (!SceneComp)
            {
                return true;
            }

            if (SceneComp->GetClass() != USceneComponent::StaticClass())
            {
                return true;
            }
        }

        return false;
    }

    bool AttachedHierarchyHasContent(AActor* RootActor)
    {
        if (!RootActor || !IsValid(RootActor))
        {
            return false;
        }

        TArray<AActor*> AttachedActors;
        RootActor->GetAttachedActors(AttachedActors, true, true);
        for (AActor* Child : AttachedActors)
        {
            if (!Child || !IsValid(Child))
            {
                continue;
            }

            if (ActorHasContentComponents(Child))
            {
                return true;
            }
        }

        return false;
    }

    bool ActorNeedsMovableConversion(AActor* Actor)
    {
        if (!Actor || !IsValid(Actor))
        {
            return false;
        }

        TArray<USceneComponent*> SceneComponents;
        Actor->GetComponents<USceneComponent>(SceneComponents);
        for (const USceneComponent* SceneComp : SceneComponents)
        {
            if (!SceneComp)
            {
                continue;
            }

            if (SceneComp->Mobility != EComponentMobility::Movable)
            {
                return true;
            }
        }

        return false;
    }

    bool ActorMatchesRule(AActor* Actor, const FString& NameContains, const FName& RequiredTag)
    {
        if (!Actor || !IsValid(Actor))
        {
            return false;
        }

        if (!NameContains.IsEmpty())
        {
            const FString Label = Actor->GetActorLabel();
            if (!Label.Contains(NameContains, ESearchCase::IgnoreCase))
            {
                return false;
            }
        }

        if (RequiredTag != NAME_None && !Actor->Tags.Contains(RequiredTag))
        {
            return false;
        }

        return true;
    }

    bool ActorMatchesGlobalRule(AActor* Actor)
    {
        return EditModelToolFilterPolicy::ActorMatchesGlobalRule(Actor, GFilterSettings);
    }

    UStaticMeshComponent* GetFirstStaticMeshComponent(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }

        TArray<UStaticMeshComponent*> MeshComponents;
        Actor->GetComponents<UStaticMeshComponent>(MeshComponents);
        for (UStaticMeshComponent* Comp : MeshComponents)
        {
            if (Comp)
            {
                return Comp;
            }
        }
        return nullptr;
    }
}

void FEditModelToolModule::StartupModule()
{
    bBatchJobRunning = false;
    ActiveBatchJobName.Reset();
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

bool FEditModelToolModule::TryBeginBatchJob(const FText& JobName)
{
    if (bBatchJobRunning)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            LOCTEXT("BatchJobAlreadyRunning", "Another batch job is still running. Please wait for it to finish."));
        return false;
    }

    bBatchJobRunning = true;
    ActiveBatchJobName = JobName.ToString();
    return true;
}

void FEditModelToolModule::EndBatchJob()
{
    bBatchJobRunning = false;
    ActiveBatchJobName.Reset();
}

void FEditModelToolModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);

    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
    FToolMenuSection& Section = Menu->FindOrAddSection("EditModelToolSection");

    Section.AddMenuEntry(
        "OpenEditModelToolDialog",
        LOCTEXT("EditModelToolMenuLabel", "Edit Model Tools (Batch Ops)"),
        LOCTEXT("EditModelToolMenuTooltip", "Open EditModelTool batch operations window."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(this, &FEditModelToolModule::OpenRenameDialog)));
}

void FEditModelToolModule::OpenRenameDialog()
{
    struct FDialogWidgetState
    {
        TSharedPtr<SEditableTextBox> InputBox;
        TSharedPtr<SEditableTextBox> ZOffsetBox;
        TSharedPtr<SEditableTextBox> MetadataValueBox;
        TSharedPtr<SEditableTextBox> TagQueryBox;
        TSharedPtr<SEditableTextBox> TagAddInputBox;
        TSharedPtr<SEditableTextBox> RuleNameContainsBox;
        TSharedPtr<SEditableTextBox> RuleRequiredTagBox;
        TSharedPtr<SEditableTextBox> RuleBatchSizeBox;
    };
    TSharedRef<FDialogWidgetState> DialogState = MakeShared<FDialogWidgetState>();
    TSharedRef<bool> bTagAllTypes = MakeShared<bool>(true);
    TSharedRef<bool> bTagMesh = MakeShared<bool>(false);
    TSharedRef<bool> bTagSpline = MakeShared<bool>(false);
    TSharedRef<bool> bTagBlueprint = MakeShared<bool>(false);
    TSharedRef<bool> bTagLight = MakeShared<bool>(false);
    TSharedRef<bool> bTagCamera = MakeShared<bool>(false);
    TSharedPtr<SWindow> DialogWindow;
    DialogWindow = SNew(SWindow)
        .Title(LOCTEXT("DialogTitle", "Edit Model Tools (Batch Ops)"))
        .SizingRule(ESizingRule::Autosized)
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    const auto RequestCloseDialog = [DialogWindow]()
    {
        if (DialogWindow.IsValid())
        {
            DialogWindow->RequestDestroyWindow();
        }
    };

    const auto BuildNameSection = [this, DialogState]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
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
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                .Padding(0.f, 0.f, 6.f, 0.f)
                [
                    SAssignNew(DialogState->InputBox, SEditableTextBox)
                    .HintText(LOCTEXT("InputPlaceholder", "window"))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("OkButton", "OK"))
                    .OnClicked_Lambda([this, DialogState]()
                    {
                        const FString EnteredName = DialogState->InputBox.IsValid() ? DialogState->InputBox->GetText().ToString() : FString();
                        RenameSelectedActors(EnteredName);
                        return FReply::Handled();
                    })
                ]
            ];
    };

    const auto BuildZSection = [this, DialogState]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ZOffsetHint", "Z offset (world units) — used by both: [Z offset: selection (broad+BC)] and [Auto: all meshes (broad+BC)]:"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                .Padding(0.f, 0.f, 0.f, 0.f)
                [
                    SAssignNew(DialogState->ZOffsetBox, SEditableTextBox)
                    .Text(FText::FromString(TEXT("5000")))
                    .HintText(LOCTEXT("ZOffsetPlaceholder", "5000"))
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("LiftOverlapZButton", "Z offset: selection (broad+BC)"))
                .ToolTipText(LOCTEXT("LiftOverlapZTooltip", "Gathers all static mesh components on the selected outliner actor(s) (you can select many). Uses the same broad-phase + B+C + Z as Auto, but only pairs within that set. The Z value above is used for each successful lift. Need at least two mesh parts in the selection in total."))
                .OnClicked_Lambda([this, DialogState]()
                {
                    float DeltaZ = 0.f;
                    if (!TryParseFloatTextBox(DialogState->ZOffsetBox, DeltaZ, LOCTEXT("BadZValue", "Please enter a valid number for Z offset.")))
                    {
                        return FReply::Handled();
                    }
                    LiftLowerMeshIfTwoOverlap(DeltaZ);
                    return FReply::Handled();
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SButton)
                .Text(LOCTEXT("AutoScanZButton", "Auto: all meshes (broad+BC)"))
                .ToolTipText(LOCTEXT("AutoScanZTooltip", "Scans the whole current level. Broad filter on a background thread, then B+C on the game thread. The Z value is taken from the field above (same as the two-mesh button); e.g. 50 = +50 on Z. Change that number, then press this button. May take a while on very large levels."))
                .OnClicked_Lambda([this, DialogState]()
                {
                    float DeltaZ = 0.f;
                    if (!TryParseFloatTextBox(DialogState->ZOffsetBox, DeltaZ, LOCTEXT("BadZValue2", "Please enter a valid number for Z offset.")))
                    {
                        return FReply::Handled();
                    }
                    AutoScanAllStaticMeshesLiftZAsync(DeltaZ);
                    return FReply::Handled();
                })
            ];
    };

    const auto BuildMetadataSection = [this, DialogState]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("MetadataValueHint", "Metadata exact value (Datasmith User Data):"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                .Padding(0.f, 0.f, 0.f, 0.f)
                [
                    SAssignNew(DialogState->MetadataValueBox, SEditableTextBox)
                    .HintText(LOCTEXT("MetadataValuePlaceholder", "e.g. Door_A01"))
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("SelectByMetadataInSelectionButton", "Select: metadata exact (selection)"))
                .ToolTipText(LOCTEXT("SelectByMetadataInSelectionTooltip", "Reads selected StaticMeshActor(s), checks Datasmith User Data metadata values, and selects actors containing at least one metadata value exactly equal to the text field above."))
                .OnClicked_Lambda([this, DialogState]()
                {
                    const FString ExactValue = GetTrimmedText(DialogState->MetadataValueBox);
                    if (ExactValue.IsEmpty())
                    {
                        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("BadMetadataValueSelection", "Please input a metadata value."));
                        return FReply::Handled();
                    }
                    SelectStaticMeshActorsByDatasmithMetadataFromSelection(ExactValue);
                    return FReply::Handled();
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SButton)
                .Text(LOCTEXT("SelectByMetadataInLevelButton", "Select: metadata exact (all level)"))
                .ToolTipText(LOCTEXT("SelectByMetadataInLevelTooltip", "Scans all StaticMeshActor(s) in the current level, checks Datasmith User Data metadata values, and selects actors containing at least one metadata value exactly equal to the text field above."))
                .OnClicked_Lambda([this, DialogState]()
                {
                    const FString ExactValue = GetTrimmedText(DialogState->MetadataValueBox);
                    if (ExactValue.IsEmpty())
                    {
                        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("BadMetadataValueLevel", "Please input a metadata value."));
                        return FReply::Handled();
                    }
                    SelectStaticMeshActorsByDatasmithMetadataFromLevel(ExactValue);
                    return FReply::Handled();
                })
            ];
    };

    const auto BuildTagSection = [this, DialogState, bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("TagQueryHint", "Tag search (actor + component tags, substring, case-insensitive):"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                .Padding(0.f, 0.f, 6.f, 0.f)
                [
                    SAssignNew(DialogState->TagQueryBox, SEditableTextBox)
                    .HintText(LOCTEXT("TagQueryPlaceholder", "e.g. my_tag"))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SelectByTagInLevelButton", "Select: tag search (whole level)"))
                    .ToolTipText(LOCTEXT(
                        "SelectByTagInLevelTooltip",
                        "Searches the current level: matches the text against each actor's tags and all components' component tags (substring, case-insensitive). Type filters: All types, or any combination of Mesh, Spline, Blueprint, Light, Camera. Unchecking all subtypes reverts to All types."))
                    .OnClicked_Lambda(
                        [this, DialogState, bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera]()
                        {
                            const FString Q = GetTrimmedText(DialogState->TagQueryBox);
                            if (Q.IsEmpty())
                            {
                                FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagQueryEmpty", "Please enter text to search in tags."));
                                return FReply::Handled();
                            }
                            SearchLevelByTagsAndSelect(
                                Q,
                                *bTagAllTypes,
                                *bTagMesh,
                                *bTagSpline,
                                *bTagBlueprint,
                                *bTagLight,
                                *bTagCamera);
                            return FReply::Handled();
                        })
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("TagAddHint", "Add tag(s) to current selection (comma or semicolon; types use checkboxes below):"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                .Padding(0.f, 0.f, 6.f, 0.f)
                [
                    SAssignNew(DialogState->TagAddInputBox, SEditableTextBox)
                    .HintText(LOCTEXT("TagAddPlaceholder", "e.g. my_tag, other_tag"))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("AddTagsToSelectionButton", "Add tag(s) to selection"))
                    .ToolTipText(LOCTEXT(
                        "AddTagsToSelectionTooltip",
                        "For each object in the outliner selection that matches the type filter (All / Mesh / Spline / Blueprint / Light / Camera), appends the tag(s) from the [Add tag(s)…] field to that actor's Actor Tags (FName, AddUnique). Multiple tags: separate with comma or semicolon. Selection must contain at least one actor."))
                    .OnClicked_Lambda(
                        [this, DialogState, bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera]()
                        {
                            const FString Raw = GetTrimmedText(DialogState->TagAddInputBox);
                            if (Raw.IsEmpty())
                            {
                                FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagAddInputEmpty", "Please enter at least one tag to add."));
                                return FReply::Handled();
                            }
                            AddTagsToSelectedActors(
                                Raw,
                                *bTagAllTypes,
                                *bTagMesh,
                                *bTagSpline,
                                *bTagBlueprint,
                                *bTagLight,
                                *bTagCamera);
                            return FReply::Handled();
                        })
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 4.f, 0.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("RemoveTagsFromSelectionButton", "Remove tag(s) from selection"))
                .ToolTipText(LOCTEXT(
                    "RemoveTagsFromSelectionTooltip",
                    "For each object in the outliner selection that matches the type filter (All / Mesh / Spline / Blueprint / Light / Camera), removes the tag(s) from the [Add tag(s)…] field from that actor's Actor Tags. Multiple tags: separate with comma or semicolon. Selection must contain at least one actor."))
                .OnClicked_Lambda(
                    [this, DialogState, bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera]()
                    {
                        const FString Raw = GetTrimmedText(DialogState->TagAddInputBox);
                        if (Raw.IsEmpty())
                        {
                            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagRemoveInputEmpty", "Please enter at least one tag to remove."));
                            return FReply::Handled();
                        }
                        RemoveTagsFromSelectedActors(
                            Raw,
                            *bTagAllTypes,
                            *bTagMesh,
                            *bTagSpline,
                            *bTagBlueprint,
                            *bTagLight,
                            *bTagCamera);
                        return FReply::Handled();
                    })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 4.f, 0.f, 0.f)
            [
                SNew(SUniformGridPanel)
                .MinDesiredSlotWidth(120.f)
                .SlotPadding(2.f)
                + SUniformGridPanel::Slot(0, 0)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda(
                        [bTagAllTypes]()
                        {
                            return *bTagAllTypes ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                    .OnCheckStateChanged_Lambda(
                        [bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera](const ECheckBoxState InState)
                        {
                            *bTagAllTypes = (InState == ECheckBoxState::Checked);
                            if (*bTagAllTypes)
                            {
                                *bTagMesh = *bTagSpline = *bTagBlueprint = *bTagLight = *bTagCamera = false;
                            }
                        })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("TagTypeAll", "All (any actor)"))
                    ]
                ]
                + SUniformGridPanel::Slot(1, 0)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda(
                        [bTagMesh]()
                        {
                            return *bTagMesh ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                    .OnCheckStateChanged_Lambda(
                        [bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera](const ECheckBoxState InState)
                        {
                            *bTagMesh = (InState == ECheckBoxState::Checked);
                            if (*bTagMesh)
                            {
                                *bTagAllTypes = false;
                            }
                            if (!*bTagMesh && !*bTagSpline && !*bTagBlueprint && !*bTagLight && !*bTagCamera)
                            {
                                *bTagAllTypes = true;
                            }
                        })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("TagTypeMesh", "Mesh"))
                    ]
                ]
                + SUniformGridPanel::Slot(2, 0)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda(
                        [bTagSpline]()
                        {
                            return *bTagSpline ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                    .OnCheckStateChanged_Lambda(
                        [bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera](const ECheckBoxState InState)
                        {
                            *bTagSpline = (InState == ECheckBoxState::Checked);
                            if (*bTagSpline)
                            {
                                *bTagAllTypes = false;
                            }
                            if (!*bTagMesh && !*bTagSpline && !*bTagBlueprint && !*bTagLight && !*bTagCamera)
                            {
                                *bTagAllTypes = true;
                            }
                        })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("TagTypeSpline", "Spline"))
                    ]
                ]
                + SUniformGridPanel::Slot(0, 1)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda(
                        [bTagBlueprint]()
                        {
                            return *bTagBlueprint ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                    .OnCheckStateChanged_Lambda(
                        [bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera](const ECheckBoxState InState)
                        {
                            *bTagBlueprint = (InState == ECheckBoxState::Checked);
                            if (*bTagBlueprint)
                            {
                                *bTagAllTypes = false;
                            }
                            if (!*bTagMesh && !*bTagSpline && !*bTagBlueprint && !*bTagLight && !*bTagCamera)
                            {
                                *bTagAllTypes = true;
                            }
                        })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("TagTypeBlueprint", "Blueprint"))
                    ]
                ]
                + SUniformGridPanel::Slot(1, 1)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda(
                        [bTagLight]()
                        {
                            return *bTagLight ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                    .OnCheckStateChanged_Lambda(
                        [bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera](const ECheckBoxState InState)
                        {
                            *bTagLight = (InState == ECheckBoxState::Checked);
                            if (*bTagLight)
                            {
                                *bTagAllTypes = false;
                            }
                            if (!*bTagMesh && !*bTagSpline && !*bTagBlueprint && !*bTagLight && !*bTagCamera)
                            {
                                *bTagAllTypes = true;
                            }
                        })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("TagTypeLight", "Light"))
                    ]
                ]
                + SUniformGridPanel::Slot(2, 1)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda(
                        [bTagCamera]()
                        {
                            return *bTagCamera ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                    .OnCheckStateChanged_Lambda(
                        [bTagAllTypes, bTagMesh, bTagSpline, bTagBlueprint, bTagLight, bTagCamera](const ECheckBoxState InState)
                        {
                            *bTagCamera = (InState == ECheckBoxState::Checked);
                            if (*bTagCamera)
                            {
                                *bTagAllTypes = false;
                            }
                            if (!*bTagMesh && !*bTagSpline && !*bTagBlueprint && !*bTagLight && !*bTagCamera)
                            {
                                *bTagAllTypes = true;
                            }
                        })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("TagTypeCamera", "Camera"))
                    ]
                ]
            ];
    };

    const auto BuildActionSection = [this, RequestCloseDialog]() -> TSharedRef<SWidget>
    {
        return SNew(SUniformGridPanel)
            .SlotPadding(4.f)
            + SUniformGridPanel::Slot(0, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("CancelButton", "Cancel"))
                .OnClicked_Lambda([RequestCloseDialog]()
                {
                    RequestCloseDialog();
                    return FReply::Handled();
                })
            ]
            + SUniformGridPanel::Slot(1, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("SetMovableButton", "Set Meshes Movable"))
                .OnClicked_Lambda([this]()
                {
                    SetSelectedMeshesMovable();
                    return FReply::Handled();
                })
            ]
            + SUniformGridPanel::Slot(0, 1)
            [
                SNew(SButton)
                .Text(LOCTEXT("GroupUnderNewActorButton", "Movable + Group To New Actor"))
                .OnClicked_Lambda([this]()
                {
                    GroupSelectedActorsUnderNewActor();
                    return FReply::Handled();
                })
            ]
            + SUniformGridPanel::Slot(1, 1)
            [
                SNew(SButton)
                .Text(LOCTEXT("SelectEmptySceneOnlyButton", "Select Empty Actors (Scene Only)"))
                .ToolTipText(LOCTEXT("SelectEmptySceneOnlyTooltip", "Scan the whole current level and select actors that are not Blueprint actors and contain only scene components (no mesh/light/camera/spline or other content components)."))
                .OnClicked_Lambda([this]()
                {
                    SelectEmptySceneOnlyActorsFromLevel();
                    return FReply::Handled();
                })
            ]
            + SUniformGridPanel::Slot(0, 2)
            [
                SNew(SButton)
                .Text(LOCTEXT("CopyMaterialsFromFirstSelectedButton", "Copy Materials From First Selected Mesh"))
                .ToolTipText(LOCTEXT("CopyMaterialsFromFirstSelectedTooltip", "Use first selected actor as source and copy material element 0/1/2... to all other selected actors' mesh components by matching element index."))
                .OnClicked_Lambda([this]()
                {
                    CopyMaterialsFromFirstSelectedMeshByElement();
                    return FReply::Handled();
                })
            ]
            + SUniformGridPanel::Slot(1, 2)
            [
                SNew(SButton)
                .Text(LOCTEXT("CopyMaterialsFromLastSelectedButton", "Copy Materials From Last Selected Mesh"))
                .ToolTipText(LOCTEXT("CopyMaterialsFromLastSelectedTooltip", "Use last selected actor as source and copy material element 0/1/2... to all other selected actors' mesh components by matching element index."))
                .OnClicked_Lambda([this]()
                {
                    CopyMaterialsFromLastSelectedMeshByElement();
                    return FReply::Handled();
                })
            ]
            ;
    };

    const auto BuildRuleBatchSection = [this, DialogState]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("RuleBatchHint", "Global filters (empty = ignore): apply to rename/movable/group/material/overlap/metadata/empty-scan. Tag add/remove/search use type filter only."))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 4.f, 0.f, 4.f)
            [
                SAssignNew(DialogState->RuleNameContainsBox, SEditableTextBox)
                .OnTextChanged_Lambda([](const FText& NewText)
                {
                    GFilterSettings.NameContains = NewText.ToString().TrimStartAndEnd();
                })
                .HintText(LOCTEXT("RuleNameContainsPlaceholder", "Name contains (optional), e.g. Wall_"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 4.f)
            [
                SAssignNew(DialogState->RuleRequiredTagBox, SEditableTextBox)
                .OnTextChanged_Lambda([](const FText& NewText)
                {
                    GFilterSettings.RequiredTag = NewText.ToString().TrimStartAndEnd();
                })
                .HintText(LOCTEXT("RuleTagPlaceholder", "Required actor tag (optional), e.g. LOD_A"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SAssignNew(DialogState->RuleBatchSizeBox, SEditableTextBox)
                .Text(FText::FromString(TEXT("32")))
                .OnTextChanged_Lambda([](const FText& NewText)
                {
                    const FString Trimmed = NewText.ToString().TrimStartAndEnd();
                    if (Trimmed.IsEmpty())
                    {
                        GFilterSettings.ChunkSize = 32;
                        return;
                    }
                    int32 ParsedValue = 0;
                    if (FDefaultValueHelper::ParseInt(Trimmed, ParsedValue) && ParsedValue > 0)
                    {
                        GFilterSettings.ChunkSize = ParsedValue;
                    }
                })
                .HintText(LOCTEXT("RuleBatchSizePlaceholder", "Batch size per tick (16/32/64/128), default 32"))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 4.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("RuleMovableButton", "Run Rule+Chunk: Set Meshes Movable"))
                .ToolTipText(LOCTEXT("RuleMovableTooltip", "From current selection, apply Name contains + required tag filter, then set matching mesh components to Movable in chunks. Single-actor failures are isolated and listed at the end."))
                .OnClicked_Lambda([this, DialogState]()
                {
                    int32 BatchSize = 0;
                    if (!TryParseIntTextBox(DialogState->RuleBatchSizeBox, BatchSize, LOCTEXT("BadRuleBatchSizeMovable", "Please enter a valid positive integer for batch size.")))
                    {
                        return FReply::Handled();
                    }

                    GFilterSettings.ChunkSize = BatchSize;
                    RunRuleBasedMovableChunked(GFilterSettings.NameContains, GFilterSettings.RequiredTag, GFilterSettings.ChunkSize);
                    return FReply::Handled();
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SButton)
                .Text(LOCTEXT("RuleCopyMaterialsButton", "Run Rule+Chunk: Copy Materials (From Last Selected)"))
                .ToolTipText(LOCTEXT("RuleCopyMaterialsTooltip", "Use last selected actor as source. Then from current selection, apply Name contains + required tag filter to targets and copy materials by slot in chunks. Single-actor failures are isolated and listed at the end."))
                .OnClicked_Lambda([this, DialogState]()
                {
                    int32 BatchSize = 0;
                    if (!TryParseIntTextBox(DialogState->RuleBatchSizeBox, BatchSize, LOCTEXT("BadRuleBatchSizeCopy", "Please enter a valid positive integer for batch size.")))
                    {
                        return FReply::Handled();
                    }

                    GFilterSettings.ChunkSize = BatchSize;
                    RunRuleBasedCopyMaterialsChunked(GFilterSettings.NameContains, GFilterSettings.RequiredTag, GFilterSettings.ChunkSize);
                    return FReply::Handled();
                })
            ];
    };

    DialogWindow->SetContent(
        SNew(SBox)
        .Padding(12.f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                BuildNameSection()
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                BuildZSection()
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                BuildMetadataSection()
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                BuildTagSection()
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                BuildRuleBatchSection()
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 12.f, 0.f, 0.f)
            .HAlign(HAlign_Right)
            [
                BuildActionSection()
            ]
        ]);

    FSlateApplication::Get().AddWindow(DialogWindow.ToSharedRef());

}

void FEditModelToolModule::SelectStaticMeshActorsByDatasmithMetadataFromSelection(const FString& ExactValue)
{
    const FString Query = ExactValue.TrimStartAndEnd();
    if (Query.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MetadataSelectionEmptyValue", "Metadata value is empty."));
        return;
    }

    USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
    if (!SelectedActors || SelectedActors->Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MetadataSelectionNoSelection", "No actors selected in outliner."));
        return;
    }

    TArray<AStaticMeshActor*> Matches;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(*It);
        if (!StaticMeshActor)
        {
            continue;
        }
        if (!ActorMatchesGlobalRule(StaticMeshActor))
        {
            continue;
        }
        if (StaticMeshActorHasDatasmithMetadataValue(StaticMeshActor, Query))
        {
            Matches.Add(StaticMeshActor);
        }
    }

    GEditor->SelectNone(true, true, false);
    for (AStaticMeshActor* Actor : Matches)
    {
        GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::Format(
            LOCTEXT("MetadataSelectionResultSelectionFmt", "Selection scan complete.\nExact metadata value: \"{0}\"\nMatched StaticMeshActor count: {1}."),
            FText::FromString(Query),
            FText::AsNumber(Matches.Num())));
}

void FEditModelToolModule::SelectStaticMeshActorsByDatasmithMetadataFromLevel(const FString& ExactValue)
{
    const FString Query = ExactValue.TrimStartAndEnd();
    if (Query.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MetadataLevelEmptyValue", "Metadata value is empty."));
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MetadataLevelNoWorld", "Could not get editor world."));
        return;
    }

    TArray<AStaticMeshActor*> Matches;
    for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
    {
        AStaticMeshActor* StaticMeshActor = *It;
        if (!StaticMeshActor || !IsValid(StaticMeshActor))
        {
            continue;
        }
        if (!ActorMatchesGlobalRule(StaticMeshActor))
        {
            continue;
        }
        if (StaticMeshActorHasDatasmithMetadataValue(StaticMeshActor, Query))
        {
            Matches.Add(StaticMeshActor);
        }
    }

    GEditor->SelectNone(true, true, false);
    for (AStaticMeshActor* Actor : Matches)
    {
        GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::Format(
            LOCTEXT("MetadataSelectionResultLevelFmt", "Full level scan complete.\nExact metadata value: \"{0}\"\nMatched StaticMeshActor count: {1}."),
            FText::FromString(Query),
            FText::AsNumber(Matches.Num())));
}

void FEditModelToolModule::SearchLevelByTagsAndSelect(
    const FString& Query,
    const bool bAllTypes,
    const bool bMesh,
    const bool bSpline,
    const bool bBlueprint,
    const bool bLight,
    const bool bCamera)
{
    const FString Q = Query.TrimStartAndEnd();
    if (Q.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagLevelEmpty", "Tag search text is empty."));
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagLevelNoWorld", "Could not get editor world."));
        return;
    }

    TArray<AActor*> Matches;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !IsValid(Actor))
        {
            continue;
        }
        if (!ActorOrComponentTagsMatchQuery(Actor, Q))
        {
            continue;
        }
        if (!ActorPassesTypeFilter(Actor, bAllTypes, bMesh, bSpline, bBlueprint, bLight, bCamera))
        {
            continue;
        }
        Matches.Add(Actor);
    }

    GEditor->SelectNone(true, true, false);
    for (AActor* Actor : Matches)
    {
        GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::Format(
            LOCTEXT(
                "TagLevelResultFmt",
                "Tag search complete.\nQuery: \"{0}\" (substring in actor tags or any component's component tags)\nMatched actor count: {1}."),
            FText::FromString(Q),
            FText::AsNumber(Matches.Num())));
}

void FEditModelToolModule::SelectEmptySceneOnlyActorsFromLevel()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SelectEmptySceneOnlyNoWorld", "Could not get editor world."));
        return;
    }

    TArray<AActor*> Matches;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!ActorMatchesGlobalRule(Actor))
        {
            continue;
        }
        if (!IsEmptySceneOnlyActor(Actor))
        {
            continue;
        }

        // Outliner root must also be "empty" for all attached descendants.
        if (AttachedHierarchyHasContent(Actor))
        {
            continue;
        }

        if (Actor)
        {
            Matches.Add(Actor);
        }
    }

    GEditor->SelectNone(true, true, false);
    for (AActor* Actor : Matches)
    {
        GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::Format(
            LOCTEXT("SelectEmptySceneOnlyResultFmt", "Empty actor scan complete.\nCriteria: actor itself is non-Blueprint with root/all runtime components exactly USceneComponent, and attached descendants recursively contain no content components.\nMatched actor count: {0}."),
            FText::AsNumber(Matches.Num())));
}

void FEditModelToolModule::AddTagsToSelectedActors(
    const FString& TagInput,
    const bool bAllTypes,
    const bool bMesh,
    const bool bSpline,
    const bool bBlueprint,
    const bool bLight,
    const bool bCamera)
{
    TArray<FName> TagNames;
    if (!EditModelToolTagOperations::ParseTagList(TagInput, TagNames))
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagAddNoValidTags", "No valid tags after parsing (use comma or semicolon between names)."));
        return;
    }

    TArray<AActor*> SelectedActorList;
    EditModelToolSelectionUtils::GatherSelectedActors(SelectedActorList);
    if (SelectedActorList.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagAddNoSelection", "No actors selected in outliner."));
        return;
    }
    const FEditModelToolTypeFilter TypeFilter{bAllTypes, bMesh, bSpline, bBlueprint, bLight, bCamera};
    const FScopedTransaction Transaction(LOCTEXT("AddTagsToSelectedTx", "Add tags to selected actors"));
    const FEditModelToolTagMutationResult Result = EditModelToolTagOperations::ApplyTagMutation(
        SelectedActorList,
        TagNames,
        EEditModelToolTagMutation::Add,
        [&TypeFilter](AActor* Actor)
        {
            return EditModelToolFilterPolicy::ActorPassesTypeFilter(Actor, TypeFilter);
        });

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::Format(
            LOCTEXT("TagAddResultFmt", "Add tags to selection complete.\nInput tags: {0}\nActors updated (new tag(s) added): {1}\nActors already had all tag(s): {2}\nAdded tag assignments: {3}."),
            FText::AsNumber(TagNames.Num()),
            FText::AsNumber(Result.UpdatedActors),
            FText::AsNumber(Result.UnchangedActors),
            FText::AsNumber(Result.TagAssignmentsChanged)));
}

void FEditModelToolModule::RemoveTagsFromSelectedActors(
    const FString& TagInput,
    const bool bAllTypes,
    const bool bMesh,
    const bool bSpline,
    const bool bBlueprint,
    const bool bLight,
    const bool bCamera)
{
    TArray<FName> TagNames;
    if (!EditModelToolTagOperations::ParseTagList(TagInput, TagNames))
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagRemoveNoValidTags", "No valid tags after parsing (use comma or semicolon between names)."));
        return;
    }

    TArray<AActor*> SelectedActorList;
    EditModelToolSelectionUtils::GatherSelectedActors(SelectedActorList);
    if (SelectedActorList.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TagRemoveNoSelection", "No actors selected in outliner."));
        return;
    }
    const FEditModelToolTypeFilter TypeFilter{bAllTypes, bMesh, bSpline, bBlueprint, bLight, bCamera};
    const FScopedTransaction Transaction(LOCTEXT("RemoveTagsFromSelectedTx", "Remove tags from selected actors"));
    const FEditModelToolTagMutationResult Result = EditModelToolTagOperations::ApplyTagMutation(
        SelectedActorList,
        TagNames,
        EEditModelToolTagMutation::Remove,
        [&TypeFilter](AActor* Actor)
        {
            return EditModelToolFilterPolicy::ActorPassesTypeFilter(Actor, TypeFilter);
        });

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::Format(
            LOCTEXT("TagRemoveResultFmt", "Remove tags from selection complete.\nInput tags: {0}\nActors updated (tag(s) removed): {1}\nActors had none of those tag(s): {2}\nRemoved tag assignments: {3}."),
            FText::AsNumber(TagNames.Num()),
            FText::AsNumber(Result.UpdatedActors),
            FText::AsNumber(Result.UnchangedActors),
            FText::AsNumber(Result.TagAssignmentsChanged)));
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
            if (!ActorMatchesGlobalRule(Actor))
            {
                continue;
            }
            ActorsToGroup.Add(Actor);
        }
    }

    if (ActorsToGroup.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoValidActorsForGroup", "No valid actors selected."));
        return;
    }

    FActorSpawnParameters SpawnParams;
    const FString UniqueRootSeed = FString::Printf(TEXT("GroupRoot_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    SpawnParams.Name = MakeUniqueObjectName(EditorWorld->PersistentLevel, AActor::StaticClass(), FName(*UniqueRootSeed));
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

    struct FGroupBatchState
    {
        TArray<TWeakObjectPtr<AActor>> Targets;
        TWeakObjectPtr<AActor> Parent;
        int32 Index = 0;
        int32 MobilityChangedCount = 0;
        int32 AttachedCount = 0;
        bool bMobilityPhaseDone = false;
        TSharedPtr<FScopedTransaction> Transaction;
    };

    TSharedRef<FGroupBatchState> State = MakeShared<FGroupBatchState>();
    State->Targets.Reserve(ActorsToGroup.Num());
    for (AActor* Actor : ActorsToGroup)
    {
        State->Targets.Add(Actor);
    }
    State->Parent = ParentActor;
    State->Transaction = MakeShared<FScopedTransaction>(LOCTEXT("GroupSelectedActorsTransaction", "Group Selected Actors Under New Actor"));

    const int32 BatchSizePerTick = EditModelToolFilterPolicy::GetEffectiveChunkSize(GFilterSettings);
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State, BatchSizePerTick](float)
    {
        if (!State->Parent.IsValid())
        {
            State->Transaction.Reset();
            return false;
        }

        AActor* Parent = State->Parent.Get();
        const int32 EndIndex = FMath::Min(State->Index + BatchSizePerTick, State->Targets.Num());
        for (; State->Index < EndIndex; ++State->Index)
        {
            AActor* Actor = State->Targets[State->Index].Get();
            if (!Actor || !IsValid(Actor) || Actor == Parent)
            {
                continue;
            }

            Actor->Modify();

            if (!State->bMobilityPhaseDone)
            {
                if (!ActorNeedsMovableConversion(Actor))
                {
                    continue;
                }

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
                        ++State->MobilityChangedCount;
                    }
                }
            }
            else if (GEditor && GEditor->CanParentActors(Parent, Actor))
            {
                GEditor->ParentActors(Parent, Actor, NAME_None);
                ++State->AttachedCount;
            }
        }

        if (State->Index < State->Targets.Num())
        {
            return true;
        }

        if (!State->bMobilityPhaseDone)
        {
            State->bMobilityPhaseDone = true;
            State->Index = 0;
            return true;
        }

        if (GEditor)
        {
            GEditor->SelectNone(true, true, false);
            GEditor->SelectActor(Parent, true, true, true);
        }

        if (State->AttachedCount == 0)
        {
            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoActorAttached", "No selected actors could be attached to the new actor."));
        }
        else if (State->MobilityChangedCount == 0)
        {
            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("GroupedNoMobilityChange", "Grouped selected actors under new actor at (0,0,0). Mobility was already movable."));
        }
        else
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::Format(
                    LOCTEXT("GroupedBatchDone", "Grouped complete.\nAttached actors: {0}\nComponents set to movable: {1}."),
                    FText::AsNumber(State->AttachedCount),
                    FText::AsNumber(State->MobilityChangedCount)));
        }

        State->Transaction.Reset();
        return false;
    }));
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
        if (!ActorMatchesGlobalRule(Actor))
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
        if (!ActorMatchesGlobalRule(Actor))
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
        if (!ActorMatchesGlobalRule(Actor))
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

void FEditModelToolModule::CopyMaterialsFromFirstSelectedMeshByElement()
{
    CopyMaterialsFromSelectedMeshByElement(
        0,
        LOCTEXT("NeedAtLeastTwoActorsForMaterialCopy", "Please select at least two actors. The first selected actor is the source."),
        LOCTEXT("FirstSelectedSourceLabel", "first"),
        LOCTEXT("FirstSelectedUsedLabel", "first"),
        LOCTEXT("CopyMaterialsFromFirstSelectedTransaction", "Copy Materials From First Selected Mesh"),
        LOCTEXT("NoMaterialSlotsUpdated", "No material slot values were changed."),
        LOCTEXT("MaterialCopyResultFmt", "Material copy complete.\nSource used: {2}\nUpdated components: {0}\nUpdated material slots: {1}"));
}

void FEditModelToolModule::CopyMaterialsFromLastSelectedMeshByElement()
{
    CopyMaterialsFromSelectedMeshByElement(
        INDEX_NONE,
        LOCTEXT("NeedAtLeastTwoActorsForMaterialCopyFromLast", "Please select at least two actors. The last selected actor is the source."),
        LOCTEXT("LastSelectedSourceLabel", "last"),
        LOCTEXT("LastSelectedUsedLabel", "last"),
        LOCTEXT("CopyMaterialsFromLastSelectedTransaction", "Copy Materials From Last Selected Mesh"),
        LOCTEXT("NoMaterialSlotsUpdatedFromLast", "No material slot values were changed."),
        LOCTEXT("MaterialCopyFromLastResultFmt", "Material copy complete.\nSource used: {2}\nUpdated components: {0}\nUpdated material slots: {1}"));
}

void FEditModelToolModule::CopyMaterialsFromSelectedMeshByElement(
    const int32 SourceActorIndex,
    const FText& NotEnoughSelectionMessage,
    const FText& SourceSelectorLabel,
    const FText& SourceUsedLabel,
    const FText& TransactionText,
    const FText& NoChangesText,
    const FText& ResultTextFormat)
{
    USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
    if (!SelectedActors || SelectedActors->Num() < 2)
    {
        FMessageDialog::Open(EAppMsgType::Ok, NotEnoughSelectionMessage);
        return;
    }

    TArray<AActor*> OrderedActors;
    OrderedActors.Reserve(SelectedActors->Num());
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (AActor* Actor = Cast<AActor>(*It))
        {
            OrderedActors.Add(Actor);
        }
    }

    if (OrderedActors.Num() < 2)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NeedAtLeastTwoValidActorsForMaterialCopyShared", "Need at least two valid selected actors."));
        return;
    }

    const int32 ResolvedSourceIndex = (SourceActorIndex == INDEX_NONE)
        ? (OrderedActors.Num() - 1)
        : FMath::Clamp(SourceActorIndex, 0, OrderedActors.Num() - 1);

    AActor* SourceActor = OrderedActors[ResolvedSourceIndex];
    if (!SourceActor)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("InvalidSourceActorForMaterialCopyShared", "Could not determine the {0} selected actor as source."),
            SourceSelectorLabel));
        return;
    }

    TArray<UStaticMeshComponent*> SourceMeshComponents;
    SourceActor->GetComponents<UStaticMeshComponent>(SourceMeshComponents);
    if (SourceMeshComponents.Num() == 0 || !SourceMeshComponents[0])
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SourceHasNoStaticMeshForMaterialCopyShared", "Source actor has no valid StaticMeshComponent."));
        return;
    }

    UStaticMeshComponent* SourceMeshComponent = SourceMeshComponents[0];
    const int32 SourceMaterialCount = SourceMeshComponent->GetNumMaterials();
    if (SourceMaterialCount <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SourceHasNoMaterialsForMaterialCopyShared", "Source mesh has no material slots."));
        return;
    }

    const FScopedTransaction Transaction(TransactionText);

    int32 UpdatedComponentCount = 0;
    int32 UpdatedSlotCount = 0;
    for (int32 ActorIndex = 0; ActorIndex < OrderedActors.Num(); ++ActorIndex)
    {
        if (ActorIndex == ResolvedSourceIndex)
        {
            continue;
        }

        AActor* TargetActor = OrderedActors[ActorIndex];
        if (!TargetActor)
        {
            continue;
        }

        TArray<UStaticMeshComponent*> TargetMeshComponents;
        TargetActor->GetComponents<UStaticMeshComponent>(TargetMeshComponents);

        for (UStaticMeshComponent* TargetMeshComponent : TargetMeshComponents)
        {
            if (!TargetMeshComponent)
            {
                continue;
            }

            const FEditModelToolMaterialCopyResult CopyResult =
                EditModelToolMaterialService::CopyMaterialsByElement(SourceMeshComponent, TargetMeshComponent);
            UpdatedComponentCount += CopyResult.UpdatedComponents;
            UpdatedSlotCount += CopyResult.UpdatedSlots;
        }
    }

    if (UpdatedSlotCount == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, NoChangesText);
    }
    else
    {
        const FText ResultText = FText::Format(
            ResultTextFormat,
            FText::AsNumber(UpdatedComponentCount),
            FText::AsNumber(UpdatedSlotCount),
            SourceUsedLabel);
        FMessageDialog::Open(EAppMsgType::Ok, ResultText);
    }
}

void FEditModelToolModule::RunRuleBasedMovableChunked(const FString& NameContains, const FString& RequiredTag, const int32 BatchSize)
{
    if (!TryBeginBatchJob(LOCTEXT("RuleMovableJobName", "Rule-based Set Meshes Movable")))
    {
        return;
    }

    TArray<AActor*> SelectedActorList;
    EditModelToolSelectionUtils::GatherSelectedActors(SelectedActorList);
    if (SelectedActorList.Num() == 0)
    {
        EndBatchJob();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RuleMovableNoSelection", "No actors selected in outliner."));
        return;
    }

    const FName RequiredTagName = RequiredTag.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*RequiredTag.TrimStartAndEnd());

    TArray<TWeakObjectPtr<AActor>> Targets;
    Targets.Reserve(SelectedActorList.Num());
    for (AActor* Actor : SelectedActorList)
    {
        if (ActorMatchesRule(Actor, NameContains, RequiredTagName))
        {
            Targets.Add(Actor);
        }
    }

    if (Targets.Num() == 0)
    {
        EndBatchJob();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RuleMovableNoRuleMatch", "No selected actors match current rule filter."));
        return;
    }

    struct FMovableBatchState
    {
        TArray<TWeakObjectPtr<AActor>> Actors;
        int32 UpdatedComponents = 0;
        int32 UpdatedActors = 0;
        TArray<FString> Failures;
        TSharedPtr<SNotificationItem> Notification;
        TSharedPtr<FScopedTransaction> Transaction;
    };

    TSharedRef<FMovableBatchState> State = MakeShared<FMovableBatchState>();
    State->Actors = MoveTemp(Targets);
    FEditModelToolBatchContext Context;
    Context.TotalItems = State->Actors.Num();
    Context.BatchSize = BatchSize;

    FEditModelToolBatchHooks Hooks;
    Hooks.OnStart = [State](FEditModelToolBatchContext&)
    {
        State->Notification = EditModelToolNotifications::ShowProgress(LOCTEXT("RuleMovableProgressTitle", "Rule-based movable is running"));
        State->Transaction = MakeShared<FScopedTransaction>(LOCTEXT("RuleMovableSingleTx", "Rule-based Set Meshes Movable"));
    };
    Hooks.OnChunk = [State](FEditModelToolBatchContext&, const int32 StartIndex, const int32 EndIndex)
    {
        for (int32 Index = StartIndex; Index < EndIndex; ++Index)
        {
            AActor* Actor = State->Actors[Index].Get();
            if (!Actor || !IsValid(Actor))
            {
                State->Failures.Add(FString::Printf(TEXT("Index %d: invalid actor."), Index));
                continue;
            }

            TArray<UStaticMeshComponent*> MeshComponents;
            Actor->GetComponents<UStaticMeshComponent>(MeshComponents);
            if (MeshComponents.Num() == 0)
            {
                State->Failures.Add(FString::Printf(TEXT("%s: no static mesh component."), *Actor->GetActorLabel()));
                continue;
            }

            int32 UpdatedInActor = 0;
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
                    ++UpdatedInActor;
                }
            }

            if (UpdatedInActor > 0)
            {
                ++State->UpdatedActors;
                State->UpdatedComponents += UpdatedInActor;
            }
        }
    };
    Hooks.OnProgress = [State](FEditModelToolBatchContext&, const int32 Processed, const int32 Total, const double EtaSec)
    {
        UE_LOG(
            LogTemp,
            Display,
            TEXT("[EditModelTool][RuleMovable] Progress %d/%d, ETA %.1fs"),
            Processed,
            Total,
            EtaSec);
        EditModelToolNotifications::UpdateProgress(
            State->Notification,
            FText::Format(LOCTEXT("RuleMovableProgressFmt", "Rule movable {0}/{1} (ETA {2}s)"), FText::AsNumber(Processed), FText::AsNumber(Total), FText::AsNumber(FMath::RoundToInt(EtaSec))),
            Total > 0 ? static_cast<float>(Processed) / static_cast<float>(Total) : 1.0f);
    };
    Hooks.OnComplete = [this, State](FEditModelToolBatchContext& ContextArg)
    {
        State->Transaction.Reset();
        FString FailurePreview;
        const int32 FailurePreviewCount = FMath::Min(8, State->Failures.Num());
        for (int32 i = 0; i < FailurePreviewCount; ++i)
        {
            FailurePreview += FString::Printf(TEXT("\n- %s"), *State->Failures[i]);
        }
        if (State->Failures.Num() > FailurePreviewCount)
        {
            FailurePreview += FString::Printf(TEXT("\n...and %d more"), State->Failures.Num() - FailurePreviewCount);
        }
        for (const FString& Failure : State->Failures)
        {
            UE_LOG(LogTemp, Warning, TEXT("[EditModelTool][RuleMovable][Failure] %s"), *Failure);
        }

        EditModelToolNotifications::Complete(
            State->Notification,
            LOCTEXT("RuleMovableComplete", "Rule-based movable finished"),
            true);

        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(FString::Printf(
                TEXT("Rule-based movable finished.\nMatched actors: %d\nUpdated actors: %d\nUpdated components: %d\nFailures: %d%s"),
                ContextArg.TotalItems,
                State->UpdatedActors,
                State->UpdatedComponents,
                State->Failures.Num(),
                *FailurePreview)));
        EndBatchJob();
    };
    FEditModelToolBatchRunner::Start(Hooks, MoveTemp(Context));
}

void FEditModelToolModule::RunRuleBasedCopyMaterialsChunked(const FString& NameContains, const FString& RequiredTag, const int32 BatchSize)
{
    if (!TryBeginBatchJob(LOCTEXT("RuleCopyJobName", "Rule-based Copy Materials")))
    {
        return;
    }

    TArray<AActor*> OrderedActors;
    EditModelToolSelectionUtils::GatherSelectedActors(OrderedActors);
    if (OrderedActors.Num() < 2)
    {
        EndBatchJob();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RuleCopyNoSelection", "Please select at least two actors."));
        return;
    }

    AActor* SourceActor = OrderedActors.Last();
    const FText SourceUsedText = LOCTEXT("RuleCopySourceUsedLast", "last");

    UStaticMeshComponent* SourceMeshComponent = GetFirstStaticMeshComponent(SourceActor);
    if (!SourceMeshComponent)
    {
        EndBatchJob();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RuleCopyInvalidSourceMesh", "Source actor has no valid static mesh component."));
        return;
    }

    const int32 SourceMaterialCount = SourceMeshComponent->GetNumMaterials();
    if (SourceMaterialCount <= 0)
    {
        EndBatchJob();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RuleCopyNoSourceSlots", "Source mesh has no material slots."));
        return;
    }

    const FName RequiredTagName = RequiredTag.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*RequiredTag.TrimStartAndEnd());
    TArray<TWeakObjectPtr<AActor>> Targets;
    Targets.Reserve(OrderedActors.Num());
    for (AActor* Actor : OrderedActors)
    {
        if (!Actor || Actor == SourceActor)
        {
            continue;
        }
        if (ActorMatchesRule(Actor, NameContains, RequiredTagName))
        {
            Targets.Add(Actor);
        }
    }

    if (Targets.Num() == 0)
    {
        EndBatchJob();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RuleCopyNoRuleMatchTargets", "No target actors match current rule filter."));
        return;
    }

    struct FCopyBatchState
    {
        TArray<TWeakObjectPtr<AActor>> Actors;
        TWeakObjectPtr<UStaticMeshComponent> SourceMesh;
        FText SourceUsed;
        int32 UpdatedComponents = 0;
        int32 UpdatedSlots = 0;
        TArray<FString> Failures;
        TSharedPtr<SNotificationItem> Notification;
        TSharedPtr<FScopedTransaction> Transaction;
    };

    TSharedRef<FCopyBatchState> State = MakeShared<FCopyBatchState>();
    State->Actors = MoveTemp(Targets);
    State->SourceMesh = SourceMeshComponent;
    State->SourceUsed = SourceUsedText;
    FEditModelToolBatchContext Context;
    Context.TotalItems = State->Actors.Num();
    Context.BatchSize = BatchSize;

    FEditModelToolBatchHooks Hooks;
    Hooks.OnStart = [State](FEditModelToolBatchContext&)
    {
        State->Notification = EditModelToolNotifications::ShowProgress(LOCTEXT("RuleCopyProgressTitle", "Rule-based material copy is running"));
        State->Transaction = MakeShared<FScopedTransaction>(LOCTEXT("RuleCopySingleTx", "Rule-based Copy Materials"));
    };
    Hooks.OnChunk = [State](FEditModelToolBatchContext& ContextArg, const int32 StartIndex, const int32 EndIndex)
    {
        UStaticMeshComponent* SourceMesh = State->SourceMesh.Get();
        if (!SourceMesh)
        {
            ContextArg.bCancelRequested = true;
            State->Failures.Add(TEXT("Source mesh is no longer valid."));
            return;
        }

        const int32 SourceMaterialCount = SourceMesh->GetNumMaterials();
        if (SourceMaterialCount <= 0)
        {
            ContextArg.bCancelRequested = true;
            State->Failures.Add(TEXT("Source mesh has no material slots anymore."));
            return;
        }

        for (int32 Index = StartIndex; Index < EndIndex; ++Index)
        {
            AActor* TargetActor = State->Actors[Index].Get();
            if (!TargetActor || !IsValid(TargetActor))
            {
                State->Failures.Add(FString::Printf(TEXT("Index %d: invalid target actor."), Index));
                continue;
            }

            TArray<UStaticMeshComponent*> TargetMeshComponents;
            TargetActor->GetComponents<UStaticMeshComponent>(TargetMeshComponents);
            if (TargetMeshComponents.Num() == 0)
            {
                State->Failures.Add(FString::Printf(TEXT("%s: no static mesh component."), *TargetActor->GetActorLabel()));
                continue;
            }

            bool bAnyComponentChangedInActor = false;
            for (UStaticMeshComponent* TargetMeshComponent : TargetMeshComponents)
            {
                if (!TargetMeshComponent)
                {
                    continue;
                }

                const FEditModelToolMaterialCopyResult CopyResult =
                    EditModelToolMaterialService::CopyMaterialsByElement(SourceMesh, TargetMeshComponent);
                if (CopyResult.bAnyChange)
                {
                    State->UpdatedComponents += CopyResult.UpdatedComponents;
                    State->UpdatedSlots += CopyResult.UpdatedSlots;
                    bAnyComponentChangedInActor = true;
                }
            }

            if (!bAnyComponentChangedInActor)
            {
                State->Failures.Add(FString::Printf(TEXT("%s: no slots changed."), *TargetActor->GetActorLabel()));
            }
        }
    };
    Hooks.OnProgress = [State](FEditModelToolBatchContext&, const int32 Processed, const int32 Total, const double EtaSec)
    {
        UE_LOG(
            LogTemp,
            Display,
            TEXT("[EditModelTool][RuleCopy] Progress %d/%d, ETA %.1fs"),
            Processed,
            Total,
            EtaSec);
        EditModelToolNotifications::UpdateProgress(
            State->Notification,
            FText::Format(LOCTEXT("RuleCopyProgressFmt", "Rule copy {0}/{1} (ETA {2}s)"), FText::AsNumber(Processed), FText::AsNumber(Total), FText::AsNumber(FMath::RoundToInt(EtaSec))),
            Total > 0 ? static_cast<float>(Processed) / static_cast<float>(Total) : 1.0f);
    };
    Hooks.OnComplete = [this, State](FEditModelToolBatchContext& ContextArg)
    {
        State->Transaction.Reset();
        FString FailurePreview;
        const int32 FailurePreviewCount = FMath::Min(8, State->Failures.Num());
        for (int32 i = 0; i < FailurePreviewCount; ++i)
        {
            FailurePreview += FString::Printf(TEXT("\n- %s"), *State->Failures[i]);
        }
        if (State->Failures.Num() > FailurePreviewCount)
        {
            FailurePreview += FString::Printf(TEXT("\n...and %d more"), State->Failures.Num() - FailurePreviewCount);
        }
        for (const FString& Failure : State->Failures)
        {
            UE_LOG(LogTemp, Warning, TEXT("[EditModelTool][RuleCopy][Failure] %s"), *Failure);
        }

        EditModelToolNotifications::Complete(
            State->Notification,
            LOCTEXT("RuleCopyComplete", "Rule-based material copy finished"),
            !ContextArg.bCancelRequested);
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(FString::Printf(
                TEXT("Rule-based copy materials finished.\nSource used: %s\nMatched targets: %d\nUpdated components: %d\nUpdated slots: %d\nFailures: %d%s"),
                *State->SourceUsed.ToString(),
                ContextArg.TotalItems,
                State->UpdatedComponents,
                State->UpdatedSlots,
                State->Failures.Num(),
                *FailurePreview)));
        EndBatchJob();
    };
    FEditModelToolBatchRunner::Start(Hooks, MoveTemp(Context));
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
        if (!ActorMatchesGlobalRule(Actor))
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
