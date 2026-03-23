// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "GaussianSplatComponent.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
class UGaussianSplatAsset;
#ifdef GAUSSIANSPLATTINGRUNTIME_GaussianSplatComponent_generated_h
#error "GaussianSplatComponent.generated.h already included, missing '#pragma once' in GaussianSplatComponent.h"
#endif
#define GAUSSIANSPLATTINGRUNTIME_GaussianSplatComponent_generated_h

#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_RPC_WRAPPERS_NO_PURE_DECLS \
	DECLARE_FUNCTION(execBuildDefaultGaussianFalloffTexture); \
	DECLARE_FUNCTION(execSetGaussianAsset);


#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_INCLASS_NO_PURE_DECLS \
private: \
	static void StaticRegisterNativesUGaussianSplatComponent(); \
	friend struct Z_Construct_UClass_UGaussianSplatComponent_Statics; \
public: \
	DECLARE_CLASS(UGaussianSplatComponent, UPrimitiveComponent, COMPILED_IN_FLAGS(0 | CLASS_Config), CASTCLASS_None, TEXT("/Script/GaussianSplattingRuntime"), NO_API) \
	DECLARE_SERIALIZER(UGaussianSplatComponent)


#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_ENHANCED_CONSTRUCTORS \
	/** Standard constructor, called after all reflected properties have been initialized */ \
	NO_API UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get()); \
private: \
	/** Private move- and copy-constructors, should never be used */ \
	UGaussianSplatComponent(UGaussianSplatComponent&&); \
	UGaussianSplatComponent(const UGaussianSplatComponent&); \
public: \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, UGaussianSplatComponent); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(UGaussianSplatComponent); \
	DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(UGaussianSplatComponent) \
	NO_API virtual ~UGaussianSplatComponent();


#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_18_PROLOG
#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_RPC_WRAPPERS_NO_PURE_DECLS \
	FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_INCLASS_NO_PURE_DECLS \
	FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h_21_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


template<> GAUSSIANSPLATTINGRUNTIME_API UClass* StaticClass<class UGaussianSplatComponent>();

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatComponent_h


#define FOREACH_ENUM_EGAUSSIANPREVIEWRENDERMODE(op) \
	op(EGaussianPreviewRenderMode::Points) \
	op(EGaussianPreviewRenderMode::Billboards) 

enum class EGaussianPreviewRenderMode : uint8;
template<> struct TIsUEnumClass<EGaussianPreviewRenderMode> { enum { Value = true }; };
template<> GAUSSIANSPLATTINGRUNTIME_API UEnum* StaticEnum<EGaussianPreviewRenderMode>();

PRAGMA_ENABLE_DEPRECATION_WARNINGS
