#include "Render/GaussianSplatShaders.h"

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCullSortCS, "/GaussianSplatting/Private/GaussianSplatCullSort.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRasterVS, "/GaussianSplatting/Private/GaussianSplatRaster.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRasterPS, "/GaussianSplatting/Private/GaussianSplatRaster.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/GaussianSplatting/Private/GaussianSplatComposite.usf", "MainPS", SF_Pixel);
