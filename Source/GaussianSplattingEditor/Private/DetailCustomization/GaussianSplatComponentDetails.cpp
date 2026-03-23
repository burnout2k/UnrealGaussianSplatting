#include "DetailCustomization/GaussianSplatComponentDetails.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatComponent.h"
#include "DetailCategoryBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IDetailCustomization> FGaussianSplatComponentDetails::MakeInstance()
{
	return MakeShared<FGaussianSplatComponentDetails>();
}

void FGaussianSplatComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	UGaussianSplatComponent* Component = nullptr;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		Component = Cast<UGaussianSplatComponent>(Object.Get());
		if (Component)
		{
			break;
		}
	}

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Gaussian Splat|Component"));

	Category.AddCustomRow(FText::FromString(TEXT("Asset Status")))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text_Lambda([Component]()
		{
			if (!Component)
			{
				return FText::FromString(TEXT("No component selected"));
			}

			const UGaussianSplatAsset* Asset = Component->Asset;
			if (!Asset)
			{
				return FText::FromString(TEXT("Asset: None"));
			}

			return FText::FromString(FString::Printf(TEXT("Asset Points: %d"), Asset->GetPointCount()));
		})
	];

	Category.AddCustomRow(FText::FromString(TEXT("Rebuild Render State")))
	.WholeRowContent()
	[
		SNew(SButton)
		.Text(FText::FromString(TEXT("Rebuild Render State")))
		.OnClicked_Lambda([Component]()
		{
			if (Component)
			{
				Component->MarkRenderStateDirty();
			}
			return FReply::Handled();
		})
	];
}
