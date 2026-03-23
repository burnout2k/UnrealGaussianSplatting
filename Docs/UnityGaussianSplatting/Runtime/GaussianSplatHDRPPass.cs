// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianSplatHDRPPass.cs
// 功能: High Definition Render Pipeline(HDRP)集成 - 在HDRP中渲染高斯溅射
// 说明:
//   Unity的HDRP使用CustomPass系统进行自定义渲染
//   本文件提供了一个CustomPass，在HDRP管道中集成高斯溅射渲染
//   高斯溅射在这里作为透明度物体进行渲染
// ============================================================================

#if GS_ENABLE_HDRP

using UnityEngine;
using UnityEngine.Rendering.HighDefinition;
using UnityEngine.Rendering;
using UnityEngine.Experimental.Rendering;

namespace GaussianSplatting.Runtime
{
    /// <summary>
    /// 高斯溅射的HDRP自定义渲染通道
    /// 提供CustomPass实现，将高斯溅射集成到HDRP渲染管道
    /// </summary>
    class GaussianSplatHDRPPass : CustomPass
    {
        RTHandle m_RenderTarget;

        // 初始化HDRP自定义pass所需的中间RT。
        // 该RT用于先承接高斯绘制结果，再与相机颜色缓冲进行合成。
        protected override void Setup(ScriptableRenderContext renderContext, CommandBuffer cmd)
        {
            m_RenderTarget = RTHandles.Alloc(Vector2.one,
                colorFormat: GraphicsFormat.R16G16B16A16_SFloat, useDynamicScale: true,
                depthBufferBits: DepthBits.None, msaaSamples: MSAASamples.None,
                filterMode: FilterMode.Point, wrapMode: TextureWrapMode.Clamp, name: "_GaussianSplatRT");
        }

        // 每帧执行：收集高斯、渲染到中间RT、再合成到相机颜色。
        protected override void Execute(CustomPassContext ctx)
        {
            var cam = ctx.hdCamera.camera;

            // 当前相机没有可渲染高斯时直接跳过。
            var system = GaussianSplatRenderSystem.instance;
            if (!system.GatherSplatsForCamera(cam))
                return;

            // 设置中间RT并清空为透明黑。
            ctx.cmd.SetGlobalTexture(m_RenderTarget.name, m_RenderTarget.nameID);
            CoreUtils.SetRenderTarget(ctx.cmd, m_RenderTarget, ctx.cameraDepthBuffer, ClearFlag.Color,
                new Color(0, 0, 0, 0));

            // 依次完成排序、视图数据计算和绘制。
            Material matComposite =
                GaussianSplatRenderSystem.instance.SortAndRenderSplats(ctx.hdCamera.camera, ctx.cmd);

            // 合成阶段：把高斯RT叠加到HDRP相机颜色缓冲。
            ctx.cmd.BeginSample(GaussianSplatRenderSystem.s_ProfCompose);
            CoreUtils.SetRenderTarget(ctx.cmd, ctx.cameraColorBuffer, ClearFlag.None);
            CoreUtils.DrawFullScreen(ctx.cmd, matComposite, ctx.propertyBlock, shaderPassId: 0);
            ctx.cmd.EndSample(GaussianSplatRenderSystem.s_ProfCompose);
        }

        // 释放中间RT资源。
        protected override void Cleanup()
        {
            m_RenderTarget.Release();
        }
    }
}

#endif
