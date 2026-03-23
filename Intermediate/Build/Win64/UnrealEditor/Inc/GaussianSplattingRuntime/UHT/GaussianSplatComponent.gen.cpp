// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GaussianSplattingRuntime/Public/GaussianSplatComponent.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodeGaussianSplatComponent() {}

// Begin Cross Module References
ENGINE_API UClass* Z_Construct_UClass_UPrimitiveComponent();
ENGINE_API UClass* Z_Construct_UClass_UTexture2D_NoRegister();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatAsset_NoRegister();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatComponent();
GAUSSIANSPLATTINGRUNTIME_API UClass* Z_Construct_UClass_UGaussianSplatComponent_NoRegister();
GAUSSIANSPLATTINGRUNTIME_API UEnum* Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode();
UPackage* Z_Construct_UPackage__Script_GaussianSplattingRuntime();
// End Cross Module References

// Begin Enum EGaussianPreviewRenderMode
static FEnumRegistrationInfo Z_Registration_Info_UEnum_EGaussianPreviewRenderMode;
static UEnum* EGaussianPreviewRenderMode_StaticEnum()
{
	if (!Z_Registration_Info_UEnum_EGaussianPreviewRenderMode.OuterSingleton)
	{
		Z_Registration_Info_UEnum_EGaussianPreviewRenderMode.OuterSingleton = GetStaticEnum(Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode, (UObject*)Z_Construct_UPackage__Script_GaussianSplattingRuntime(), TEXT("EGaussianPreviewRenderMode"));
	}
	return Z_Registration_Info_UEnum_EGaussianPreviewRenderMode.OuterSingleton;
}
template<> GAUSSIANSPLATTINGRUNTIME_API UEnum* StaticEnum<EGaussianPreviewRenderMode>()
{
	return EGaussianPreviewRenderMode_StaticEnum();
}
struct Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Enum_MetaDataParams[] = {
		{ "Billboards.DisplayName", "Gaussian Billboards" },
		{ "Billboards.Name", "EGaussianPreviewRenderMode::Billboards" },
		{ "BlueprintType", "true" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
		{ "Points.DisplayName", "Points" },
		{ "Points.Name", "EGaussianPreviewRenderMode::Points" },
	};
#endif // WITH_METADATA
	static constexpr UECodeGen_Private::FEnumeratorParam Enumerators[] = {
		{ "EGaussianPreviewRenderMode::Points", (int64)EGaussianPreviewRenderMode::Points },
		{ "EGaussianPreviewRenderMode::Billboards", (int64)EGaussianPreviewRenderMode::Billboards },
	};
	static const UECodeGen_Private::FEnumParams EnumParams;
};
const UECodeGen_Private::FEnumParams Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics::EnumParams = {
	(UObject*(*)())Z_Construct_UPackage__Script_GaussianSplattingRuntime,
	nullptr,
	"EGaussianPreviewRenderMode",
	"EGaussianPreviewRenderMode",
	Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics::Enumerators,
	RF_Public|RF_Transient|RF_MarkAsNative,
	UE_ARRAY_COUNT(Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics::Enumerators),
	EEnumFlags::None,
	(uint8)UEnum::ECppForm::EnumClass,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics::Enum_MetaDataParams), Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics::Enum_MetaDataParams)
};
UEnum* Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode()
{
	if (!Z_Registration_Info_UEnum_EGaussianPreviewRenderMode.InnerSingleton)
	{
		UECodeGen_Private::ConstructUEnum(Z_Registration_Info_UEnum_EGaussianPreviewRenderMode.InnerSingleton, Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode_Statics::EnumParams);
	}
	return Z_Registration_Info_UEnum_EGaussianPreviewRenderMode.InnerSingleton;
}
// End Enum EGaussianPreviewRenderMode

// Begin Class UGaussianSplatComponent Function BuildDefaultGaussianFalloffTexture
struct Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_UGaussianSplatComponent, nullptr, "BuildDefaultGaussianFalloffTexture", nullptr, nullptr, nullptr, 0, 0, RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x04020401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture_Statics::Function_MetaDataParams), Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture_Statics::Function_MetaDataParams) };
UFunction* Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(UGaussianSplatComponent::execBuildDefaultGaussianFalloffTexture)
{
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->BuildDefaultGaussianFalloffTexture();
	P_NATIVE_END;
}
// End Class UGaussianSplatComponent Function BuildDefaultGaussianFalloffTexture

