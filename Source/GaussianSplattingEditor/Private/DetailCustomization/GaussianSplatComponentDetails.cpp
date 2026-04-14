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
	// 找到当前正在编辑的 GaussianSplatComponent。
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

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Gaussian Splat"));

	// 展示当前组件是否绑定了 Asset，以及绑定 Asset 后的点数。
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

	// 手动触发渲染状态重建，方便调试 SceneProxy / ViewExtension 是否同步到最新状态。
	Category.AddCustomRow(FText::FromString(TEXT("Refresh Component Rendering")))
	.WholeRowContent()
	[
		SNew(SButton)
		.Text(FText::FromString(TEXT("Refresh Component Rendering")))
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
