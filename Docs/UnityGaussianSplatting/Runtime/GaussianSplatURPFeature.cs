// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianSplatURPFeature.cs
// 功能: Universal Render Pipeline(URP)集成 - 在URP中渲染高斯溅射
// 说明:
//   Unity 6.0+的URP使用RenderGraph系统
//   本文件提供了URP渲染特性插件，使高斯溅射能在URP管道中正确渲染
//   集成点: BeforeRenderingTransparents 通道
// ============================================================================

#if GS_ENABLE_URP

#if !UNITY_6000_0_OR_NEWER
#error Unity Gaussian Splatting URP support only works in Unity 6 or later
#endif

using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.Rendering.RenderGraphModule;

namespace GaussianSplatting.Runtime
{
    /// <summary>
    /// 高斯溅射的URP渲染特性插件
    /// 注册为ScriptableRendererFeature，集成到URP管道
    /// </summary>
    // ReSharper disable once InconsistentNaming
    class GaussianSplatURPFeature : ScriptableRendererFeature
    {
        // 实际执行渲染逻辑的URP Pass。
        class GSRenderPass : ScriptableRenderPass
        {
            const string GaussianSplatRTName = "_GaussianSplatRT";

            const string ProfilerTag = "GaussianSplatRenderGraph";
            static readonly ProfilingSampler s_profilingSampler = new(ProfilerTag);
            static readonly int s_gaussianSplatRT = Shader.PropertyToID(GaussianSplatRTName);

            // RenderGraph传递到执行函数的数据包。
            class PassData
            {
                internal UniversalCameraData CameraData;
                internal TextureHandle SourceTexture;
                internal TextureHandle SourceDepth;
                internal TextureHandle GaussianSplatRT;
            }

            // 记录RenderGraph节点：创建临时RT、绘制高斯、再合成到当前相机颜色目标。
            public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
            {
                using var builder = renderGraph.AddUnsafePass(ProfilerTag, out PassData passData);

                // 读取URP当前帧的相机和资源上下文。
                var cameraData = frameData.Get<UniversalCameraData>();
                var resourceData = frameData.Get<UniversalResourceData>();

                // 创建用于高斯溅射中间结果的HDR颜色RT。
                RenderTextureDescriptor rtDesc = cameraData.cameraTargetDescriptor;
                rtDesc.depthBufferBits = 0;
                rtDesc.msaaSamples = 1;
                rtDesc.graphicsFormat = GraphicsFormat.R16G16B16A16_SFloat;
                var textureHandle = UniversalRenderer.CreateRenderGraphTexture(renderGraph, rtDesc, GaussianSplatRTName, true);

                passData.CameraData = cameraData;
                passData.SourceTexture = resourceData.activeColorTexture;
                passData.SourceDepth = resourceData.activeDepthTexture;
                passData.GaussianSplatRT = textureHandle;

                // 声明本pass的读写资源依赖关系。
                builder.UseTexture(resourceData.activeColorTexture, AccessFlags.ReadWrite);
                builder.UseTexture(resourceData.activeDepthTexture);
                builder.UseTexture(textureHandle, AccessFlags.Write);
                builder.AllowPassCulling(false);
                builder.SetRenderFunc(static (PassData data, UnsafeGraphContext context) =>
                {
                    var commandBuffer = CommandBufferHelpers.GetNativeCommandBuffer(context.cmd);
                    using var _ = new ProfilingScope(commandBuffer, s_profilingSampler);

                    // 先渲染高斯到临时RT。
                    commandBuffer.SetGlobalTexture(s_gaussianSplatRT, data.GaussianSplatRT);
                    CoreUtils.SetRenderTarget(commandBuffer, data.GaussianSplatRT, data.SourceDepth, ClearFlag.Color, Color.clear);
                    Material matComposite = GaussianSplatRenderSystem.instance.SortAndRenderSplats(data.CameraData.camera, commandBuffer);

                    // 再把临时RT按合成材质叠加回相机颜色目标。
                    commandBuffer.BeginSample(GaussianSplatRenderSystem.s_ProfCompose);
                    Blitter.BlitCameraTexture(commandBuffer, data.GaussianSplatRT, data.SourceTexture, matComposite, 0);
                    commandBuffer.EndSample(GaussianSplatRenderSystem.s_ProfCompose);
                });
            }
        }

        GSRenderPass m_Pass;
        bool m_HasCamera;

        // 创建并配置自定义Pass实例。
        public override void Create()
        {
            m_Pass = new GSRenderPass
            {
                renderPassEvent = RenderPassEvent.BeforeRenderingTransparents
            };
        }

        // 在相机预裁剪阶段收集本相机是否存在可渲染高斯对象。
        public override void OnCameraPreCull(ScriptableRenderer renderer, in CameraData cameraData)
        {
            m_HasCamera = false;
            var system = GaussianSplatRenderSystem.instance;
            if (!system.GatherSplatsForCamera(cameraData.camera))
                return;

            m_HasCamera = true;
        }

        // 仅当当前相机确实需要高斯渲染时，才把pass入队。
        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
        {
            if (!m_HasCamera)
                return;
            renderer.EnqueuePass(m_Pass);
        }

        // 释放引用，交由GC清理。
        protected override void Dispose(bool disposing)
        {
            m_Pass = null;
        }
    }
}

#endif // #if GS_ENABLE_URP
