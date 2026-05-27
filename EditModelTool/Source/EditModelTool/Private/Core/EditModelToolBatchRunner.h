#pragma once

#include "CoreMinimal.h"

#include "Containers/Ticker.h"

struct FEditModelToolBatchContext
{
    int32 TotalItems = 0;
    int32 BatchSize = 32;
    int32 CurrentIndex = 0;
    double StartedAtSeconds = 0.0;
    bool bCancelRequested = false;
};

struct FEditModelToolBatchHooks
{
    TFunction<void(FEditModelToolBatchContext&)> OnStart;
    TFunction<void(FEditModelToolBatchContext&, int32, int32)> OnChunk;
    TFunction<void(FEditModelToolBatchContext&, int32, int32, double)> OnProgress;
    TFunction<void(FEditModelToolBatchContext&)> OnComplete;
};

class FEditModelToolBatchRunner
{
public:
    static FTSTicker::FDelegateHandle Start(const FEditModelToolBatchHooks& Hooks, FEditModelToolBatchContext&& Context);
};
