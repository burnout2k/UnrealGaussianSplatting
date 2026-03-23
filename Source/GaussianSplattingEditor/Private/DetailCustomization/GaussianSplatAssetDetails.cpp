#include "DetailCustomization/GaussianSplatAssetDetails.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "GaussianSplatAsset.h"
#include "DetailCategoryBuilder.h"
#include "Misc/Attribute.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IDetailCustomization> FGaussianSplatAssetDetails::MakeInstance()
{
	return MakeShared<FGaussianSplatAssetDetails>();
}

void FGaussianSplatAssetDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	UGaussianSplatAsset* Asset = nullptr;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		Asset = Cast<UGaussianSplatAsset>(Object.Get());
		if (Asset)
		{
			break;
		}
	}

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Gaussian Splat|Asset"));

	Category.AddCustomRow(FText::FromString(TEXT("Point Count")))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text_Lambda([Asset]()
		{
			const int32 PointCount = Asset ? Asset->GetPointCount() : 0;
			return FText::FromString(FString::Printf(TEXT("Points: %d"), PointCount));
		})
	];

	Category.AddCustomRow(FText::FromString(TEXT("Rebuild Bounds")))
	.WholeRowContent()
	[
		SNew(SButton)
		.Text(FText::FromString(TEXT("Rebuild Bounds")))
		.OnClicked_Lambda([Asset]()
		{
			if (!Asset)
			{
				return FReply::Handled();
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Rebuild Gaussian Splat Bounds")));
			Asset->Modify();
			Asset->RebuildBounds();
			Asset->MarkPackageDirty();
			return FReply::Handled();
		})
	];
}
