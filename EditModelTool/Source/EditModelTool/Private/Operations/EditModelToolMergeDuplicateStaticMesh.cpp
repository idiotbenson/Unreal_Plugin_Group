#include "EditModelToolPrivatePCH.h"

#include "Operations/EditModelToolMergeDuplicateStaticMesh.h"

#include "EditModelToolSession.h"
#include "Core/EditModelToolSelectionUtils.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "MeshDescription.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Async/Async.h"
#include "ScopedTransaction.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"

#ifdef _MSC_VER
// If your build log does not show this line, MSVC is not compiling this updated translation unit (stale build / wrong tree).
#pragma message("EditModelTool: compiling EditModelToolMergeDuplicateStaticMesh.cpp (merge report UI = SScrollBox + STextBlock; no SMultiLineEditableText)")
#endif

#define LOCTEXT_NAMESPACE "EditModelToolMergeDup"

namespace
{
    constexpr uint64 GStrongGeometryFingerprintBit = (1ull << 62);

    struct FQuantizedTriple
    {
        int64 X = 0;
        int64 Y = 0;
        int64 Z = 0;

        bool operator<(const FQuantizedTriple& O) const
        {
            if (X != O.X)
            {
                return X < O.X;
            }
            if (Y != O.Y)
            {
                return Y < O.Y;
            }
            return Z < O.Z;
        }
    };

    /** Undirected edge keyed by lex-min / lex-max quantized endpoints (asset-order invariant). */
    struct FCanonEdge
    {
        FQuantizedTriple Lo;
        FQuantizedTriple Hi;

        bool operator<(const FCanonEdge& O) const
        {
            if (Lo < O.Lo)
            {
                return true;
            }
            if (O.Lo < Lo)
            {
                return false;
            }
            return Hi < O.Hi;
        }
    };

    /** Triangle as sorted corner triple (winding invariant). */
    struct FCanonTriangle
    {
        FQuantizedTriple A;
        FQuantizedTriple B;
        FQuantizedTriple C;

        bool operator<(const FCanonTriangle& O) const
        {
            if (A < O.A)
            {
                return true;
            }
            if (O.A < A)
            {
                return false;
            }
            if (B < O.B)
            {
                return true;
            }
            if (O.B < B)
            {
                return false;
            }
            return C < O.C;
        }
    };

    static FQuantizedTriple QuantizePosition(const FVector3f& P3f, const double Scale)
    {
        FQuantizedTriple Q;
        Q.X = static_cast<int64>(FMath::RoundToInt(static_cast<double>(P3f.X) * Scale));
        Q.Y = static_cast<int64>(FMath::RoundToInt(static_cast<double>(P3f.Y) * Scale));
        Q.Z = static_cast<int64>(FMath::RoundToInt(static_cast<double>(P3f.Z) * Scale));
        return Q;
    }

    static FCanonEdge MakeCanonEdge(const FQuantizedTriple& U, const FQuantizedTriple& V)
    {
        FCanonEdge E;
        if (U < V)
        {
            E.Lo = U;
            E.Hi = V;
        }
        else if (V < U)
        {
            E.Lo = V;
            E.Hi = U;
        }
        else
        {
            E.Lo = U;
            E.Hi = V;
        }
        return E;
    }

    static uint64 Fnv1a64_Update(uint64 Hash, uint64 Word)
    {
        constexpr uint64 FnvPrime = 1099511628211ull;
        Hash ^= Word;
        return Hash * FnvPrime;
    }

