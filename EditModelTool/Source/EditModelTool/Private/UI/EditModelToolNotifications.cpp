#include "UI/EditModelToolNotifications.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace EditModelToolNotifications
{
TSharedPtr<SNotificationItem> ShowProgress(const FText& Title)
{
    FNotificationInfo Info(Title);
    Info.bFireAndForget = false;
    Info.ExpireDuration = 0.0f;
    Info.bUseLargeFont = false;
    TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (Item.IsValid())
    {
        Item->SetCompletionState(SNotificationItem::CS_Pending);
    }
    return Item;
}

void UpdateProgress(const TSharedPtr<SNotificationItem>& Item, const FText& StatusText, float Completion)
{
    if (!Item.IsValid())
    {
        return;
    }
    Item->SetText(StatusText);
    Item->SetSubText(FText::AsPercent(FMath::Clamp(Completion, 0.0f, 1.0f)));
}

void Complete(const TSharedPtr<SNotificationItem>& Item, const FText& StatusText, bool bSuccess)
{
    if (!Item.IsValid())
    {
        return;
    }

    Item->SetText(StatusText);
    Item->SetSubText(FText::GetEmpty());
    Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
    Item->ExpireAndFadeout();
}
}
