// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GaussianSplattingRuntime/Public/GaussianSplatFunctionLibrary.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodeGaussianSplatFunctionLibrary() {}

// Begin Cross Module References
ENGINE_API UClass* Z_Construct_UClass_UBlueprintFunctionLibrary();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatAsset_NoRegister();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatComponent_NoRegister();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatFunctionLibrary();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatFunctionLibrary_NoRegister();
UPackage* Z_Construct_UPackage__Script_GaussianSplattingRuntime();
// End Cross Module References

// Begin Class UGaussianSplatFunctionLibrary Function SetGaussianAsset
struct Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics
{
	struct GaussianSplatFunctionLibrary_eventSetGaussianAsset_Parms
	{
		UGaussianSplatComponent* Component;
		UGaussianSplatAsset* Asset;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatFunctionLibrary.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Component_MetaData[] = {
		{ "EditInline", "true" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FObjectPropertyParams NewProp_Component;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_Asset;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::NewProp_Component = { "Component", nullptr, (EPropertyFlags)0x0010000000080080, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(GaussianSplatFunctionLibrary_eventSetGaussianAsset_Parms, Component), Z_Construct_UClass_UGaussianSplatComponent_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Component_MetaData), NewProp_Component_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::NewProp_Asset = { "Asset", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(GaussianSplatFunctionLibrary_eventSetGaussianAsset_Parms, Asset), Z_Construct_UClass_UGaussianSplatAsset_NoRegister, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::NewProp_Component,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::NewProp_Asset,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_UGaussianSplatFunctionLibrary, nullptr, "SetGaussianAsset", nullptr, nullptr, Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::PropPointers), sizeof(Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::GaussianSplatFunctionLibrary_eventSetGaussianAsset_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x04022401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::Function_MetaDataParams), Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::GaussianSplatFunctionLibrary_eventSetGaussianAsset_Parms) < MAX_uint16);
UFunction* Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(UGaussianSplatFunctionLibrary::execSetGaussianAsset)
{
	P_GET_OBJECT(UGaussianSplatComponent,Z_Param_Component);
	P_GET_OBJECT(UGaussianSplatAsset,Z_Param_Asset);
	P_FINISH;
	P_NATIVE_BEGIN;
	UGaussianSplatFunctionLibrary::SetGaussianAsset(Z_Param_Component,Z_Param_Asset);
	P_NATIVE_END;
}
// End Class UGaussianSplatFunctionLibrary Function SetGaussianAsset

// Begin Class UGaussianSplatFunctionLibrary
void UGaussianSplatFunctionLibrary::StaticRegisterNativesUGaussianSplatFunctionLibrary()
{
	UClass* Class = UGaussianSplatFunctionLibrary::StaticClass();
	static const FNameNativePtrPair Funcs[] = {
		{ "SetGaussianAsset", &UGaussianSplatFunctionLibrary::execSetGaussianAsset },
	};
	FNativeFunctionRegistrar::RegisterFunctions(Class, Funcs, UE_ARRAY_COUNT(Funcs));
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(UGaussianSplatFunctionLibrary);
UClass* Z_Construct_UClass_UGaussianSplatFunctionLibrary_NoRegister()
{
	return UGaussianSplatFunctionLibrary::StaticClass();
}
struct Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "IncludePath", "GaussianSplatFunctionLibrary.h" },
		{ "ModuleRelativePath", "Public/GaussianSplatFunctionLibrary.h" },
	};
#endif // WITH_METADATA
	static UObject* (*const DependentSingletons[])();
	static constexpr FClassFunctionLinkInfo FuncInfo[] = {
		{ &Z_Construct_UFunction_UGaussianSplatFunctionLibrary_SetGaussianAsset, "SetGaussianAsset" }, // 4111557235
	};
	static_assert(UE_ARRAY_COUNT(FuncInfo) < 2048);
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UGaussianSplatFunctionLibrary>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
UObject* (*const Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_UBlueprintFunctionLibrary,
	(UObject* (*)())Z_Construct_UPackage__Script_GaussianSplattingRuntime,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics::ClassParams = {
	&UGaussianSplatFunctionLibrary::StaticClass,
	nullptr,
	&StaticCppClassTypeInfo,
	DependentSingletons,
	FuncInfo,
	nullptr,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	UE_ARRAY_COUNT(FuncInfo),
	0,
	0,
	0x001000A0u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics::Class_MetaDataParams), Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_UGaussianSplatFunctionLibrary()
{
	if (!Z_Registration_Info_UClass_UGaussianSplatFunctionLibrary.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UGaussianSplatFunctionLibrary.OuterSingleton, Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_UGaussianSplatFunctionLibrary.OuterSingleton;
}
template<> GAUSSIANSPLATTINGRUNTIME_API UClass* StaticClass<UGaussianSplatFunctionLibrary>()
{
	return UGaussianSplatFunctionLibrary::StaticClass();
}
UGaussianSplatFunctionLibrary::UGaussianSplatFunctionLibrary(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}
DEFINE_VTABLE_PTR_HELPER_CTOR(UGaussianSplatFunctionLibrary);
UGaussianSplatFunctionLibrary::~UGaussianSplatFunctionLibrary() {}
// End Class UGaussianSplatFunctionLibrary

// Begin Registration
struct Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UGaussianSplatFunctionLibrary, UGaussianSplatFunctionLibrary::StaticClass, TEXT("UGaussianSplatFunctionLibrary"), &Z_Registration_Info_UClass_UGaussianSplatFunctionLibrary, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UGaussianSplatFunctionLibrary), 2707208209U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_2697264966(TEXT("/Script/GaussianSplattingRuntime"),
	Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
