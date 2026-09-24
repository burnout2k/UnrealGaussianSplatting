#include "Render/GaussianSplatShaders.h"

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPointsCullCS, "/GaussianSplatting/Private/GaussianSplatPointsCull.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBillboardsCullCS, "/GaussianSplatting/Private/GaussianSplatBillboardsCull.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPointsRasterVS, "/GaussianSplatting/Private/GaussianSplatPointsRaster.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPointsRasterPS, "/GaussianSplatting/Private/GaussianSplatPointsRaster.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBillboardsRasterVS, "/GaussianSplatting/Private/GaussianSplatBillboardsRaster.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBillboardsRasterPS, "/GaussianSplatting/Private/GaussianSplatBillboardsRaster.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/GaussianSplatting/Private/GaussianSplatComposite.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRadixSetupCS, "/GaussianSplatting/Private/GaussianSplatDeviceRadixSort.usf", "RadixSetup", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRadixUpsweepCS, "/GaussianSplatting/Private/GaussianSplatDeviceRadixSort.usf", "Upsweep", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRadixScanCS, "/GaussianSplatting/Private/GaussianSplatDeviceRadixSort.usf", "Scan", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRadixDownsweepCS, "/GaussianSplatting/Private/GaussianSplatDeviceRadixSort.usf", "Downsweep", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatSortPrepareCS, "/GaussianSplatting/Private/GaussianSplatSortValidate.usf", "PrepareCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatSortCompareCS, "/GaussianSplatting/Private/GaussianSplatSortValidate.usf", "CompareCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatSortSelfCheckCS, "/GaussianSplatting/Private/GaussianSplatSortValidate.usf", "SelfCheckCS", SF_Compute);
