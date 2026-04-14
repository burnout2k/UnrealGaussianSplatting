#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class AGaussianSplatActor;
class UGaussianSplatAsset;
class UGaussianSplatComponent;

class SGaussianSplatEditorPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SGaussianSplatEditorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    AGaussianSplatActor* GetSelectedGaussianActor() const;
    UGaussianSplatComponent* GetSelectedGaussianComponent() const;
    UGaussianSplatAsset* GetSelectedGaussianAsset() const;

    FText GetSelectionSummaryText() const;
    FText GetStatsText() const;
    FText GetBoundsText() const;
    FText GetComponentSettingsText() const;

    FReply RefreshSelectedAsset();
    FReply RebuildSelectedAssetBounds();
    FReply FocusSelectedActor();
    FReply PlaceSelectedActorAtOrigin();
    FReply PlaceSelectedActorInFrontOfViewport();
    FReply SetSelectedComponentToPoints();
    FReply SetSelectedComponentToBillboards();
};
