// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GpuSorting.cs
// 功能: GPU基数排序实现 - 使用计算着色器在GPU上进行高效排序
// 说明:
//   3DGS渲染需要按相机深度排序溅射点(深度排序)
//   本文件实现了8位基数排序(Radix Sort)的GPU版本
//   使用reduce-then-scan算法，性能远优于CPU排序
//   Copyright Thomas Smith 2024, MIT license
//   https://github.com/b0nes164/GPUSorting
// ============================================================================

using UnityEngine;
using UnityEngine.Assertions;
using UnityEngine.Rendering;

namespace GaussianSplatting.Runtime
{
    /// <summary>
    /// GPU基数排序实现(8位增量)
    /// 对2个缓冲区进行同步排序:
    /// 1. 键缓冲区(Key): 包含排序关键字(例如深度值)
    /// 2. 值缓冲区(Values): 携带的有效负载(例如溅射索引)
    /// 排序后键值对保持对应关系
    /// </summary>
    public class GpuSorting
    {
        //The size of a threadblock partition in the sort
        const uint DEVICE_RADIX_SORT_PARTITION_SIZE = 3840;

        //The size of our radix in bits
        const uint DEVICE_RADIX_SORT_BITS = 8;

        //Number of digits in our radix, 1 << DEVICE_RADIX_SORT_BITS
        const uint DEVICE_RADIX_SORT_RADIX = 256;

        //Number of sorting passes required to sort a 32bit key, KEY_BITS / DEVICE_RADIX_SORT_BITS
        const uint DEVICE_RADIX_SORT_PASSES = 4;

        //Keywords to enable for the shader
        private LocalKeyword m_keyUintKeyword;
        private LocalKeyword m_payloadUintKeyword;
        private LocalKeyword m_ascendKeyword;
        private LocalKeyword m_sortPairKeyword;
        private LocalKeyword m_vulkanKeyword;

        public struct Args
        {
            public uint             count;
            public GraphicsBuffer   inputKeys;
            public GraphicsBuffer   inputValues;
            public SupportResources resources;
            internal int workGroupCount;
        }

        public struct SupportResources
        {
            public GraphicsBuffer altBuffer;
            public GraphicsBuffer altPayloadBuffer;
            public GraphicsBuffer passHistBuffer;
            public GraphicsBuffer globalHistBuffer;

            // 按待排序数量分配排序所需的临时缓冲区。
            // 包含替换缓冲区、分区直方图和全局直方图。
            public static SupportResources Load(uint count)
            {
                // 分区直方图大小 = 分区数 * 基数桶数(256)。
                uint scratchBufferSize = DivRoundUp(count, DEVICE_RADIX_SORT_PARTITION_SIZE) * DEVICE_RADIX_SORT_RADIX; 
                // 全局直方图大小 = 每轮256桶 * 4轮(32位key每次处理8位)。
                uint reducedScratchBufferSize = DEVICE_RADIX_SORT_RADIX * DEVICE_RADIX_SORT_PASSES;

                var target = GraphicsBuffer.Target.Structured;
                var resources = new SupportResources
                {
                    altBuffer = new GraphicsBuffer(target, (int)count, 4) { name = "DeviceRadixAlt" },
                    altPayloadBuffer = new GraphicsBuffer(target, (int)count, 4) { name = "DeviceRadixAltPayload" },
                    passHistBuffer = new GraphicsBuffer(target, (int)scratchBufferSize, 4) { name = "DeviceRadixPassHistogram" },
                    globalHistBuffer = new GraphicsBuffer(target, (int)reducedScratchBufferSize, 4) { name = "DeviceRadixGlobalHistogram" },
                };
                return resources;
            }

            // 释放所有排序辅助缓冲区，避免GPU内存泄漏。
            public void Dispose()
            {
                altBuffer?.Dispose();
                altPayloadBuffer?.Dispose();
                passHistBuffer?.Dispose();
                globalHistBuffer?.Dispose();

                altBuffer = null;
                altPayloadBuffer = null;
                passHistBuffer = null;
                globalHistBuffer = null;
            }
        }

        readonly ComputeShader m_CS;
        readonly int m_kernelInitDeviceRadixSort = -1;
        readonly int m_kernelUpsweep = -1;
        readonly int m_kernelScan = -1;
        readonly int m_kernelDownsweep = -1;

        readonly bool m_Valid;

        public bool Valid => m_Valid;

        // 构造排序器：查找内核、检查可用性、配置关键字。
        public GpuSorting(ComputeShader cs)
        {
            m_CS = cs;
            if (cs)
            {
                // 获取各阶段内核索引。
                m_kernelInitDeviceRadixSort = cs.FindKernel("InitDeviceRadixSort");
                m_kernelUpsweep = cs.FindKernel("Upsweep");
                m_kernelScan = cs.FindKernel("Scan");
                m_kernelDownsweep = cs.FindKernel("Downsweep");
            }

            // 内核必须全部存在才认为可用。
            m_Valid = m_kernelInitDeviceRadixSort >= 0 &&
                      m_kernelUpsweep >= 0 &&
                      m_kernelScan >= 0 &&
                      m_kernelDownsweep >= 0;
            if (m_Valid)
            {
                // 再检查平台是否支持这些内核。
                if (!cs.IsSupported(m_kernelInitDeviceRadixSort) ||
                    !cs.IsSupported(m_kernelUpsweep) ||
                    !cs.IsSupported(m_kernelScan) ||
                    !cs.IsSupported(m_kernelDownsweep))
                {
                    m_Valid = false;
                }
            }

            // 设置排序使用的数据类型和行为关键字。
            m_keyUintKeyword = new LocalKeyword(cs, "KEY_UINT");
            m_payloadUintKeyword = new LocalKeyword(cs, "PAYLOAD_UINT");
            m_ascendKeyword = new LocalKeyword(cs, "SHOULD_ASCEND");
            m_sortPairKeyword = new LocalKeyword(cs, "SORT_PAIRS");
            m_vulkanKeyword = new LocalKeyword(cs, "VULKAN");

            cs.EnableKeyword(m_keyUintKeyword);
            cs.EnableKeyword(m_payloadUintKeyword);
            cs.EnableKeyword(m_ascendKeyword);
            cs.EnableKeyword(m_sortPairKeyword);
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Vulkan)
                cs.EnableKeyword(m_vulkanKeyword);
            else
                cs.DisableKeyword(m_vulkanKeyword);
        }

