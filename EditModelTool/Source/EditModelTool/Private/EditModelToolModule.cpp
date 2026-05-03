#include "EditModelToolModule.h"
#include "EditModelToolPluginDefaults.h"
#include "EditModelToolSession.h"
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
#include "Engine/StaticMesh.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/LightComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Light.h"
#include "Components/ActorComponent.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Internationalization/Regex.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Templates/SharedPointer.h"
#include "DatasmithAssetUserData.h"
#include "Containers/Ticker.h"
#include "Misc/Guid.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"

#define LOCTEXT_NAMESPACE "FEditModelToolModule"

namespace
{
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

    void GatherOrderedStaticMeshComponents(const TArray<AActor*>& OrderedActors, TArray<UStaticMeshComponent*>& OutOrdered)
    {
        OutOrdered.Reset();
        for (AActor* Actor : OrderedActors)
        {
            if (!Actor)
            {
                continue;
            }
            TArray<UStaticMeshComponent*> Comps;
            Actor->GetComponents<UStaticMeshComponent>(Comps);
            for (UStaticMeshComponent* C : Comps)
            {
                if (C)
                {
                    OutOrdered.Add(C);
                }
            }
        }
    }

    UStaticMeshComponent* GetRichestStaticMeshComponentOnActor(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }

        UStaticMeshComponent* Best = nullptr;
        int32 BestCount = 0;
        TArray<UStaticMeshComponent*> Comps;
        Actor->GetComponents<UStaticMeshComponent>(Comps);
        for (UStaticMeshComponent* C : Comps)
        {
            if (!C)
            {
                continue;
            }
            const int32 N = C->GetNumMaterials();
            if (N > BestCount)
            {
                BestCount = N;
                Best = C;
            }
        }
        return Best;
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

    /** INDEX_NONE if invalid/unknown LOD0; otherwise LOD0 triangle count (0 is valid for degenerate meshes). */
    int32 TryGetLod0TriangleCount(const UStaticMesh* Mesh)
    {
        if (!Mesh)
        {
            return INDEX_NONE;
        }
        const int32 N = Mesh->GetNumTriangles(0);
        return (N >= 0) ? N : INDEX_NONE;
    }

    /** Mirrors broad-phase coincidence tolerances — large-world float error vs fixed 0.1. */
    float AdaptiveOverlapLinearTolWorldAABBs(const FBox& B0, const FBox& B1)
    {
        auto MaxCornerAbs = [](const FBox& B) -> float
        {
            return FMath::Max(B.Min.GetAbs().GetMax(), B.Max.GetAbs().GetMax());
        };
        const float CoordRef = FMath::Max(FMath::Max(MaxCornerAbs(B0), MaxCornerAbs(B1)), 1.f);
        const float ExtRef = FMath::Max(B0.GetExtent().GetMax(), B1.GetExtent().GetMax());
        return FMath::Max(0.1f, FMath::Max(CoordRef * 5e-6f, ExtRef * 5e-5f));
    }

    bool BoundsMinMaxNearlyEqualAdaptiveWorld(const FBox& B0, const FBox& B1)
    {
        const float T = AdaptiveOverlapLinearTolWorldAABBs(B0, B1);
        return FMath::IsNearlyEqual(B0.Min.X, B1.Min.X, T) && FMath::IsNearlyEqual(B0.Min.Y, B1.Min.Y, T)
            && FMath::IsNearlyEqual(B0.Min.Z, B1.Min.Z, T) && FMath::IsNearlyEqual(B0.Max.X, B1.Max.X, T)
            && FMath::IsNearlyEqual(B0.Max.Y, B1.Max.Y, T) && FMath::IsNearlyEqual(B0.Max.Z, B1.Max.Z, T);
    }

