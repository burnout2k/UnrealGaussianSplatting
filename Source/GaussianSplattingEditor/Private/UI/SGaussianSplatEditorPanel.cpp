#include "UI/SGaussianSplatEditorPanel.h"

#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "AssetRegistry/AssetData.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "IContentBrowserSingleton.h"
#include "LevelEditorViewport.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"

void SGaussianSplatEditorPanel::Construct(const FArguments& InArgs)
{
    ChildSlot
    [
        SNew(SBox)
        .Padding(12.0f)
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Gaussian Splat Editor")))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(this, &SGaussianSplatEditorPanel::GetSelectionSummaryText)
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(this, &SGaussianSplatEditorPanel::GetStatsText)
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(this, &SGaussianSplatEditorPanel::GetBoundsText)
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(STextBlock)
                .Text(this, &SGaussianSplatEditorPanel::GetComponentSettingsText)
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(SSeparator)
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Asset Actions")))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 10.0f)
            [
                SNew(SUniformGridPanel)
                .SlotPadding(4.0f)

                + SUniformGridPanel::Slot(0, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Refresh Asset")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::RefreshSelectedAsset)
                ]

                + SUniformGridPanel::Slot(1, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Rebuild Bounds")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::RebuildSelectedAssetBounds)
                ]
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Actor Actions")))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 10.0f)
            [
                SNew(SUniformGridPanel)
                .SlotPadding(4.0f)

                + SUniformGridPanel::Slot(0, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Focus View On Actor")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::FocusSelectedActor)
                ]

                + SUniformGridPanel::Slot(1, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Move Actor To Origin")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::PlaceSelectedActorAtOrigin)
                ]

                + SUniformGridPanel::Slot(0, 1)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Move Actor To View")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::PlaceSelectedActorInFrontOfViewport)
                ]
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Preview Mode")))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SUniformGridPanel)
                .SlotPadding(4.0f)

                + SUniformGridPanel::Slot(0, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Points")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::SetSelectedComponentToPoints)
                ]

                + SUniformGridPanel::Slot(1, 0)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Billboards")))
                    .OnClicked(this, &SGaussianSplatEditorPanel::SetSelectedComponentToBillboards)
                ]
            ]
        ]
    ];
}

AGaussianSplatActor* SGaussianSplatEditorPanel::GetSelectedGaussianActor() const
{
    if (!GEditor)
    {
        return nullptr;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return nullptr;
    }

    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (AGaussianSplatActor* Actor = Cast<AGaussianSplatActor>(*It))
        {
            return Actor;
        }
    }

    return nullptr;
}

UGaussianSplatComponent* SGaussianSplatEditorPanel::GetSelectedGaussianComponent() const
{
    if (!GEditor)
    {
        return nullptr;
    }

    USelection* SelectedComponents = GEditor->GetSelectedComponents();
    if (SelectedComponents)
    {
        for (FSelectionIterator It(*SelectedComponents); It; ++It)
        {
            if (UGaussianSplatComponent* Component = Cast<UGaussianSplatComponent>(*It))
            {
                return Component;
            }
        }
    }

    if (AGaussianSplatActor* Actor = GetSelectedGaussianActor())
    {
        return Actor->SplatComponent;
    }

    return nullptr;
}

UGaussianSplatAsset* SGaussianSplatEditorPanel::GetSelectedGaussianAsset() const
{
    if (UGaussianSplatComponent* Component = GetSelectedGaussianComponent())
    {
        return Component->Asset;
    }

    if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
    {
        FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
        TArray<FAssetData> SelectedAssets;
        ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
        for (const FAssetData& AssetData : SelectedAssets)
        {
            if (UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(AssetData.GetAsset()))
            {
                return Asset;
            }
        }
    }

    return nullptr;
}

FText SGaussianSplatEditorPanel::GetSelectionSummaryText() const
{
    const AGaussianSplatActor* Actor = GetSelectedGaussianActor();
    const UGaussianSplatComponent* Component = GetSelectedGaussianComponent();
    const UGaussianSplatAsset* Asset = GetSelectedGaussianAsset();

    if (Actor)
    {
        return FText::FromString(FString::Printf(TEXT("Actor: %s"), *Actor->GetName()));
    }

    if (Component)
    {
        return FText::FromString(FString::Printf(TEXT("Component: %s"), *Component->GetName()));
    }

    if (Asset)
    {
        return FText::FromString(FString::Printf(TEXT("Asset: %s"), *Asset->GetName()));
    }

    return FText::FromString(TEXT("Selection: choose a Gaussian actor, component, or asset in Content Browser."));
}

FText SGaussianSplatEditorPanel::GetStatsText() const
{
    const UGaussianSplatAsset* Asset = GetSelectedGaussianAsset();
    if (!Asset)
    {
        return FText::FromString(TEXT("Stats: no Gaussian asset selected."));
    }

    return FText::FromString(FString::Printf(
        TEXT("Points: %d | Covariances: %d | Colors: %d | SH Floats: %d"),
        Asset->Positions.Num(),
        Asset->Covariances.Num(),
        Asset->ColorsOpacity.Num(),
        Asset->SHCoefficients.Num()));
}

