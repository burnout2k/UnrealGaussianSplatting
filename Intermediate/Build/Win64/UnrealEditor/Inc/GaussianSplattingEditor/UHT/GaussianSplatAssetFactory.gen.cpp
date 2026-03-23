// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GaussianSplattingEditor/Public/GaussianSplatAssetFactory.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodeGaussianSplatAssetFactory() {}

// Begin Cross Module References
GAUSSIANSPLATTINGEDITOR_API UClass* Z_Construct_UClass_UGaussianSplatAssetFactory();
GAUSSIANSPLATTINGEDITOR_API UClass* Z_Construct_UClass_UGaussianSplatAssetFactory_NoRegister();
UNREALED_API UClass* Z_Construct_UClass_UFactory();
UPackage* Z_Construct_UPackage__Script_GaussianSplattingEditor();
// End Cross Module References

// Begin Class UGaussianSplatAssetFactory
void UGaussianSplatAssetFactory::StaticRegisterNativesUGaussianSplatAssetFactory()
{
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(UGaussianSplatAssetFactory);
UClass* Z_Construct_UClass_UGaussianSplatAssetFactory_NoRegister()
{
	return UGaussianSplatAssetFactory::StaticClass();
}
struct Z_Construct_UClass_UGaussianSplatAssetFactory_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "IncludePath", "GaussianSplatAssetFactory.h" },
		{ "ModuleRelativePath", "Public/GaussianSplatAssetFactory.h" },
	};
#endif // WITH_METADATA
	static UObject* (*const DependentSingletons[])();
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UGaussianSplatAssetFactory>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
UObject* (*const Z_Construct_UClass_UGaussianSplatAssetFactory_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_UFactory,
	(UObject* (*)())Z_Construct_UPackage__Script_GaussianSplattingEditor,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatAssetFactory_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_UGaussianSplatAssetFactory_Statics::ClassParams = {
	&UGaussianSplatAssetFactory::StaticClass,
	nullptr,
	&StaticCppClassTypeInfo,
	DependentSingletons,
	nullptr,
	nullptr,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	0,
	0,
	0,
	0x001000A0u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatAssetFactory_Statics::Class_MetaDataParams), Z_Construct_UClass_UGaussianSplatAssetFactory_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_UGaussianSplatAssetFactory()
{
	if (!Z_Registration_Info_UClass_UGaussianSplatAssetFactory.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UGaussianSplatAssetFactory.OuterSingleton, Z_Construct_UClass_UGaussianSplatAssetFactory_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_UGaussianSplatAssetFactory.OuterSingleton;
}
template<> GAUSSIANSPLATTINGEDITOR_API UClass* StaticClass<UGaussianSplatAssetFactory>()
{
	return UGaussianSplatAssetFactory::StaticClass();
}
DEFINE_VTABLE_PTR_HELPER_CTOR(UGaussianSplatAssetFactory);
UGaussianSplatAssetFactory::~UGaussianSplatAssetFactory() {}
// End Class UGaussianSplatAssetFactory

// Begin Registration
struct Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingEditor_Public_GaussianSplatAssetFactory_h_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UGaussianSplatAssetFactory, UGaussianSplatAssetFactory::StaticClass, TEXT("UGaussianSplatAssetFactory"), &Z_Registration_Info_UClass_UGaussianSplatAssetFactory, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UGaussianSplatAssetFactory), 3976448658U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingEditor_Public_GaussianSplatAssetFactory_h_2043349344(TEXT("/Script/GaussianSplattingEditor"),
	Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingEditor_Public_GaussianSplatAssetFactory_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingEditor_Public_GaussianSplatAssetFactory_h_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
