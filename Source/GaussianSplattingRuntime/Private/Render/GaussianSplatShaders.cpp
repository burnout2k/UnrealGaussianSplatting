#include "Render/GaussianSplatShaders.h"

// 这些宏把 C++ 类型和插件 Shader 文件里的入口点绑定起来。
// 之后 RDG Pass 中通过 TShaderMapRef<...> 就能拿到对应编译结果。
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCullSortCS, "/GaussianSplatting/Private/GaussianSplatCullSort.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRasterVS, "/GaussianSplatting/Private/GaussianSplatRaster.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatRasterPS, "/GaussianSplatting/Private/GaussianSplatRaster.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/GaussianSplatting/Private/GaussianSplatComposite.usf", "MainPS", SF_Pixel);
