#include "Render/GaussianSplatShaders.h"

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCullSortCS, "/GaussianSplatting/Private/GaussianSplatCullSort.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRasterPS, "/GaussianSplatting/Private/GaussianSplatRaster.usf", "MainPS", SF_Pixel);
