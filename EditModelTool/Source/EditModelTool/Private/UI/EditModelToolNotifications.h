#pragma once

#include "CoreMinimal.h"

class SNotificationItem;

namespace EditModelToolNotifications
{
TSharedPtr<SNotificationItem> ShowProgress(const FText& Title);
void UpdateProgress(const TSharedPtr<SNotificationItem>& Item, const FText& StatusText, float Completion);
void Complete(const TSharedPtr<SNotificationItem>& Item, const FText& StatusText, bool bSuccess);
}