    /** Author-space boxes — duplicate extrusions / copies often match here despite different StaticMesh UObject paths. */
    bool LocalMeshesAuthorBoundsNearlyMatch(const UStaticMesh* MeshA, const UStaticMesh* MeshB)
    {
        if (!MeshA || !MeshB || MeshA == MeshB)
        {
            return false;
        }

        const FBox ABox = MeshA->GetBoundingBox();
        const FBox BBox = MeshB->GetBoundingBox();
        if (!ABox.IsValid || !BBox.IsValid)
        {
            return false;
        }

        const float SizeRef =
            FMath::Max(FMath::Max(ABox.GetExtent().GetMax(), BBox.GetExtent().GetMax()), 1.f);
        const float T = FMath::Max(KINDA_SMALL_NUMBER, SizeRef * 5e-5f);
        return FMath::IsNearlyEqual(ABox.Min.X, BBox.Min.X, T) && FMath::IsNearlyEqual(ABox.Min.Y, BBox.Min.Y, T)
            && FMath::IsNearlyEqual(ABox.Min.Z, BBox.Min.Z, T) && FMath::IsNearlyEqual(ABox.Max.X, BBox.Max.X, T)
            && FMath::IsNearlyEqual(ABox.Max.Y, BBox.Max.Y, T)
            && FMath::IsNearlyEqual(ABox.Max.Z, BBox.Max.Z, T);
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
        TSharedPtr<SEditableTextBox> SameNormalAngleBox;
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
        .SizingRule(ESizingRule::UserSized)
        .ClientSize(FVector2D(300.f, 520.f))
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
                .AutoWrapText(true)
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

    const auto BuildTwoSelectionOverlapSection = [this]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT(
                    "TwoSelectionOverlapHint",
                    "Two-mesh spacing — Requires two actors with StaticMesh. Same StaticMesh asset = strong shape match; if assets differ but LOD0 triangle counts match, a weak heuristic is shown (same count ≠ same topology). Primary offset uses bounds centres; pivots secondary. Rotation/scale matter. Selection order matches Copy Materials First/Last."))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 4.f, 0.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("TwoSelectionOverlapDeltaButton", "Two selection: report overlap translation (Δ world)"))
                .ToolTipText(LOCTEXT(
                    "TwoSelectionOverlapDeltaTooltip",
                    "StaticMesh equality, CalcBounds hulls, authoring BoundingBox hints, pivot deltas. Clipboard gets one line only: (FIRST hull centre minus LAST hull centre) as X,Y,Z commas. Dialog still shows full text. Paste with Ctrl+V. Log: [EditModelTool][TwoOverlapDelta]."))
                .OnClicked_Lambda([this]()
                {
                    ReportLocationDeltaBetweenTwoSelectedActors();
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
                .AutoWrapText(true)
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
                .AutoWrapText(true)
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
                .AutoWrapText(true)
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
                        "For each object in the outliner selection that matches the type filter (All / Mesh / Spline / Blueprint / Light / Camera), appends the tag(s) from the [Add tag(s)...] field to that actor's Actor Tags (FName, AddUnique). Multiple tags: separate with comma or semicolon. Selection must contain at least one actor."))
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
                    "For each object in the outliner selection that matches the type filter (All / Mesh / Spline / Blueprint / Light / Camera), removes the tag(s) from the [Add tag(s)...] field from that actor's Actor Tags. Multiple tags: separate with comma or semicolon. Selection must contain at least one actor."))
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

    const auto BuildActionSection = [this, RequestCloseDialog, DialogState]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT(
                    "SameNormalAngleUiHint",
                    "Modeling — same-normal: half-angle cone in degrees (0.01–90). Applies when you click the button below; empty field resets to plugin default."))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 8.f)
            [
                SAssignNew(DialogState->SameNormalAngleBox, SEditableTextBox)
                .Text(FText::FromString(FString::Printf(
                    TEXT("%.2f"),
                    EditModelTool::SessionSameNormalAngleToleranceDegrees())))
                .HintText(LOCTEXT("SameNormalAnglePlaceholder", "e.g. 8"))
                .OnTextCommitted_Lambda([](const FText& NewText, ETextCommit::Type /*CommitType*/)
                {
                    const FString Trimmed = NewText.ToString().TrimStartAndEnd();
                    if (Trimmed.IsEmpty())
                    {
                        EditModelTool::SessionSameNormalAngleToleranceDegrees() =
                            EditModelTool::PluginDefaults::SameNormalFaceAngleToleranceDegrees;
                        return;
                    }
                    float Parsed = 0.f;
                    if (FDefaultValueHelper::ParseFloat(Trimmed, Parsed))
                    {
                        EditModelTool::SessionSameNormalAngleToleranceDegrees() = FMath::Clamp(Parsed, 0.01f, 90.f);
                    }
                })
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SUniformGridPanel)
                .SlotPadding(2.f)
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
                .ToolTipText(LOCTEXT(
                    "CopyMaterialsFromFirstSelectedTooltip",
                    "Copies materials slot-by-slot from the first-selected actor's richest static mesh into every other selected mesh. If another selected mesh has more material slots, that mesh is used as the source instead so more slots can propagate."))
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
                .ToolTipText(LOCTEXT(
                    "CopyMaterialsFromLastSelectedTooltip",
                    "Copies materials slot-by-slot from the last-selected actor's richest static mesh into every other selected mesh. If another selected mesh has more material slots, that mesh is used as the source instead so more slots can propagate."))
                    .OnClicked_Lambda([this]()
                    {
                        CopyMaterialsFromLastSelectedMeshByElement();
                        return FReply::Handled();
                    })
                ]
                + SUniformGridPanel::Slot(0, 3)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SelectSameNormalFacesButton", "Select Same-Normal Faces (Modeling Mode)"))
                    .ToolTipText(LOCTEXT("SelectSameNormalFacesTooltip", "Uses currently selected face as seed, then selects faces with similar normal on the same mesh (triangle face mode). Angle comes from the field above."))
                    .OnClicked_Lambda([this, DialogState]()
                    {
                        float Parsed = 0.f;
                        const FString Trimmed = DialogState->SameNormalAngleBox.IsValid()
                            ? DialogState->SameNormalAngleBox->GetText().ToString().TrimStartAndEnd()
                            : FString();
                        if (Trimmed.IsEmpty())
                        {
                            EditModelTool::SessionSameNormalAngleToleranceDegrees() =
                                EditModelTool::PluginDefaults::SameNormalFaceAngleToleranceDegrees;
                        }
                        else
                        {
                            if (!FDefaultValueHelper::ParseFloat(Trimmed, Parsed))
                            {
                                FMessageDialog::Open(
                                    EAppMsgType::Ok,
                                    LOCTEXT(
                                        "BadSameNormalAngle",
                                        "Please enter a valid number for same-normal angle (degrees), or clear the field to use the plugin default."));
                                return FReply::Handled();
                            }
                            EditModelTool::SessionSameNormalAngleToleranceDegrees() = FMath::Clamp(Parsed, 0.01f, 90.f);
                        }
                        SelectSameNormalFacesFromModelingSelection();
                        return FReply::Handled();
                    })
                ]
            ];
    };

    const auto BuildRuleBatchSection = [this, DialogState]() -> TSharedRef<SWidget>
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 8.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT("RuleBatchHint", "Global filters (empty = ignore): apply to rename/movable/group/material/overlap/metadata/empty-scan. Tag add/remove/search use type filter only."))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 4.f, 0.f, 4.f)
            [
                SAssignNew(DialogState->RuleNameContainsBox, SEditableTextBox)
                .OnTextChanged_Lambda([](const FText& NewText)
                {
                    EditModelTool::SessionFilterSettings().NameContains = NewText.ToString().TrimStartAndEnd();
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
                    EditModelTool::SessionFilterSettings().RequiredTag = NewText.ToString().TrimStartAndEnd();
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
                        EditModelTool::SessionFilterSettings().ChunkSize = 32;
                        return;
                    }
                    int32 ParsedValue = 0;
                    if (FDefaultValueHelper::ParseInt(Trimmed, ParsedValue) && ParsedValue > 0)
                    {
                        EditModelTool::SessionFilterSettings().ChunkSize = ParsedValue;
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

                    EditModelTool::SessionFilterSettings().ChunkSize = BatchSize;
                    RunRuleBasedMovableChunked(EditModelTool::SessionFilterSettings().NameContains, EditModelTool::SessionFilterSettings().RequiredTag, EditModelTool::SessionFilterSettings().ChunkSize);
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

                    EditModelTool::SessionFilterSettings().ChunkSize = BatchSize;
                    RunRuleBasedCopyMaterialsChunked(EditModelTool::SessionFilterSettings().NameContains, EditModelTool::SessionFilterSettings().RequiredTag, EditModelTool::SessionFilterSettings().ChunkSize);
                    return FReply::Handled();
                })
            ];
    };

    DialogWindow->SetContent(
        SNew(SBox)
        .Padding(6.f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                SNew(SBox)
                .MaxDesiredWidth(284.f)
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
                        BuildTwoSelectionOverlapSection()
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
                    .Padding(0.f, 8.f, 0.f, 0.f)
                    .HAlign(HAlign_Right)
                    [
                        BuildActionSection()
                    ]
                ]
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
        if (!EditModelTool::ActorMatchesGlobalSessionFilter(StaticMeshActor))
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
        if (!EditModelTool::ActorMatchesGlobalSessionFilter(StaticMeshActor))
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
        if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
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
            if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
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

    const int32 BatchSizePerTick = EditModelToolFilterPolicy::GetEffectiveChunkSize(EditModelTool::SessionFilterSettings());
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
        if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
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
        LOCTEXT("MaterialCopyResultFmt", "Material copy complete.\n{2}\nUpdated components: {0}\nUpdated material slots: {1}"));
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
        LOCTEXT("MaterialCopyFromLastResultFmt", "Material copy complete.\n{2}\nUpdated components: {0}\nUpdated material slots: {1}"));
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

    AActor* PreferredSourceActor = OrderedActors[ResolvedSourceIndex];
    if (!PreferredSourceActor)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("InvalidSourceActorForMaterialCopyShared", "Could not determine the {0} selected actor as source."),
            SourceSelectorLabel));
        return;
    }

    TArray<UStaticMeshComponent*> OrderedMeshComponents;
    GatherOrderedStaticMeshComponents(OrderedActors, OrderedMeshComponents);
    if (OrderedMeshComponents.Num() < 2)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MaterialCopyNeedTwoMeshComponents", "Need at least two static mesh components in the current selection."));
        return;
    }

    int32 GlobalMaxSlots = 0;
    for (UStaticMeshComponent* M : OrderedMeshComponents)
    {
        GlobalMaxSlots = FMath::Max(GlobalMaxSlots, M->GetNumMaterials());
    }
    if (GlobalMaxSlots <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MaterialCopySelectionNoSlots", "No material slots found on selected static meshes."));
        return;
    }

    UStaticMeshComponent* PreferredRichSMC = GetRichestStaticMeshComponentOnActor(PreferredSourceActor);
    if (!PreferredRichSMC || PreferredRichSMC->GetNumMaterials() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SourceHasNoStaticMeshForMaterialCopyShared", "Preferred source actor has no static mesh component with materials."));
        return;
    }

    UStaticMeshComponent* EffectiveSourceSMC = nullptr;
    if (PreferredRichSMC->GetNumMaterials() >= GlobalMaxSlots)
    {
        EffectiveSourceSMC = PreferredRichSMC;
    }
    else
    {
        for (UStaticMeshComponent* Candidate : OrderedMeshComponents)
        {
            if (Candidate && Candidate->GetNumMaterials() == GlobalMaxSlots)
            {
                EffectiveSourceSMC = Candidate;
                break;
            }
        }
    }

    if (!EffectiveSourceSMC)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MaterialCopyCouldNotResolveSource", "Could not choose a source mesh with material slots."));
        return;
    }

    const bool bPreferredWasWeakerThanSelection = PreferredRichSMC->GetNumMaterials() < GlobalMaxSlots;
    const FText SourceLineText = bPreferredWasWeakerThanSelection
        ? FText::Format(
              LOCTEXT(
                  "MatCopyEffectiveSourceChosenFmt",
                  "Source mesh: richest in selection ({0} slot(s)) — preferred {1}-selected mesh had fewer."),
              FText::AsNumber(GlobalMaxSlots),
              SourceUsedLabel)
        : FText::Format(
              LOCTEXT("MatCopyPreferredSourceFmt", "Source mesh: {0}-selected actor (mesh with most slots on that actor, {1} slot(s))."),
              SourceUsedLabel,
              FText::AsNumber(EffectiveSourceSMC->GetNumMaterials()));

    const FScopedTransaction Transaction(TransactionText);

    int32 UpdatedComponentCount = 0;
    int32 UpdatedSlotCount = 0;
    for (UStaticMeshComponent* TargetMeshComponent : OrderedMeshComponents)
    {
        if (!TargetMeshComponent || TargetMeshComponent == EffectiveSourceSMC)
        {
            continue;
        }

        const FEditModelToolMaterialCopyResult CopyResult =
            EditModelToolMaterialService::CopyMaterialsByElement(EffectiveSourceSMC, TargetMeshComponent);
        UpdatedComponentCount += CopyResult.UpdatedComponents;
        UpdatedSlotCount += CopyResult.UpdatedSlots;
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
            SourceLineText);
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

