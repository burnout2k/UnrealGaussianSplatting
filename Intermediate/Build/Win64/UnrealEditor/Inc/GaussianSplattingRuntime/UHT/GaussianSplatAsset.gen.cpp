// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GaussianSplattingRuntime/Public/GaussianSplatAsset.h"
#include "Serialization/ArchiveUObjectFromStructuredArchive.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodeGaussianSplatAsset() {}

// Begin Cross Module References
COREUOBJECT_API UClass* Z_Construct_UClass_UObject();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FBoxSphereBounds();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FQuat4f();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FVector3f();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FVector4f();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatAsset();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatAsset_NoRegister();
UPackage* Z_Construct_UPackage__Script_GaussianSplattingRuntime();
// End Cross Module References

// Begin Class UGaussianSplatAsset
void UGaussianSplatAsset::StaticRegisterNativesUGaussianSplatAsset()
{
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(UGaussianSplatAsset);
UClass* Z_Construct_UClass_UGaussianSplatAsset_NoRegister()
{
	return UGaussianSplatAsset::StaticClass();
}
struct Z_Construct_UClass_UGaussianSplatAsset_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "BlueprintType", "true" },
		{ "IncludePath", "GaussianSplatAsset.h" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Positions_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Rotations_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Scales_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ColorsOpacity_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_SHCoefficients_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Bounds_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatAsset.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FStructPropertyParams NewProp_Positions_Inner;
	static const UECodeGen_Private::FArrayPropertyParams NewProp_Positions;
	static const UECodeGen_Private::FStructPropertyParams NewProp_Rotations_Inner;
	static const UECodeGen_Private::FArrayPropertyParams NewProp_Rotations;
	static const UECodeGen_Private::FStructPropertyParams NewProp_Scales_Inner;
	static const UECodeGen_Private::FArrayPropertyParams NewProp_Scales;
	static const UECodeGen_Private::FStructPropertyParams NewProp_ColorsOpacity_Inner;
	static const UECodeGen_Private::FArrayPropertyParams NewProp_ColorsOpacity;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_SHCoefficients_Inner;
	static const UECodeGen_Private::FArrayPropertyParams NewProp_SHCoefficients;
	static const UECodeGen_Private::FStructPropertyParams NewProp_Bounds;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static UObject* (*const DependentSingletons[])();
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UGaussianSplatAsset>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Positions_Inner = { "Positions", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, Z_Construct_UScriptStruct_FVector3f, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FArrayPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Positions = { "Positions", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Array, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatAsset, Positions), EArrayPropertyFlags::None, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Positions_MetaData), NewProp_Positions_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Rotations_Inner = { "Rotations", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, Z_Construct_UScriptStruct_FQuat4f, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FArrayPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Rotations = { "Rotations", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Array, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatAsset, Rotations), EArrayPropertyFlags::None, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Rotations_MetaData), NewProp_Rotations_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Scales_Inner = { "Scales", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, Z_Construct_UScriptStruct_FVector3f, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FArrayPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Scales = { "Scales", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Array, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatAsset, Scales), EArrayPropertyFlags::None, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Scales_MetaData), NewProp_Scales_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_ColorsOpacity_Inner = { "ColorsOpacity", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, Z_Construct_UScriptStruct_FVector4f, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FArrayPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_ColorsOpacity = { "ColorsOpacity", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Array, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatAsset, ColorsOpacity), EArrayPropertyFlags::None, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ColorsOpacity_MetaData), NewProp_ColorsOpacity_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_SHCoefficients_Inner = { "SHCoefficients", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FArrayPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_SHCoefficients = { "SHCoefficients", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Array, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatAsset, SHCoefficients), EArrayPropertyFlags::None, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_SHCoefficients_MetaData), NewProp_SHCoefficients_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Bounds = { "Bounds", nullptr, (EPropertyFlags)0x0010000000020015, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatAsset, Bounds), Z_Construct_UScriptStruct_FBoxSphereBounds, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Bounds_MetaData), NewProp_Bounds_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UClass_UGaussianSplatAsset_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Positions_Inner,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Positions,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Rotations_Inner,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Rotations,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Scales_Inner,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Scales,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_ColorsOpacity_Inner,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_ColorsOpacity,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_SHCoefficients_Inner,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_SHCoefficients,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatAsset_Statics::NewProp_Bounds,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatAsset_Statics::PropPointers) < 2048);
UObject* (*const Z_Construct_UClass_UGaussianSplatAsset_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_UObject,
	(UObject* (*)())Z_Construct_UPackage__Script_GaussianSplattingRuntime,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatAsset_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_UGaussianSplatAsset_Statics::ClassParams = {
	&UGaussianSplatAsset::StaticClass,
	nullptr,
	&StaticCppClassTypeInfo,
	DependentSingletons,
	nullptr,
	Z_Construct_UClass_UGaussianSplatAsset_Statics::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	0,
	UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatAsset_Statics::PropPointers),
	0,
	0x001000A0u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatAsset_Statics::Class_MetaDataParams), Z_Construct_UClass_UGaussianSplatAsset_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_UGaussianSplatAsset()
{
	if (!Z_Registration_Info_UClass_UGaussianSplatAsset.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UGaussianSplatAsset.OuterSingleton, Z_Construct_UClass_UGaussianSplatAsset_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_UGaussianSplatAsset.OuterSingleton;
}
template<> GAUSSIANSPLATTINGRUNTIME_API UClass* StaticClass<UGaussianSplatAsset>()
{
	return UGaussianSplatAsset::StaticClass();
}
UGaussianSplatAsset::UGaussianSplatAsset(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}
DEFINE_VTABLE_PTR_HELPER_CTOR(UGaussianSplatAsset);
UGaussianSplatAsset::~UGaussianSplatAsset() {}
IMPLEMENT_FSTRUCTUREDARCHIVE_SERIALIZER(UGaussianSplatAsset)
// End Class UGaussianSplatAsset

// Begin Registration
struct Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatAsset_h_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UGaussianSplatAsset, UGaussianSplatAsset::StaticClass, TEXT("UGaussianSplatAsset"), &Z_Registration_Info_UClass_UGaussianSplatAsset, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UGaussianSplatAsset), 747303779U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatAsset_h_4017874796(TEXT("/Script/GaussianSplattingRuntime"),
	Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatAsset_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatAsset_h_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
