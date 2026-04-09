#pragma once

#include "IDetailCustomization.h"

// 自定义 Asset 详情面板，用于把点数、重建 Bounds 按钮等常用信息摆到更直观的位置。
class FGaussianSplatAssetDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	// UE 构建 Details 面板时会调用这里拼装自定义 UI。
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