    static uint64 HashSortedTriples(const TArray<FQuantizedTriple>& Sorted)
    {
        uint64 H = 1469598103934665603ull;
        for (const FQuantizedTriple& T : Sorted)
        {
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.X)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.Y)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.Z)));
        }
        return H;
    }

    static uint64 HashSortedCanonTriangles(const TArray<FCanonTriangle>& Sorted)
    {
        uint64 H = 975531335987437023ull;
        for (const FCanonTriangle& T : Sorted)
        {
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.A.X)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.A.Y)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.A.Z)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.B.X)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.B.Y)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.B.Z)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.C.X)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.C.Y)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.C.Z)));
        }
        return H;
    }

    static uint64 HashSortedCanonEdges(const TArray<FCanonEdge>& Sorted)
    {
        uint64 H = 1229492661964435241ull;
        for (const FCanonEdge& E : Sorted)
        {
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Lo.X)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Lo.Y)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Lo.Z)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Hi.X)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Hi.Y)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Hi.Z)));
        }
        return H;
    }

    static bool TryComputeGeometryFingerprint(const UStaticMesh* Mesh, uint64& OutFingerprint)
    {
        OutFingerprint = 0;
        if (!Mesh)
        {
            return false;
        }

        const FMeshDescription* Desc = Mesh->GetMeshDescription(0);
        const int32 TriRender = Mesh->GetNumTriangles(0);
        const int32 VertRender = Mesh->GetNumVertices(0);

        if (Desc && Desc->Triangles().Num() > 0 && Desc->Vertices().Num() > 0)
        {
            const TVertexAttributesConstRef<FVector3f> VertexPositions = Desc->GetVertexPositions();
            TArray<FQuantizedTriple> VertexMultiset;
            VertexMultiset.Reserve(Desc->Vertices().Num());
            constexpr double Scale = 10000.0;
            for (FVertexID VertexID : Desc->Vertices().GetElementIDs())
            {
                VertexMultiset.Add(QuantizePosition(VertexPositions[VertexID], Scale));
            }
            VertexMultiset.Sort();

            TArray<FCanonTriangle> CanonTriangles;
            TArray<FCanonEdge> CanonEdges;
            CanonTriangles.Reserve(Desc->Triangles().Num());
            CanonEdges.Reserve(Desc->Triangles().Num() * 3);

            for (FTriangleID TriID : Desc->Triangles().GetElementIDs())
            {
                const TArrayView<const FVertexInstanceID> Vis = Desc->GetTriangleVertexInstances(TriID);
                if (Vis.Num() < 3)
                {
                    return false;
                }
                TArray<FQuantizedTriple> Corners;
                Corners.Reserve(3);
                for (int32 i = 0; i < 3; ++i)
                {
                    const FVertexID Vid = Desc->GetVertexInstanceVertex(Vis[i]);
                    Corners.Add(QuantizePosition(VertexPositions[Vid], Scale));
                }
                Corners.Sort();
                FCanonTriangle CT;
                CT.A = Corners[0];
                CT.B = Corners[1];
                CT.C = Corners[2];
                CanonTriangles.Add(CT);
                CanonEdges.Add(MakeCanonEdge(Corners[0], Corners[1]));
                CanonEdges.Add(MakeCanonEdge(Corners[1], Corners[2]));
                CanonEdges.Add(MakeCanonEdge(Corners[0], Corners[2]));
            }
            CanonTriangles.Sort();
            CanonEdges.Sort();

            const int32 TriDesc = Desc->Triangles().Num();
            const int32 VertDesc = Desc->Vertices().Num();
            uint64 H = HashSortedTriples(VertexMultiset);
            H = Fnv1a64_Update(H, HashSortedCanonTriangles(CanonTriangles));
            H = Fnv1a64_Update(H, HashSortedCanonEdges(CanonEdges));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(TriDesc)));
            H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(VertDesc)));
            H |= GStrongGeometryFingerprintBit;
            OutFingerprint = H;
            return true;
        }

        if (TriRender < 0 || VertRender < 0)
        {
            return false;
        }

        const FBox B = Mesh->GetBoundingBox();
        if (!B.IsValid)
        {
            return false;
        }

        auto Qc = [](double V) -> int64
        {
            return static_cast<int64>(FMath::RoundToInt(V * 1000.0));
        };

        uint64 H = 2166136261ull;
        H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(TriRender)));
        H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(VertRender)));
        H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Min.X)));
        H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Min.Y)));
        H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Min.Z)));
        H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Max.X)));
        H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Max.Y)));
        H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Max.Z)));
        H &= ~GStrongGeometryFingerprintBit;
        OutFingerprint = H;
        return true;
    }

    static void GatherOrderedStaticMeshComponentsFiltered(
        const TArray<AActor*>& OrderedActors,
        TArray<UStaticMeshComponent*>& OutOrdered)
    {
        OutOrdered.Reset();
        for (AActor* Actor : OrderedActors)
        {
            if (!Actor || !IsValid(Actor))
            {
                continue;
            }
            if (!EditModelTool::ActorMatchesGlobalSessionFilter(Actor))
            {
                continue;
            }
            TArray<UStaticMeshComponent*> Comps;
            Actor->GetComponents<UStaticMeshComponent>(Comps);
            for (UStaticMeshComponent* C : Comps)
            {
                if (C && C->GetStaticMesh())
                {
                    OutOrdered.Add(C);
                }
            }
        }
    }

    static void BuildOrderedUniqueMeshesFromComponents(
        const TArray<UStaticMeshComponent*>& Smcs,
        TArray<UStaticMesh*>& OutUniqueMeshesInVisitOrder)
    {
        OutUniqueMeshesInVisitOrder.Reset();
        for (UStaticMeshComponent* Smc : Smcs)
        {
            UStaticMesh* Mesh = Smc ? Smc->GetStaticMesh() : nullptr;
            if (Mesh && !OutUniqueMeshesInVisitOrder.Contains(Mesh))
            {
                OutUniqueMeshesInVisitOrder.Add(Mesh);
            }
        }
    }

    struct FPickMasterState
    {
        UStaticMesh* Picked = nullptr;
        TWeakPtr<SWindow> HostWindow;
    };

    static bool ShowPickMasterMeshDialog(const TArray<UStaticMesh*>& Meshes, UStaticMesh*& OutPicked)
    {
        OutPicked = nullptr;
        if (Meshes.Num() == 0)
        {
            return false;
        }

        TSharedRef<FPickMasterState> State = MakeShared<FPickMasterState>();
        TSharedRef<SVerticalBox> MeshButtonList = SNew(SVerticalBox);
        for (UStaticMesh* M : Meshes)
        {
            if (!M)
            {
                continue;
            }
            MeshButtonList->AddSlot()
                .AutoHeight()
                .Padding(0.f, 0.f, 0.f, 4.f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(FString::Printf(
                        TEXT("%s  —  %s"),
                        *M->GetName(),
                        *M->GetPathName())))
                    .ToolTipText(FText::FromString(M->GetPathName()))
                    .OnClicked_Lambda([State, M]()
                    {
                        State->Picked = M;
                        if (const TSharedPtr<SWindow> W = State->HostWindow.Pin())
                        {
                            W->RequestDestroyWindow();
                        }
                        return FReply::Handled();
                    })
                ];
        }

        TSharedPtr<SWindow> Window = SNew(SWindow)
            .Title(LOCTEXT("PickMasterTitle", "Merge duplicate StaticMesh — pick first pool anchor"))
            .SizingRule(ESizingRule::Autosized)
            .SupportsMinimize(false)
            .SupportsMaximize(false)
            [
                SNew(SBox)
                .Padding(10.f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.f, 0.f, 0.f, 8.f)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .Text(LOCTEXT(
                            "PickMasterBody",
                            "That StaticMesh is moved to the front of the merge pool. "
                            "Sequential merge then runs: each round the first remaining asset is the anchor; any other pool asset with the same fingerprint is merged into the anchor; anchor and matches are removed from the pool."))
                    ]
                    + SVerticalBox::Slot()
                    .MaxHeight(360.f)
                    [
                        SNew(SScrollBox)
                        + SScrollBox::Slot()
                        [
                            MeshButtonList
                        ]
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.f, 8.f, 0.f, 0.f)
                    [
                        SNew(SUniformGridPanel)
                        .SlotPadding(4.f)
                        + SUniformGridPanel::Slot(0, 0)
                        [
                            SNew(SButton)
                            .Text(LOCTEXT("PickMasterCancel", "Cancel"))
                            .OnClicked_Lambda([State]()
                            {
                                State->Picked = nullptr;
                                if (const TSharedPtr<SWindow> W = State->HostWindow.Pin())
                                {
                                    W->RequestDestroyWindow();
                                }
                                return FReply::Handled();
                            })
                        ]
                    ]
                ]
            ];

        State->HostWindow = Window;
        FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(), nullptr);
        OutPicked = State->Picked;
        return OutPicked != nullptr;
    }

    static FString PolicyToDisplayString(const EEditModelToolMergeDuplicateMasterPolicy Policy)
    {
        switch (Policy)
        {
        case EEditModelToolMergeDuplicateMasterPolicy::FirstInSelectionOrder:
            return TEXT("錨點=池首（每輪第一個剩餘資產）/ anchor = first remaining in pool");
        case EEditModelToolMergeDuplicateMasterPolicy::LastInSelectionOrder:
            return TEXT("錨點=池尾（每輪最後一個剩餘資產）/ anchor = last remaining in pool");
        case EEditModelToolMergeDuplicateMasterPolicy::PickMasterFromDialog:
            return TEXT("自選資產置於池首後同上 / user-picked asset moved to front, then first-anchor rounds");
        default:
            return TEXT("(unknown)");
        }
    }

    static FString BuildSequentialMergeSummaryReport(
        const EEditModelToolMergeDuplicateMasterPolicy Policy,
        const int32 AnchorRounds,
        const int32 AssetCompareMatched,
        const int32 AssetCompareNoMatch,
        const int32 DuplicateAssetMappings,
        const int32 DistinctSourceAnchorMeshes,
        const int32 MaxDuplicateKindsPerSource,
        const int32 ComponentsChanged,
        const int32 SmcWithMesh,
        const int32 SmcNotReplaced)
    {
        FString Out;
        Out.Reserve(2560);
        Out.Append(TEXT("=== StaticMesh 序向合併 — 報告 ===\n\n"));
        Out.Append(FString::Printf(TEXT("策略 / Policy:\n  %s\n\n"), *PolicyToDisplayString(Policy)));

        const int32 TotalCompareChecks = AssetCompareMatched + AssetCompareNoMatch;
        Out.Append(TEXT("-- 比對過程累計（逐輪錨點、逐候選更新；非僅最終結果）--\n"));
        Out.Append(FString::Printf(
            TEXT("  錨點輪數 = 先後擔任「比對來源（錨點）」的互異 StaticMesh 種類數 / Distinct anchor meshes used as round source: %d\n"),
            AnchorRounds));
        Out.Append(TEXT("  （每輪一個 mesh 與池中其餘資產比指紋；每輪結束錨點離池，故輪數即累計出現過的來源 mesh「種類」數。）\n"));
        Out.Append(FString::Printf(
            TEXT("  指紋匹配累計（可併入錨點）/ Cumulative fingerprint matches: %d\n"),
            AssetCompareMatched));
        Out.Append(FString::Printf(
            TEXT("  指紋不匹配累計 / Cumulative non-matches: %d\n"),
            AssetCompareNoMatch));
        Out.Append(FString::Printf(
            TEXT("  錨點↔候選比對次數累計 / Total anchor-vs-candidate checks: %d\n"),
            TotalCompareChecks));
        Out.Append(FString::Printf(
            TEXT("  比對中曾吸納 ≥1 種重複的來源（錨點）mesh 種類數 / Distinct source meshes that absorbed duplicates: %d\n"),
            DistinctSourceAnchorMeshes));
        Out.Append(FString::Printf(
            TEXT("  累計建立 duplicate→anchor 對照（重複併入來源）次數 / Total duplicate→anchor mappings: %d\n"),
            DuplicateAssetMappings));
        if (DistinctSourceAnchorMeshes > 0)
        {
            Out.Append(FString::Printf(
                TEXT("  單一來源 mesh 最多吸納的重複「資產種類」數 / Max duplicate kinds merged into one source: %d\n"),
                MaxDuplicateKindsPerSource));
        }
        Out.Append(TEXT("\n"));
        if (DuplicateAssetMappings > 0)
        {
            Out.Append(FString::Printf(
                TEXT("  摘要：全流程共 %d 種來源 mesh 擔任過錨點比對；其中 %d 種實際收斂了重複；%d 種重複資產併入這些來源。\n"),
                AnchorRounds,
                DistinctSourceAnchorMeshes,
                DuplicateAssetMappings));
            Out.Append(FString::Printf(
                TEXT("  Summary: %d anchor rounds (distinct round sources); %d source(s) absorbed ≥1 duplicate; %d duplicate kinds merged.\n\n"),
                AnchorRounds,
                DistinctSourceAnchorMeshes,
                DuplicateAssetMappings));
        }
        else
        {
            Out.Append(TEXT("  （比對過程未產生任何 duplicate→anchor 合併 / no merges during compare）\n\n"));
        }

        Out.Append(TEXT("-- 選取範圍內 StaticMeshComponent --\n"));
        Out.Append(FString::Printf(TEXT("  有指派 mesh 的元件數 / SMC with mesh: %d\n"), SmcWithMesh));
        Out.Append(FString::Printf(TEXT("  成功替換引用 / Replaced (SetStaticMesh): %d\n"), ComponentsChanged));
        Out.Append(FString::Printf(TEXT("  未替換元件 / Unchanged SMC: %d\n\n"), SmcNotReplaced));
        Out.Append(TEXT("\n強指紋: LOD0 MeshDescription；弱指紋: 三角數+頂點數+包圍盒。未刪除 Content .uasset。\n"));
        return Out;
    }

    /** Slate can struggle with huge FText blocks; preview is capped but clipboard still gets the full string. */
    static constexpr int32 GMergeReportMaxPreviewChars = 350000;

    static TSharedRef<FString> MakeMergeReportPreviewText(const TSharedRef<FString>& FullReport)
    {
        if (FullReport->Len() <= GMergeReportMaxPreviewChars)
        {
            return FullReport;
        }
        FString Preview = FullReport->Left(GMergeReportMaxPreviewChars);
        Preview.Append(
            TEXT("\n\n--- [Preview truncated for Slate; use \"Copy report\" for the full log] ---\n"));
        return MakeShared<FString>(MoveTemp(Preview));
    }

    static void ShowSequentialMergeReportWindow_Impl(const TSharedRef<FString>& FullReport)
    {
        if (!FSlateApplication::IsInitialized())
        {
            return;
        }

        const TSharedRef<FString> PreviewText = MakeMergeReportPreviewText(FullReport);

        // Slate lambdas must not capture [&] to a local TSharedPtr<SWindow>: the stack slot is dead after this
        // function returns. Hold a weak ref on the heap so Close can Pin() after the window is constructed.
        struct FReportWindowHolder
        {
            TWeakPtr<SWindow> WindowWeak;
        };
        const TSharedRef<FReportWindowHolder> Holder = MakeShared<FReportWindowHolder>();
        const TSharedPtr<SWindow> Window = SNew(SWindow)
            .Title(LOCTEXT("MergeReportWinTitle", "StaticMesh 序向合併 — 報告"))
            .ClientSize(FVector2D(960, 720))
            .SizingRule(ESizingRule::UserSized)
            .SupportsMaximize(true)
            .SupportsMinimize(false)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .FillHeight(1.f)
                .Padding(8.f, 8.f, 8.f, 4.f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    .Padding(4.f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(*PreviewText))
                        .AutoWrapText(true)
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(8.f, 0.f, 8.f, 8.f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(0.f, 0.f, 8.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("MergeReportCopyBtn", "複製報告到剪貼簿 / Copy report"))
                        .OnClicked_Lambda([FullReport]()
                        {
                            FPlatformApplicationMisc::ClipboardCopy(*(*FullReport));
                            return FReply::Handled();
                        })
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("MergeReportCloseBtn", "關閉 / Close"))
                        .OnClicked_Lambda([Holder]()
                        {
                            if (const TSharedPtr<SWindow> Pinned = Holder->WindowWeak.Pin())
                            {
                                Pinned->RequestDestroyWindow();
                            }
                            return FReply::Handled();
                        })
                    ]
                ]
            ];
        Holder->WindowWeak = Window;

        if (Window.IsValid())
        {
            // Do not nest AddModalWindow from inside another Slate modal (batch dialog); use a normal window.
            FSlateApplication::Get().AddWindow(Window.ToSharedRef(), true);
        }
    }

    static void ShowSequentialMergeReportWindow(const FString& ReportBody)
    {
        TSharedRef<FString> FullReport = MakeShared<FString>(ReportBody);
        // Core ticker callbacks are not guaranteed to run on the game thread in all contexts; Slate
        // (AddWindow, widget construction) must run on the game thread. Defer via the game-thread task graph.
        AsyncTask(ENamedThreads::GameThread, [FullReport]()
        {
            ShowSequentialMergeReportWindow_Impl(FullReport);
        });
    }

    static void RunMergeImpl(EEditModelToolMergeDuplicateMasterPolicy Policy, UStaticMesh* UserMasterOrNull)
    {
        if (!GEditor)
        {
            return;
        }

        TArray<AActor*> OrderedActors;
        EditModelToolSelectionUtils::GatherSelectedActors(OrderedActors);
        TArray<UStaticMeshComponent*> Smcs;
        GatherOrderedStaticMeshComponentsFiltered(OrderedActors, Smcs);
        if (Smcs.Num() == 0)
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                LOCTEXT("MergeNoMeshComps", "No StaticMesh components with assigned meshes found in the current selection (after session filter)."));
            return;
        }

        TMap<UStaticMesh*, uint64> AssetToFingerprint;
        for (UStaticMeshComponent* Smc : Smcs)
        {
            UStaticMesh* Mesh = Smc->GetStaticMesh();
            if (!Mesh || AssetToFingerprint.Contains(Mesh))
            {
                continue;
            }
            uint64 Fp = 0;
            if (!TryComputeGeometryFingerprint(Mesh, Fp))
            {
                FMessageDialog::Open(
                    EAppMsgType::Ok,
                    FText::Format(
                        LOCTEXT("MergeBadFingerprintFmt", "Could not compute geometry fingerprint for mesh asset:\n{0}\n\nOperation aborted."),
                        FText::FromString(Mesh->GetPathName())));
                return;
            }
            AssetToFingerprint.Add(Mesh, Fp);
        }

        TArray<UStaticMesh*> Pool;
        BuildOrderedUniqueMeshesFromComponents(Smcs, Pool);
        if (Pool.Num() < 2)
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                LOCTEXT("MergeNeedTwoUniqueAssets", "Sequential merge needs at least two different StaticMesh assets in the selection."));
            return;
        }

        if (Policy == EEditModelToolMergeDuplicateMasterPolicy::PickMasterFromDialog)
        {
            const int32 Idx = Pool.IndexOfByKey(UserMasterOrNull);
            if (Idx == INDEX_NONE || UserMasterOrNull == nullptr)
            {
                FMessageDialog::Open(
                    EAppMsgType::Ok,
                    LOCTEXT("MergePickMasterMissing", "Chosen master mesh is not in the current selection pool."));
                return;
            }
            Pool.RemoveAt(Idx);
            Pool.Insert(UserMasterOrNull, 0);
        }

        TMap<UStaticMesh*, UStaticMesh*> DuplicateToAnchor;
        TSet<UStaticMesh*> AnchorsThatAbsorbedDuplicate;
        TMap<UStaticMesh*, int32> DupKindsMergedPerAnchor;
        int32 AnchorRoundCount = 0;
        int32 AssetCompareMatched = 0;
        int32 AssetCompareNoMatch = 0;
        while (Pool.Num() > 0)
        {
            ++AnchorRoundCount;

            const bool bAnchorFirst = (Policy != EEditModelToolMergeDuplicateMasterPolicy::LastInSelectionOrder);
            UStaticMesh* Anchor = bAnchorFirst ? Pool[0] : Pool.Last();
            const uint64* FpAnchorPtr = AssetToFingerprint.Find(Anchor);
            if (!FpAnchorPtr)
            {
                FMessageDialog::Open(
                    EAppMsgType::Ok,
                    LOCTEXT("MergeInternalMissingFp", "Internal error: missing fingerprint for anchor mesh."));
                return;
            }
            const uint64 FpAnchor = *FpAnchorPtr;

            TSet<UStaticMesh*> RemoveFromPool;
            RemoveFromPool.Add(Anchor);

            for (UStaticMesh* Candidate : Pool)
            {
                if (Candidate == Anchor)
                {
                    continue;
                }
                const uint64* FpCandPtr = AssetToFingerprint.Find(Candidate);
                const bool bMatched = FpCandPtr && (*FpCandPtr == FpAnchor);
                if (bMatched)
                {
                    ++AssetCompareMatched;
                    DuplicateToAnchor.Add(Candidate, Anchor);
                    RemoveFromPool.Add(Candidate);
                    AnchorsThatAbsorbedDuplicate.Add(Anchor);
                    ++DupKindsMergedPerAnchor.FindOrAdd(Anchor);
                }
                else
                {
                    ++AssetCompareNoMatch;
                }
            }

            TArray<UStaticMesh*> NextPool;
            NextPool.Reserve(Pool.Num());
            for (UStaticMesh* P : Pool)
            {
                if (!RemoveFromPool.Contains(P))
                {
                    NextPool.Add(P);
                }
            }
            Pool = MoveTemp(NextPool);
        }

        const int32 DistinctSourceAnchorMeshes = AnchorsThatAbsorbedDuplicate.Num();
        int32 MaxDuplicateKindsPerSource = 0;
        for (const TPair<UStaticMesh*, int32>& Kvp : DupKindsMergedPerAnchor)
        {
            MaxDuplicateKindsPerSource = FMath::Max(MaxDuplicateKindsPerSource, Kvp.Value);
        }

        const int32 SmcWithMesh = Smcs.Num();

        int32 ComponentsChanged = 0;
        if (DuplicateToAnchor.Num() > 0)
        {
            const FScopedTransaction Transaction(LOCTEXT("MergeDupStaticMeshTx", "Merge duplicate StaticMesh references"));
            for (UStaticMeshComponent* Smc : Smcs)
            {
                UStaticMesh* Mesh = Smc->GetStaticMesh();
                if (!Mesh)
                {
                    continue;
                }
                if (UStaticMesh** AnchorPtr = DuplicateToAnchor.Find(Mesh))
                {
                    UStaticMesh* TargetAnchor = *AnchorPtr;
                    if (TargetAnchor && TargetAnchor != Mesh)
                    {
                        Smc->Modify();
                        Smc->SetStaticMesh(TargetAnchor);
                        ++ComponentsChanged;
                    }
                }
            }
        }

        const int32 SmcNotReplaced = FMath::Max(0, SmcWithMesh - ComponentsChanged);
        const FString FullReport = BuildSequentialMergeSummaryReport(
            Policy,
            AnchorRoundCount,
            AssetCompareMatched,
            AssetCompareNoMatch,
            DuplicateToAnchor.Num(),
            DistinctSourceAnchorMeshes,
            MaxDuplicateKindsPerSource,
            ComponentsChanged,
            SmcWithMesh,
            SmcNotReplaced);
        ShowSequentialMergeReportWindow(FullReport);
    }
}

