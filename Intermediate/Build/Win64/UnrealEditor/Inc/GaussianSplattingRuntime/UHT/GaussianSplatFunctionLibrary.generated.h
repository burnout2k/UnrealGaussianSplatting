// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "GaussianSplatFunctionLibrary.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
class UGaussianSplatAsset;
class UGaussianSplatComponent;
#ifdef GAUSSIANSPLATTINGRUNTIME_GaussianSplatFunctionLibrary_generated_h
#error "GaussianSplatFunctionLibrary.generated.h already included, missing '#pragma once' in GaussianSplatFunctionLibrary.h"
#endif
#define GAUSSIANSPLATTINGRUNTIME_GaussianSplatFunctionLibrary_generated_h

#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_RPC_WRAPPERS_NO_PURE_DECLS \
	DECLARE_FUNCTION(execSetGaussianAsset);


#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_INCLASS_NO_PURE_DECLS \
private: \
	static void StaticRegisterNativesUGaussianSplatFunctionLibrary(); \
	friend struct Z_Construct_UClass_UGaussianSplatFunctionLibrary_Statics; \
public: \
	DECLARE_CLASS(UGaussianSplatFunctionLibrary, UBlueprintFunctionLibrary, COMPILED_IN_FLAGS(0), CASTCLASS_None, TEXT("/Script/GaussianSplattingRuntime"), NO_API) \
	DECLARE_SERIALIZER(UGaussianSplatFunctionLibrary)


#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_ENHANCED_CONSTRUCTORS \
	/** Standard constructor, called after all reflected properties have been initialized */ \
	NO_API UGaussianSplatFunctionLibrary(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get()); \
private: \
	/** Private move- and copy-constructors, should never be used */ \
	UGaussianSplatFunctionLibrary(UGaussianSplatFunctionLibrary&&); \
	UGaussianSplatFunctionLibrary(const UGaussianSplatFunctionLibrary&); \
public: \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, UGaussianSplatFunctionLibrary); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(UGaussianSplatFunctionLibrary); \
	DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(UGaussianSplatFunctionLibrary) \
	NO_API virtual ~UGaussianSplatFunctionLibrary();


#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_10_PROLOG
#define FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_RPC_WRAPPERS_NO_PURE_DECLS \
	FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_INCLASS_NO_PURE_DECLS \
	FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h_13_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


template<> GAUSSIANSPLATTINGRUNTIME_API UClass* StaticClass<class UGaussianSplatFunctionLibrary>();

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_Unreal_Projects_test_Plugins_UnrealGaussianSplatting_Source_GaussianSplattingRuntime_Public_GaussianSplatFunctionLibrary_h


PRAGMA_ENABLE_DEPRECATION_WARNINGS