FText SGaussianSplatEditorPanel::GetBoundsText() const
{
    const UGaussianSplatAsset* Asset = GetSelectedGaussianAsset();
    if (!Asset)
    {
        return FText::FromString(TEXT("Bounds: unavailable."));
    }

    const FVector Origin = Asset->Bounds.Origin;
    const FVector Extent = Asset->Bounds.BoxExtent;
    return FText::FromString(FString::Printf(
        TEXT("Bounds Origin: X=%.2f Y=%.2f Z=%.2f | Extent: X=%.2f Y=%.2f Z=%.2f"),
        Origin.X, Origin.Y, Origin.Z,
        Extent.X, Extent.Y, Extent.Z));
}

FText SGaussianSplatEditorPanel::GetComponentSettingsText() const
{
    const UGaussianSplatComponent* Component = GetSelectedGaussianComponent();
    if (!Component)
    {
        return FText::FromString(TEXT("Component Settings: unavailable."));
    }

    const TCHAR* Mode = Component->PreviewRenderMode == EGaussianPreviewRenderMode::Points
        ? TEXT("Points")
        : TEXT("Billboards");
    return FText::FromString(FString::Printf(
        TEXT("Mode: %s | Density: %.2f | Opacity: %.2f | PointSize: %.2f"),
        Mode,
        Component->DensityScale,
        Component->OpacityScale,
        Component->PointSize));
}

FReply SGaussianSplatEditorPanel::RefreshSelectedAsset()
{
    if (UGaussianSplatAsset* Asset = GetSelectedGaussianAsset())
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Refresh Gaussian Splat Asset")));
        Asset->Modify();
        Asset->RefreshDerivedData();
        Asset->MarkPackageDirty();
    }

    return FReply::Handled();
}

FReply SGaussianSplatEditorPanel::RebuildSelectedAssetBounds()
{
    if (UGaussianSplatAsset* Asset = GetSelectedGaussianAsset())
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Rebuild Gaussian Splat Bounds")));
        Asset->Modify();
        Asset->RebuildBounds();
        Asset->MarkPackageDirty();
    }

    return FReply::Handled();
}

FReply SGaussianSplatEditorPanel::FocusSelectedActor()
{
    if (AGaussianSplatActor* Actor = GetSelectedGaussianActor())
    {
        GEditor->SelectNone(false, true, false);
        GEditor->SelectActor(Actor, true, true, true);
        GEditor->MoveViewportCamerasToActor(*Actor, false);
    }

    return FReply::Handled();
}

FReply SGaussianSplatEditorPanel::PlaceSelectedActorAtOrigin()
{
    AGaussianSplatActor* Actor = GetSelectedGaussianActor();
    UGaussianSplatAsset* Asset = GetSelectedGaussianAsset();
    if (Actor && Asset)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Place Gaussian Splat At Origin")));
        Actor->Modify();
        Actor->SetActorLocation(-Asset->Bounds.Origin);
    }

    return FReply::Handled();
}

FReply SGaussianSplatEditorPanel::PlaceSelectedActorInFrontOfViewport()
{
    AGaussianSplatActor* Actor = GetSelectedGaussianActor();
    UGaussianSplatAsset* Asset = GetSelectedGaussianAsset();
    if (Actor && Asset && GCurrentLevelEditingViewportClient)
    {
        const FVector ViewLocation = GCurrentLevelEditingViewportClient->GetViewLocation();
        const FVector Forward = GCurrentLevelEditingViewportClient->GetViewRotation().Vector();
        const FVector TargetLocation = ViewLocation + Forward * 300.0f;

        const FScopedTransaction Transaction(FText::FromString(TEXT("Place Gaussian Splat In Front Of Viewport")));
        Actor->Modify();
        Actor->SetActorLocation(TargetLocation - Asset->Bounds.Origin);
    }

    return FReply::Handled();
}

FReply SGaussianSplatEditorPanel::SetSelectedComponentToPoints()
{
    if (UGaussianSplatComponent* Component = GetSelectedGaussianComponent())
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Set Gaussian Preview Mode To Points")));
        Component->Modify();
        Component->SetPreviewRenderMode(EGaussianPreviewRenderMode::Points);
    }

    return FReply::Handled();
}

FReply SGaussianSplatEditorPanel::SetSelectedComponentToBillboards()
{
    if (UGaussianSplatComponent* Component = GetSelectedGaussianComponent())
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Set Gaussian Preview Mode To Billboards")));
        Component->Modify();
        Component->SetPreviewRenderMode(EGaussianPreviewRenderMode::Billboards);
    }

    return FReply::Handled();
}