void EditModelToolMergeDuplicateStaticMesh::RunMergeDuplicateStaticMeshes(const EEditModelToolMergeDuplicateMasterPolicy Policy)
{
    UStaticMesh* UserMaster = nullptr;
    if (Policy == EEditModelToolMergeDuplicateMasterPolicy::PickMasterFromDialog)
    {
        TArray<AActor*> OrderedActors;
        EditModelToolSelectionUtils::GatherSelectedActors(OrderedActors);
        TArray<UStaticMeshComponent*> Smcs;
        GatherOrderedStaticMeshComponentsFiltered(OrderedActors, Smcs);
        TArray<UStaticMesh*> UniqueMeshes;
        BuildOrderedUniqueMeshesFromComponents(Smcs, UniqueMeshes);
        if (UniqueMeshes.Num() < 2)
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                LOCTEXT("MergePickNeedTwoAssets", "Pick-master merge needs at least two different StaticMesh assets in the selection."));
            return;
        }
        if (!ShowPickMasterMeshDialog(UniqueMeshes, UserMaster) || !UserMaster)
        {
            return;
        }
    }

    RunMergeImpl(Policy, UserMaster);
}

TSharedRef<SWidget> EditModelToolMergeDuplicateStaticMesh::BuildMergeDuplicateStaticMeshSection()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 8.f, 0.f, 0.f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text(LOCTEXT(
                "MergeDupHint",
                "Merge duplicate StaticMesh (selection): unique assets in outliner order form a pool. "
                "Each round the first (or last) remaining asset is the anchor; every other pool asset with the same LOD0 fingerprint is merged into the anchor, then anchor and matches are removed from the pool — repeat until the pool is empty. "
                "Strong fingerprint uses MeshDescription vertices + triangle triples + edge pairs (quantized); weak fallback uses tri/vert counts + bounds. Does not delete .uasset files."))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f, 0.f, 0.f)
        [
            SNew(SUniformGridPanel)
            .SlotPadding(2.f)
            + SUniformGridPanel::Slot(0, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("MergeDupFirst", "Merge dup: sequential (anchor = first in pool)"))
                .ToolTipText(LOCTEXT(
                    "MergeDupFirstTT",
                    "Builds an ordered pool of unique StaticMesh assets (first occurrence along selection). "
                    "Repeatedly: the first remaining asset is the anchor; any other pool asset with the same fingerprint is rewired to that anchor on all selected components; anchor and all matches are removed from the pool. Then the next first-remaining becomes anchor, and so on."))
                .OnClicked_Lambda([]()
                {
                    RunMergeDuplicateStaticMeshes(EEditModelToolMergeDuplicateMasterPolicy::FirstInSelectionOrder);
                    return FReply::Handled();
                })
            ]
            + SUniformGridPanel::Slot(1, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("MergeDupLast", "Merge dup: sequential (anchor = last in pool)"))
                .ToolTipText(LOCTEXT(
                    "MergeDupLastTT",
                    "Same sequential pool merge as the first-anchor button, except each round uses the last remaining asset in the pool as the anchor and compares it against all others still in the pool."))
                .OnClicked_Lambda([]()
                {
                    RunMergeDuplicateStaticMeshes(EEditModelToolMergeDuplicateMasterPolicy::LastInSelectionOrder);
                    return FReply::Handled();
                })
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f, 0.f, 0.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("MergeDupPick", "Merge dup: pick first anchor, then sequential…"))
            .ToolTipText(LOCTEXT(
                "MergeDupPickTT",
                "Pick one StaticMesh asset from the selection; it is moved to the front of the pool, then the same sequential first-anchor merge runs on the shrinking pool."))
            .OnClicked_Lambda([]()
            {
                RunMergeDuplicateStaticMeshes(EEditModelToolMergeDuplicateMasterPolicy::PickMasterFromDialog);
                return FReply::Handled();
            })
        ];
}

#undef LOCTEXT_NAMESPACE