void FEditModelToolModule::ReportLocationDeltaBetweenTwoSelectedActors()
{
    TArray<AActor*> Selected;
    EditModelToolSelectionUtils::GatherSelectedActors(Selected);
    if (Selected.Num() != 2)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            LOCTEXT("TwoOverlapDeltaNeedTwo", "Select exactly two actors in the World Outliner (current count is not 2)."));
        return;
    }

    AActor* ActorFirst = Selected[0];
    AActor* ActorLast = Selected[1];
    if (!ActorFirst || !ActorLast || !IsValid(ActorFirst) || !IsValid(ActorLast))
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TwoOverlapDeltaInvalidActor", "One of the selected actors is invalid."));
        return;
    }

    UStaticMeshComponent* SMCFirst = GetFirstStaticMeshComponent(ActorFirst);
    UStaticMeshComponent* SMCLast = GetFirstStaticMeshComponent(ActorLast);
    if (!SMCFirst || !SMCLast)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            LOCTEXT(
                "TwoOverlapDeltaNeedSMC",
                "Each actor must have at least one StaticMeshComponent. (Mesh-based shape check requires a static mesh on both.)"));
        return;
    }

    UStaticMesh* MeshFirst = SMCFirst->GetStaticMesh();
    UStaticMesh* MeshLast = SMCLast->GetStaticMesh();
    if (!MeshFirst || !MeshLast)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TwoOverlapDeltaNeedMeshAsset", "Both actors need a StaticMesh assigned on their first StaticMeshComponent."));
        return;
    }

    const bool bSameStaticMeshAsset = (MeshFirst == MeshLast);

    const int32 TriFirst = TryGetLod0TriangleCount(MeshFirst);
    const int32 TriLast = TryGetLod0TriangleCount(MeshLast);
    const bool bTriReadableBoth = (TriFirst != INDEX_NONE && TriLast != INDEX_NONE);
    const bool bHeuristicDiffAssetMatchedTris = !bSameStaticMeshAsset && bTriReadableBoth && (TriFirst == TriLast);
    const bool bHeuristicTwinAuthoringAABBs = LocalMeshesAuthorBoundsNearlyMatch(MeshFirst, MeshLast);
    const bool bHeuristicStrongExtrusionTwin =
        !bSameStaticMeshAsset && bHeuristicDiffAssetMatchedTris && bHeuristicTwinAuthoringAABBs;

    /** Match lifting code: CalcBounds avoids stale cached Bounds when reporting. */
    const FBox WBoxFirst = SMCFirst->CalcBounds(SMCFirst->GetComponentTransform()).GetBox();
    const FBox WBoxLast = SMCLast->CalcBounds(SMCLast->GetComponentTransform()).GetBox();
    /** World-space AABB centre — same semantics as Broad+BC Z lift. */
    const FVector GeomCenterFirst = WBoxFirst.GetCenter();
    const FVector GeomCenterLast = WBoxLast.GetCenter();
    const FVector GeomCentreDelta = GeomCenterLast - GeomCenterFirst;
    const float GeomAdaptiveTol = AdaptiveOverlapLinearTolWorldAABBs(WBoxFirst, WBoxLast);
    const bool bHullCentresCoincidentXYZ = BoundsMinMaxNearlyEqualAdaptiveWorld(WBoxFirst, WBoxLast);

    const FVector PivotFirst = SMCFirst->GetComponentLocation();
    const FVector PivotLast = SMCLast->GetComponentLocation();

    const FVector ScaleFirst = SMCFirst->GetComponentScale();
    const FVector ScaleLast = SMCLast->GetComponentScale();
    const float ScaleTol = 0.001f;
    const bool bScaleMatches = FMath::IsNearlyEqual(ScaleFirst.X, ScaleLast.X, ScaleTol)
        && FMath::IsNearlyEqual(ScaleFirst.Y, ScaleLast.Y, ScaleTol)
        && FMath::IsNearlyEqual(ScaleFirst.Z, ScaleLast.Z, ScaleTol);

    const float AngleDeg = FMath::RadiansToDegrees(SMCFirst->GetComponentQuat().AngularDistance(SMCLast->GetComponentQuat()));
    static constexpr float RotationWarningDeg = 0.75f;
    const bool bRotationMatches = AngleDeg <= RotationWarningDeg;

    TArray<FString> WarningLines;

    FString ShapeSection;
    FString TriNoteSuffix;
    if (bTriReadableBoth)
    {
        TriNoteSuffix = FString::Printf(TEXT("\n\nLOD0 triangle count — [First] %d / [Last] %d."), TriFirst, TriLast);
    }
    else
    {
        TriNoteSuffix = TEXT("\n\nLOD0 triangle count — unavailable (failed to resolve for one or both meshes).");
    }

    if (bSameStaticMeshAsset)
    {
        ShapeSection = FString::Printf(
            TEXT("Shape: SAME StaticMesh asset (same authored geometry resource)\n%s%s"),
            *MeshFirst->GetPathName(),
            *TriNoteSuffix);
    }
    else
    {
        ShapeSection = FString::Printf(
            TEXT("Shape: DIFFERENT StaticMesh assets — not the same mesh resource.\n\n[First] %s\n[Last]  %s%s"),
            *MeshFirst->GetPathName(),
            *MeshLast->GetPathName(),
            *TriNoteSuffix);

        if (bHeuristicStrongExtrusionTwin)
        {
            ShapeSection += FString::Printf(
                TEXT(
                    "\n\n--- Stronger heuristic (still not proof) ---\nLOD0 tris=%d AND mesh authoring BoundingBox (local) closely matches."
                    "\nOften true for duplicated extruded StaticMesh clones (distinct asset paths).\nTreat overlap distance as tentative until you verify."),
                TriFirst);
            WarningLines.Add(TEXT("Twin-extrusion style heuristic — trusting bounds/triangle match only."));
        }
        else if (bHeuristicDiffAssetMatchedTris)
        {
            ShapeSection += FString::Printf(
                TEXT(
                    "\n\n--- Heuristic (weak) ---\nLOD0 triangle counts match (%d). Same count does NOT prove identical topology (different welds,"
                    " vertex order, holes, LOD); bounds-centre offset below is exploratory only."),
                TriFirst);
            WarningLines.Add(TEXT("Different assets with matching triangle counts only — overlap offset is tentative; verify meshes externally."));
        }
        else
        {
            WarningLines.Add(TEXT("Meshes differ — use bounds/pivot deltas at your own risk."));
        }
    }





    if (!bScaleMatches)
    {
        WarningLines.Add(FString::Printf(
            TEXT("World scale differs ([First]= %.6f,%.6f,%.6f) vs ([Last]= %.6f,%.6f,%.6f) — same asset still needs matching scale for exact overlap."),
            ScaleFirst.X,
            ScaleFirst.Y,
            ScaleFirst.Z,
            ScaleLast.X,
            ScaleLast.Y,
            ScaleLast.Z));
    }

    if (!bRotationMatches)
    {
        WarningLines.Add(FString::Printf(
            TEXT("Component rotation differs by ~%.3f degrees — translating the actor alone will NOT fully overlap the volumes."),
            AngleDeg));
    }

    FString HullInterpret;
    if (bHullCentresCoincidentXYZ)
    {
        HullInterpret = FString::Printf(
            TEXT("World hull AABBs: COINCIDENT (within adaptive linear tol ~%.6g). This uses CalcBounds + tol scaled by coordinate magnitude—not a fixed 0.1 fuzz."),
            GeomAdaptiveTol);
    }
    else
    {
        HullInterpret =
            FString::Printf(
                TEXT(
                    "World hull AABBs: NOT coincident.\nGeometry-centre delta (LAST-FIRST) = %.6f, %.6f, %.6f (|d| ~= %.6f).\n"
                    "Exported map text RelativeLocation pairs can be kilometres apart yet still describe the SAME pair of meshes before you move actors—this tool reflects the LIVE editor transforms, not pasted text."),
                GeomCentreDelta.X,
                GeomCentreDelta.Y,
                GeomCentreDelta.Z,
                GeomCentreDelta.Length());
    }

    FString RecSection;
    const FVector MoveLastOntoGeomFirst = GeomCenterFirst - GeomCenterLast;

    if (bHullCentresCoincidentXYZ && bScaleMatches && bRotationMatches)
    {
        FString KindLine;
        if (bSameStaticMeshAsset)
        {
            KindLine =
                TEXT("Same StaticMesh asset; world boxes already coincide after CalcBounds (+ adaptive tolerance).\nRecommended LAST translation (should be ~0):");
        }
        else if (bHeuristicStrongExtrusionTwin)
        {
            KindLine =
                TEXT("DIFFERENT assets but twin-extrusion heuristic (triangle + authoring-box match).\nRough LAST alignment (AABBs coincide; treat as exploratory):");
        }
        else
        {
            KindLine =
                TEXT("Different UObject paths but world hulls coincide within tolerance.\nLAST translation residual (AABBs centred; exploratory if assets differ):");
        }

        const FVector MovRecip = GeomCenterLast - GeomCenterFirst;
        RecSection = FString::Printf(
            TEXT("%s\ndX,dY,dZ = %.6g, %.6g, %.6g\nPivot offsets may still be non-zero; geometry hulls overlap.\nReciprocal (FIRST<-LAST hull): %.6g, %.6g, %.6g"),
            *KindLine,
            MoveLastOntoGeomFirst.X,
            MoveLastOntoGeomFirst.Y,
            MoveLastOntoGeomFirst.Z,
            MovRecip.X,
            MovRecip.Y,
            MovRecip.Z);
    }
    else if (bSameStaticMeshAsset && bScaleMatches && bRotationMatches)
    {
        const FString G = FString::Printf(
            TEXT("RECOMMENDED (align GEOMETRY centres, world):\n Move LAST actor by translation dX,dY,dZ = %.6f, %.6f, %.6f\n (add to LAST Actor Location; bounds-centre overlap, same mesh, matched rot/scale)"),
            MoveLastOntoGeomFirst.X,
            MoveLastOntoGeomFirst.Y,
            MoveLastOntoGeomFirst.Z);
        const FVector MoveFirstOntoGeomLast = GeomCenterLast - GeomCenterFirst;
        const FString Gi = FString::Printf(
            TEXT("Reciprocal: move FIRST actor by %.6f, %.6f, %.6f to match LAST geometry centre."),
            MoveFirstOntoGeomLast.X,
            MoveFirstOntoGeomLast.Y,
            MoveFirstOntoGeomLast.Z);
        RecSection = G + FString(TEXT("\n\n")) + Gi;
    }
    else if (bSameStaticMeshAsset)
    {
        RecSection = FString::Printf(
            TEXT("Approximate geometry-centre offset (LAST <- FIRST), same asset but rotation/scale not ideal:\ndX,dY,dZ = %.6f, %.6f, %.6f\nFix rotation/scale first for a trustworthy overlap."),
            MoveLastOntoGeomFirst.X,
            MoveLastOntoGeomFirst.Y,
            MoveLastOntoGeomFirst.Z);
    }
    else if (bHeuristicStrongExtrusionTwin && bScaleMatches && bRotationMatches)
    {
        RecSection = FString::Printf(
            TEXT(
                "HEURISTIC (twin extrusion): different assets yet triangles + authoring boxes match.\nSuggested LAST hull-centre move:\n"
                "dX,dY,dZ = %.6f, %.6f, %.6f\nVerify overlap manually — identical topology is NOT guaranteed."),
            MoveLastOntoGeomFirst.X,
            MoveLastOntoGeomFirst.Y,
            MoveLastOntoGeomFirst.Z);
    }
    else if (bHeuristicDiffAssetMatchedTris && bScaleMatches && bRotationMatches)
    {
        RecSection = FString::Printf(
            TEXT(
                "HEURISTIC ONLY (different assets; LOD0 tris matched = %d):\nRough bounds-centre alignment LAST<-FIRST:\n"
                "dX,dY,dZ = %.6f, %.6f, %.6f\n"
                "(Could still be unrelated meshes — confirm in authoring tool.)"),
            TriFirst,
            MoveLastOntoGeomFirst.X,
            MoveLastOntoGeomFirst.Y,
            MoveLastOntoGeomFirst.Z);
    }
    else if (bHeuristicDiffAssetMatchedTris)
    {
        RecSection = FString::Printf(
            TEXT(
                "HEURISTIC ONLY (triangle match %d, different assets) — approximate bounds-centre offset LAST<-FIRST:\n"
                "dX,dY,dZ = %.6f, %.6f, %.6f\nRotation/scale do not fully match — fix those before trusting overlap."),
            TriFirst,
            MoveLastOntoGeomFirst.X,
            MoveLastOntoGeomFirst.Y,
            MoveLastOntoGeomFirst.Z);
    }
    else
    {
        RecSection =
            TEXT("(No geometry-centre recommendation — assets differ, LOD0 triangles differ or unreadable, and hulls do not coincide within adaptive tolerance.)");
    }

    const FString GeomBlock = FString::Printf(
        TEXT(
            "Actors / components\n---------------\n[First] %s  (%s)\n[Last]  %s  (%s)\n\n%s\n\n%s\n\nGeometry (CalcBounds hull centres)\n---------------\n[First centre] %.6f, %.6f, %.6f\n[Last centre]  %.6f, %.6f, %.6f\n[last - first geom] %.6f, %.6f, %.6f\n\n%s"),
        *ActorFirst->GetActorLabel(),
        *ActorFirst->GetClass()->GetName(),
        *ActorLast->GetActorLabel(),
        *ActorLast->GetClass()->GetName(),
        *ShapeSection,
        *HullInterpret,
        GeomCenterFirst.X,
        GeomCenterFirst.Y,
        GeomCenterFirst.Z,
        GeomCenterLast.X,
        GeomCenterLast.Y,
        GeomCenterLast.Z,
        (GeomCenterLast.X - GeomCenterFirst.X),
        (GeomCenterLast.Y - GeomCenterFirst.Y),
        (GeomCenterLast.Z - GeomCenterFirst.Z),
        *RecSection);

    const FVector DeltaPivotLMF = PivotLast - PivotFirst;
    const FString PivotBlock = FString::Printf(
        TEXT("Component pivot ONLY (often NOT geometric centre)\n---------------\n[First pivot] %.6f, %.6f, %.6f\n[Last pivot]  %.6f, %.6f, %.6f\n[last - first pivot] %.6f, %.6f, %.6f\n\nPivot translation to put LAST pivot on FIRST: %.6f, %.6f, %.6f"),
        PivotFirst.X,
        PivotFirst.Y,
        PivotFirst.Z,
        PivotLast.X,
        PivotLast.Y,
        PivotLast.Z,
        DeltaPivotLMF.X,
        DeltaPivotLMF.Y,
        DeltaPivotLMF.Z,
        (PivotFirst.X - PivotLast.X),
        (PivotFirst.Y - PivotLast.Y),
        (PivotFirst.Z - PivotLast.Z));

    FString WarnBlock;
    if (WarningLines.Num() > 0)
    {
        WarnBlock = TEXT("Warnings\n---------------");
        for (const FString& W : WarningLines)
        {
            WarnBlock += TEXT("\n- ");
            WarnBlock += W;
        }
        WarnBlock += TEXT("\n");
    }

    const FString Combined = FString::Printf(TEXT("%s\n\n%s\n\n%s"), *GeomBlock, *PivotBlock, *WarnBlock);

    const FVector GeomFirstMinusLastHull = GeomCenterFirst - GeomCenterLast;
    const FString ClipboardGeomFirstMinusLast = FString::Printf(
        TEXT("%.6f,%.6f,%.6f"),
        GeomFirstMinusLastHull.X,
        GeomFirstMinusLastHull.Y,
        GeomFirstMinusLastHull.Z);

    UE_LOG(LogTemp, Log, TEXT("[EditModelTool][TwoOverlapDelta] clipboard = first-last hull centre XYZ: %s"), *ClipboardGeomFirstMinusLast);

    FPlatformApplicationMisc::ClipboardCopy(*ClipboardGeomFirstMinusLast);
    const FString DialogBody = FString::Printf(
        TEXT(
            "[EditModelTool] Clipboard: only (FIRST hull centre) minus (LAST hull centre), XYZ in world units (comma-separated).\n\n%s\n\n"
            "Paste with Ctrl+V (Cmd+V). Full breakdown below:\n---\n\n%s"),
        *ClipboardGeomFirstMinusLast,
        *Combined);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(DialogBody));
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
        if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
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
