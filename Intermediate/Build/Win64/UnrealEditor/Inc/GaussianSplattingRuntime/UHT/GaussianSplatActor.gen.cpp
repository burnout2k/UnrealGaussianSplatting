// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GaussianSplattingRuntime/Public/GaussianSplatActor.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodeGaussianSplatActor() {}

// Begin Cross Module References
ENGINE_API UClass* Z_Construct_UClass_AActor();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_AGaussianSplatActor();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_AGaussianSplatActor_NoRegister();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatComponent_NoRegister();
UPackage* Z_Construct_UPackage__Script_GaussianSplattingRuntime();
// End Cross Module References

// Begin Class AGaussianSplatActor
void AGaussianSplatActor::StaticRegisterNativesAGaussianSplatActor()
{
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(AGaussianSplatActor);
UClass* Z_Construct_UClass_AGaussianSplatActor_NoRegister()
{
	return AGaussianSplatActor::StaticClass();
}
struct Z_Construct_UClass_AGaussianSplatActor_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "IncludePath", "GaussianSplatActor.h" },
		{ "ModuleRelativePath", "Public/GaussianSplatActor.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_SplatComponent_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "Public/GaussianSplatActor.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FObjectPropertyParams NewProp_SplatComponent;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static UObject* (*const DependentSingletons[])();
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<AGaussianSplatActor>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AGaussianSplatActor_Statics::NewProp_SplatComponent = { "SplatComponent", nullptr, (EPropertyFlags)0x01140000000a001d, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AGaussianSplatActor, SplatComponent), Z_Construct_UClass_UGaussianSplatComponent_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_SplatComponent_MetaData), NewProp_SplatComponent_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UClass_AGaussianSplatActor_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AGaussianSplatActor_Statics::NewProp_SplatComponent,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_AGaussianSplatActor_Statics::PropPointers) < 2048);
UObject* (*const Z_Construct_UClass_AGaussianSplatActor_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_AActor,
	(UObject* (*)())Z_Construct_UPackage__Script_GaussianSplattingRuntime,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_AGaussianSplatActor_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_AGaussianSplatActor_Statics::ClassParams = {
	&AGaussianSplatActor::StaticClass,
	"Engine",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	nullptr,
	Z_Construct_UClass_AGaussianSplatActor_Statics::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	0,
	UE_ARRAY_COUNT(Z_Construct_UClass_AGaussianSplatActor_Statics::PropPointers),
	0,
	0x009000A4u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_AGaussianSplatActor_Statics::Class_MetaDataParams), Z_Construct_UClass_AGaussianSplatActor_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_AGaussianSplatActor()
{
	if (!Z_Registration_Info_UClass_AGaussianSplatActor.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_AGaussianSplatActor.OuterSingleton, Z_Construct_UClass_AGaussianSplatActor_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_AGaussianSplatActor.OuterSingleton;
}
template<> GAUSSIANSPLATTINGRUNTIME_API UClass* StaticClass<AGaussianSplatActor>()
{
	return AGaussianSplatActor::StaticClass();
}
DEFINE_VTABLE_PTR_HELPER_CTOR(AGaussianSplatActor);
AGaussianSplatActor::~AGaussianSplatActor() {}
// End Class AGaussianSplatActor

// Begin Registration
struct Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatActor_h_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_AGaussianSplatActor, AGaussianSplatActor::StaticClass, TEXT("AGaussianSplatActor"), &Z_Registration_Info_UClass_AGaussianSplatActor, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(AGaussianSplatActor), 1261741338U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatActor_h_401389490(TEXT("/Script/GaussianSplattingRuntime"),
	Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatActor_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatActor_h_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
