#pragma once

#include "IDetailCustomization.h"

// 自定义 Component 详情面板，用于展示绑定 Asset 状态并提供渲染状态重建按钮。
class FGaussianSplatComponentDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	// UE 构建 Details 面板时会调用这里拼装自定义 UI。
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
