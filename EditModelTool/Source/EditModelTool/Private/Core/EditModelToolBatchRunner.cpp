#include "Core/EditModelToolBatchRunner.h"

#include "HAL/PlatformTime.h"

FTSTicker::FDelegateHandle FEditModelToolBatchRunner::Start(const FEditModelToolBatchHooks& Hooks, FEditModelToolBatchContext&& Context)
{
    TSharedRef<FEditModelToolBatchContext> SharedContext = MakeShared<FEditModelToolBatchContext>(MoveTemp(Context));
    SharedContext->StartedAtSeconds = FPlatformTime::Seconds();

    if (Hooks.OnStart)
    {
        Hooks.OnStart(*SharedContext);
    }

    return FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([SharedContext, Hooks](float)
    {
        if (SharedContext->bCancelRequested)
        {
            if (Hooks.OnComplete)
            {
                Hooks.OnComplete(*SharedContext);
            }
            return false;
        }

        const int32 StartIndex = SharedContext->CurrentIndex;
        const int32 EndIndex = FMath::Min(StartIndex + SharedContext->BatchSize, SharedContext->TotalItems);
        if (StartIndex < EndIndex && Hooks.OnChunk)
        {
            Hooks.OnChunk(*SharedContext, StartIndex, EndIndex);
        }

        SharedContext->CurrentIndex = EndIndex;

        const int32 Processed = SharedContext->CurrentIndex;
        const int32 Remaining = FMath::Max(0, SharedContext->TotalItems - Processed);
        const double Elapsed = FMath::Max(0.001, FPlatformTime::Seconds() - SharedContext->StartedAtSeconds);
        const double PerItem = static_cast<double>(Processed) / Elapsed;
        const double EtaSeconds = Remaining / FMath::Max(0.001, PerItem);
        if (Hooks.OnProgress)
        {
            Hooks.OnProgress(*SharedContext, Processed, SharedContext->TotalItems, EtaSeconds);
        }

        if (SharedContext->CurrentIndex >= SharedContext->TotalItems)
        {
            if (Hooks.OnComplete)
            {
                Hooks.OnComplete(*SharedContext);
            }
            return false;
        }

        return true;
    }));
}
