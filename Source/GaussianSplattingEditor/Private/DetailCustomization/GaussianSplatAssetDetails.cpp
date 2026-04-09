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
	// 收集当前 Details 面板正在编辑的对象，并找到第一个 GaussianSplatAsset。
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

	// 只读显示点数，帮助用户快速确认导入是否成功。
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

	// 提供一个手动重建 Bounds 的按钮，便于导入后或手改数据后重新校准包围体。
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
			// 这里只重建 Bounds，不重建整套 GPU 资源，因为这个按钮的目标就是修正包围体。
			Asset->MarkPackageDirty();
			return FReply::Handled();
		})
	];
}
