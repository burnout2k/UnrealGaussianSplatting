#include "Render/GaussianSplatShaders.h"

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPointsCullCS, "/GaussianSplatting/Private/GaussianSplatPointsCull.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBillboardsCullCS, "/GaussianSplatting/Private/GaussianSplatBillboardsCull.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPointsRasterVS, "/GaussianSplatting/Private/GaussianSplatPointsRaster.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPointsRasterPS, "/GaussianSplatting/Private/GaussianSplatPointsRaster.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBillboardsRasterVS, "/GaussianSplatting/Private/GaussianSplatBillboardsRaster.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatBillboardsRasterPS, "/GaussianSplatting/Private/GaussianSplatBillboardsRaster.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/GaussianSplatting/Private/GaussianSplatComposite.usf", "MainPS", SF_Pixel);