        // 整数向上取整除法。
        static uint DivRoundUp(uint x, uint y) => (x + y - 1) / y;

        //Can we remove the last 4 padding without breaking?
        struct SortConstants
        {
            public uint numKeys;                        // The number of keys to sort
            public uint radixShift;                     // The radix shift value for the current pass
            public uint threadBlocks;                   // threadBlocks
            public uint padding0;                       // Padding - unused
        }

        // 在给定CommandBuffer中执行完整基数排序流程。
        // 处理32位key，共4轮，每轮8位。
        public void Dispatch(CommandBuffer cmd, Args args)
        {
            Assert.IsTrue(Valid);

            // src/dst双缓冲轮转，避免读写冲突。
            GraphicsBuffer srcKeyBuffer = args.inputKeys;
            GraphicsBuffer srcPayloadBuffer = args.inputValues;
            GraphicsBuffer dstKeyBuffer = args.resources.altBuffer;
            GraphicsBuffer dstPayloadBuffer = args.resources.altPayloadBuffer;

            SortConstants constants = default;
            constants.numKeys = args.count;
            constants.threadBlocks = DivRoundUp(args.count, DEVICE_RADIX_SORT_PARTITION_SIZE);

            // 设置全局常量：总key数量、分区数量。
            cmd.SetComputeIntParam(m_CS, "e_numKeys", (int)constants.numKeys);
            cmd.SetComputeIntParam(m_CS, "e_threadBlocks", (int)constants.threadBlocks);

            // 绑定各阶段固定缓冲区。
            // Upsweep: 统计每个分区每个桶的数量。
            cmd.SetComputeBufferParam(m_CS, m_kernelUpsweep, "b_passHist", args.resources.passHistBuffer);
            cmd.SetComputeBufferParam(m_CS, m_kernelUpsweep, "b_globalHist", args.resources.globalHistBuffer);

            // Scan: 对直方图做前缀和。
            cmd.SetComputeBufferParam(m_CS, m_kernelScan, "b_passHist", args.resources.passHistBuffer);

            // Downsweep: 根据偏移把元素搬运到正确位置。
            cmd.SetComputeBufferParam(m_CS, m_kernelDownsweep, "b_passHist", args.resources.passHistBuffer);
            cmd.SetComputeBufferParam(m_CS, m_kernelDownsweep, "b_globalHist", args.resources.globalHistBuffer);

            // 每次排序前先清零全局直方图。
            cmd.SetComputeBufferParam(m_CS, m_kernelInitDeviceRadixSort, "b_globalHist", args.resources.globalHistBuffer);
            cmd.DispatchCompute(m_CS, m_kernelInitDeviceRadixSort, 1, 1, 1);

            // 以8位为一轮执行排序(共4轮覆盖32位key)。
            for (constants.radixShift = 0; constants.radixShift < 32; constants.radixShift += DEVICE_RADIX_SORT_BITS)
            {
                cmd.SetComputeIntParam(m_CS, "e_radixShift", (int)constants.radixShift);

                // 阶段1: 统计当前8位数字的桶分布。
                cmd.SetComputeBufferParam(m_CS, m_kernelUpsweep, "b_sort", srcKeyBuffer);
                cmd.DispatchCompute(m_CS, m_kernelUpsweep, (int)constants.threadBlocks, 1, 1);

                // 阶段2: 前缀和，得到每个桶的写入起始偏移。
                cmd.DispatchCompute(m_CS, m_kernelScan, (int)DEVICE_RADIX_SORT_RADIX, 1, 1);

                // 阶段3: 按偏移搬运键值对到目标缓冲区。
                cmd.SetComputeBufferParam(m_CS, m_kernelDownsweep, "b_sort", srcKeyBuffer);
                cmd.SetComputeBufferParam(m_CS, m_kernelDownsweep, "b_sortPayload", srcPayloadBuffer);
                cmd.SetComputeBufferParam(m_CS, m_kernelDownsweep, "b_alt", dstKeyBuffer);
                cmd.SetComputeBufferParam(m_CS, m_kernelDownsweep, "b_altPayload", dstPayloadBuffer);
                cmd.DispatchCompute(m_CS, m_kernelDownsweep, (int)constants.threadBlocks, 1, 1);

                // 轮转双缓冲，为下一轮继续排序做准备。
                (srcKeyBuffer, dstKeyBuffer) = (dstKeyBuffer, srcKeyBuffer);
                (srcPayloadBuffer, dstPayloadBuffer) = (dstPayloadBuffer, srcPayloadBuffer);
            }
        }
    }
}
