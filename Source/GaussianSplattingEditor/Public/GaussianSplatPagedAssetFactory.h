#pragma once

// Imports a PLY as packed pages (UGaussianSplatPagedAsset) rather than as source
// arrays. The bake settings are UPROPERTYs rather than console variables because
// they belong to the asset that comes out, not to the session: a scene imported
// at 16 m cells should re-import at 16 m next year without anyone remembering to
// set a CVar first. They are also what an editor Python script sets.

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "GaussianSplatPagedAssetFactory.generated.h"

UCLASS()
class GAUSSIANSPLATTINGEDITOR_API UGaussianSplatPagedAssetFactory : public UFactory, public FReimportHandler
{
    GENERATED_BODY()

public:
    UGaussianSplatPagedAssetFactory();

    // 16 m suits a street capture, 64 m a drone one. The cell is what the LOD
    // selection reasons about, so it wants to be about the distance over which
    // detail should change. The importer warns when cells end up holding fewer
    // than about eight pages.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Bake", meta = (ClampMin = "1.0"))
    float CellSize = 16.0f;

    // Cells thinner than this are reconstruction noise rather than surface, and
    // are not written at all.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Bake", meta = (ClampMin = "0"))
    int32 MinCellOccupancy = 100;

    // The always-resident prefix of every cell. Matches the selection's own floor,
    // which the paged path clamps to this baked value.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Bake",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float FloorFraction = 0.02f;

    // Fix 5 Step 2 (review M1): bake this capture without its spherical harmonics.
    // The one measurement that prices SH3 directly -- Uno against Uno-nosh at the
    // same spot, same geometry, same selection.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Bake")
    bool bStripSH = false;

    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;

    // FReimportHandler: the source path and the bake settings are in the asset, so
    // a re-import needs no arguments and cannot silently change the bake.
    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32 GetPriority() const override;
};