// Begin Class UGaussianSplatComponent Function SetGaussianAsset
struct Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics
{
	struct GaussianSplatComponent_eventSetGaussianAsset_Parms
	{
		UGaussianSplatAsset* InAsset;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FObjectPropertyParams NewProp_InAsset;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::NewProp_InAsset = { "InAsset", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(GaussianSplatComponent_eventSetGaussianAsset_Parms, InAsset), Z_Construct_UClass_UGaussianSplatAsset_NoRegister, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::NewProp_InAsset,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_UGaussianSplatComponent, nullptr, "SetGaussianAsset", nullptr, nullptr, Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::PropPointers), sizeof(Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::GaussianSplatComponent_eventSetGaussianAsset_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x04020401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::Function_MetaDataParams), Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::GaussianSplatComponent_eventSetGaussianAsset_Parms) < MAX_uint16);
UFunction* Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(UGaussianSplatComponent::execSetGaussianAsset)
{
	P_GET_OBJECT(UGaussianSplatAsset,Z_Param_InAsset);
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->SetGaussianAsset(Z_Param_InAsset);
	P_NATIVE_END;
}
// End Class UGaussianSplatComponent Function SetGaussianAsset

// Begin Class UGaussianSplatComponent
void UGaussianSplatComponent::StaticRegisterNativesUGaussianSplatComponent()
{
	UClass* Class = UGaussianSplatComponent::StaticClass();
	static const FNameNativePtrPair Funcs[] = {
		{ "BuildDefaultGaussianFalloffTexture", &UGaussianSplatComponent::execBuildDefaultGaussianFalloffTexture },
		{ "SetGaussianAsset", &UGaussianSplatComponent::execSetGaussianAsset },
	};
	FNativeFunctionRegistrar::RegisterFunctions(Class, Funcs, UE_ARRAY_COUNT(Funcs));
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(UGaussianSplatComponent);
UClass* Z_Construct_UClass_UGaussianSplatComponent_NoRegister()
{
	return UGaussianSplatComponent::StaticClass();
}
struct Z_Construct_UClass_UGaussianSplatComponent_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "BlueprintSpawnableComponent", "" },
		{ "ClassGroupNames", "Rendering" },
		{ "HideCategories", "Mobility VirtualTexture Trigger" },
		{ "IncludePath", "GaussianSplatComponent.h" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Asset_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_DensityScale_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ClampMin", "0.0" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OpacityScale_MetaData[] = {
		{ "Category", "Gaussian Splat" },
		{ "ClampMin", "0.0" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_PointSize_MetaData[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ClampMax", "32.0" },
		{ "ClampMin", "0.1" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bDepthSort_MetaData[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bFrustumCull_MetaData[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_MaxRenderPoints_MetaData[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ClampMax", "2000000" },
		{ "ClampMin", "1000" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_PreviewRenderMode_MetaData[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_GaussianFalloffTexture_MetaData[] = {
		{ "Category", "Gaussian Splat|Preview" },
		{ "ModuleRelativePath", "Public/GaussianSplatComponent.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FObjectPropertyParams NewProp_Asset;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_DensityScale;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_OpacityScale;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_PointSize;
	static void NewProp_bDepthSort_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bDepthSort;
	static void NewProp_bFrustumCull_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bFrustumCull;
	static const UECodeGen_Private::FIntPropertyParams NewProp_MaxRenderPoints;
	static const UECodeGen_Private::FBytePropertyParams NewProp_PreviewRenderMode_Underlying;
	static const UECodeGen_Private::FEnumPropertyParams NewProp_PreviewRenderMode;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_GaussianFalloffTexture;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static UObject* (*const DependentSingletons[])();
	static constexpr FClassFunctionLinkInfo FuncInfo[] = {
		{ &Z_Construct_UFunction_UGaussianSplatComponent_BuildDefaultGaussianFalloffTexture, "BuildDefaultGaussianFalloffTexture" }, // 1876011276
		{ &Z_Construct_UFunction_UGaussianSplatComponent_SetGaussianAsset, "SetGaussianAsset" }, // 339175218
	};
	static_assert(UE_ARRAY_COUNT(FuncInfo) < 2048);
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UGaussianSplatComponent>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_Asset = { "Asset", nullptr, (EPropertyFlags)0x0114000000000015, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, Asset), Z_Construct_UClass_UGaussianSplatAsset_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Asset_MetaData), NewProp_Asset_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_DensityScale = { "DensityScale", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, DensityScale), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_DensityScale_MetaData), NewProp_DensityScale_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_OpacityScale = { "OpacityScale", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, OpacityScale), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OpacityScale_MetaData), NewProp_OpacityScale_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_PointSize = { "PointSize", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, PointSize), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_PointSize_MetaData), NewProp_PointSize_MetaData) };
void Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bDepthSort_SetBit(void* Obj)
{
	((UGaussianSplatComponent*)Obj)->bDepthSort = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bDepthSort = { "bDepthSort", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(UGaussianSplatComponent), &Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bDepthSort_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bDepthSort_MetaData), NewProp_bDepthSort_MetaData) };
void Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bFrustumCull_SetBit(void* Obj)
{
	((UGaussianSplatComponent*)Obj)->bFrustumCull = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bFrustumCull = { "bFrustumCull", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(UGaussianSplatComponent), &Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bFrustumCull_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bFrustumCull_MetaData), NewProp_bFrustumCull_MetaData) };
const UECodeGen_Private::FIntPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_MaxRenderPoints = { "MaxRenderPoints", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Int, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, MaxRenderPoints), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_MaxRenderPoints_MetaData), NewProp_MaxRenderPoints_MetaData) };
const UECodeGen_Private::FBytePropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_PreviewRenderMode_Underlying = { "UnderlyingType", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Byte, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, nullptr, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FEnumPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_PreviewRenderMode = { "PreviewRenderMode", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Enum, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, PreviewRenderMode), Z_Construct_UEnum_GaussianSplattingRuntime_EGaussianPreviewRenderMode, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_PreviewRenderMode_MetaData), NewProp_PreviewRenderMode_MetaData) }; // 4015181283
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_GaussianFalloffTexture = { "GaussianFalloffTexture", nullptr, (EPropertyFlags)0x0114000000000015, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UGaussianSplatComponent, GaussianFalloffTexture), Z_Construct_UClass_UTexture2D_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_GaussianFalloffTexture_MetaData), NewProp_GaussianFalloffTexture_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UClass_UGaussianSplatComponent_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_Asset,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_DensityScale,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_OpacityScale,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_PointSize,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bDepthSort,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_bFrustumCull,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_MaxRenderPoints,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_PreviewRenderMode_Underlying,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_PreviewRenderMode,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UGaussianSplatComponent_Statics::NewProp_GaussianFalloffTexture,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatComponent_Statics::PropPointers) < 2048);
UObject* (*const Z_Construct_UClass_UGaussianSplatComponent_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_UPrimitiveComponent,
	(UObject* (*)())Z_Construct_UPackage__Script_GaussianSplattingRuntime,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatComponent_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_UGaussianSplatComponent_Statics::ClassParams = {
	&UGaussianSplatComponent::StaticClass,
	"Engine",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	FuncInfo,
	Z_Construct_UClass_UGaussianSplatComponent_Statics::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	UE_ARRAY_COUNT(FuncInfo),
	UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatComponent_Statics::PropPointers),
	0,
	0x00B000A4u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_UGaussianSplatComponent_Statics::Class_MetaDataParams), Z_Construct_UClass_UGaussianSplatComponent_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_UGaussianSplatComponent()
{
	if (!Z_Registration_Info_UClass_UGaussianSplatComponent.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UGaussianSplatComponent.OuterSingleton, Z_Construct_UClass_UGaussianSplatComponent_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_UGaussianSplatComponent.OuterSingleton;
}
template<> GAUSSIANSPLATTINGRUNTIME_API UClass* StaticClass<UGaussianSplatComponent>()
{
	return UGaussianSplatComponent::StaticClass();
}
UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}
DEFINE_VTABLE_PTR_HELPER_CTOR(UGaussianSplatComponent);
UGaussianSplatComponent::~UGaussianSplatComponent() {}
// End Class UGaussianSplatComponent

// Begin Registration
struct Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_Statics
{
	static constexpr FEnumRegisterCompiledInInfo EnumInfo[] = {
		{ EGaussianPreviewRenderMode_StaticEnum, TEXT("EGaussianPreviewRenderMode"), &Z_Registration_Info_UEnum_EGaussianPreviewRenderMode, CONSTRUCT_RELOAD_VERSION_INFO(FEnumReloadVersionInfo, 4015181283U) },
	};
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UGaussianSplatComponent, UGaussianSplatComponent::StaticClass, TEXT("UGaussianSplatComponent"), &Z_Registration_Info_UClass_UGaussianSplatComponent, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UGaussianSplatComponent), 2276196934U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_613154944(TEXT("/Script/GaussianSplattingRuntime"),
	Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_Statics::ClassInfo),
	nullptr, 0,
	Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_Statics::EnumInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_Statics::EnumInfo));
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
