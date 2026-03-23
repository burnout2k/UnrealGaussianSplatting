// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianSplatAsset.cs
// 功能: 高斯溅射资产配置类 - 存储和管理3D高斯溅射的数据格式和配置信息
// 说明:
//   - 3D高斯溅射(3D Gaussian Splatting, 3DGS)是一种新的3D重建和渲染方法
//   - 该类定义了高斯溅射数据的存储格式、尺寸、位置、缩放、颜色、球谐函数等数据
//   - Unity中的高斯溅射数据通过多个TextAsset (二进制文件)存储GPU数据
//   - 支持多种压缩格式来平衡文件大小和渲染质量
// ============================================================================

using System;
using Unity.Collections.LowLevel.Unsafe;
using Unity.Mathematics;
using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace GaussianSplatting.Runtime
{
    /// <summary>
    /// 高斯溅射资产配置类
    /// 这是一个ScriptableObject，用于在编辑器中创建和配置高斯溅射资产
    /// 它包含所有必要的元数据来描述GPU上的高斯溅射数据的格式和位置
    /// </summary>
    public class GaussianSplatAsset : ScriptableObject
    {
        public const int kCurrentVersion = 2023_10_20; // 资产版本号，用于兼容性检查
        public const int kChunkSize = 256; // 每个数据块包含的溅射数量，用于数据分块存储
        public const int kTextureWidth = 2048; // 颜色纹理宽度，允许最多32M个溅射(2k x 16k)
        public const int kMaxSplats = 8_600_000; // 最大溅射数量限制(受GPU 2GB缓冲区限制)

        [SerializeField] int m_FormatVersion; // 当前资产版本
        [SerializeField] int m_SplatCount; // 溅射总数
        [SerializeField] Vector3 m_BoundsMin; // 包围盒最小角
        [SerializeField] Vector3 m_BoundsMax; // 包围盒最大角
        [SerializeField] Hash128 m_DataHash; // 数据哈希值，用于检测数据变更

        public int formatVersion => m_FormatVersion;
        public int splatCount => m_SplatCount;
        public Vector3 boundsMin => m_BoundsMin;
        public Vector3 boundsMax => m_BoundsMax;
        public Hash128 dataHash => m_DataHash;

        // ==================== 位置和缩放向量的存储格式编码 ====================
        // 3D高斯溅射有两个向量数据:
        // 1. 位置(Position): 溅射的3D空间位置
        // 2. 缩放(Scale): 控制溅射的高斯椭球体大小(log编码)
        public enum VectorFormat
        {
            Float32 = 0, // 12字节: 每个分量32位浮点数 - 最高精度，无损
            Norm16 = 1,  // 6字节: 每个分量16位归一化整数 - 中等精度
            Norm11 = 2,  // 4字节: 11.10.11位编码 - 高压缩，适合大多数使用场景
            Norm6 = 3    // 2字节: 6.5.5位编码 - 最高压缩，低质量
        }

        // 根据向量格式返回单个向量占用的字节数。
        // 用于计算位置/缩放等数据缓冲区大小。
        public static int GetVectorSize(VectorFormat fmt)
        {
            return fmt switch
            {
                VectorFormat.Float32 => 12,
                VectorFormat.Norm16 => 6,
                VectorFormat.Norm11 => 4,
                VectorFormat.Norm6 => 2,
                _ => throw new ArgumentOutOfRangeException(nameof(fmt), fmt, null)
            };
        }

        // ==================== 颜色数据的存储格式编码 ====================
        // 存储每个溅射的基础颜色(直流分量 DC0)
        // 3DGS使用球谐函数来表示颜色，DC0是0阶分量(方向无关的环境光颜色)
        public enum ColorFormat
        {
            Float32x4 = 0,  // 16字节: RGBA各32位浮点数 - 最高质量无损
            Float16x4 = 1,  // 8字节: RGBA各16位浮点数 - 高质量，减少内存
            Norm8x4 = 2,    // 4字节: RGBA各8位整数(0-1范围) - 中等质量
            BC7 = 3,        // 1字节: BC7压缩格式 - 最高压缩比
        }
        // 根据颜色格式返回单像素(单溅射颜色记录)占用的字节数。
        public static int GetColorSize(ColorFormat fmt)
        {
            return fmt switch
            {
                ColorFormat.Float32x4 => 16,
                ColorFormat.Float16x4 => 8,
                ColorFormat.Norm8x4 => 4,
                ColorFormat.BC7 => 1,
                _ => throw new ArgumentOutOfRangeException(nameof(fmt), fmt, null)
            };
        }

        // ==================== 球谐函数(Spherical Harmonics)的存储格式 ====================
        // 3DGS使用球谐函数来编码每个溅射的视角相关颜色
        // 球谐函数可以用来表示顶点上任何方向的光照/颜色信息
        // - Float32: 每个溅射存储完整的16个球谐系数(3个分量 x 15个高阶 + DC)
        // - Cluster: 使用K-Means聚类将多个溅射映射到共享的球谐表格,节省内存
        public enum SHFormat
        {
            Float32 = 0,      // 每个溅射独立存储完整的32位浮点系数 - 最高质量
            Float16 = 1,      // 每个溅射独立存储16位浮点系数 - 高质量，节省内存
            Norm11 = 2,       // 每个溅射独立存储11.10.11位编码系数 - 平衡质量和大小
            Norm6 = 3,        // 每个溅射独立存储6.5.5位编码系数 - 高压缩
            // 以下使用聚类(Clustering)存储:
            Cluster64k = 4,   // 64K个唯一的球谐表格项 - 低压缩比，高质量
            Cluster32k = 5,   // 32K个唯一的球谐表格项
            Cluster16k = 6,   // 16K个唯一的球谐表格项
            Cluster8k = 7,    // 8K个唯一的球谐表格项
            Cluster4k = 8,    // 4K个唯一的球谐表格项 - 高压缩比，低质量
        }

        public struct SHTableItemFloat32
        {
            public float3 sh1, sh2, sh3, sh4, sh5, sh6, sh7, sh8, sh9, shA, shB, shC, shD, shE, shF;
            public float3 shPadding; // pad to multiple of 16 bytes
        }
        public struct SHTableItemFloat16
        {
            public half3 sh1, sh2, sh3, sh4, sh5, sh6, sh7, sh8, sh9, shA, shB, shC, shD, shE, shF;
            public half3 shPadding; // pad to multiple of 16 bytes
        }
        public struct SHTableItemNorm11
        {
            public uint sh1, sh2, sh3, sh4, sh5, sh6, sh7, sh8, sh9, shA, shB, shC, shD, shE, shF;
        }
        public struct SHTableItemNorm6
        {
            public ushort sh1, sh2, sh3, sh4, sh5, sh6, sh7, sh8, sh9, shA, shB, shC, shD, shE, shF;
            public ushort shPadding; // pad to multiple of 4 bytes
        }

        // 初始化资产元数据。
        // 这里只写“描述信息”，真正的大块二进制数据由SetAssetFiles绑定。
        public void Initialize(int splats, VectorFormat formatPos, VectorFormat formatScale, ColorFormat formatColor, SHFormat formatSh, Vector3 bMin, Vector3 bMax, CameraInfo[] cameraInfos)
        {
            m_SplatCount = splats;
            m_FormatVersion = kCurrentVersion;
            m_PosFormat = formatPos;
            m_ScaleFormat = formatScale;
            m_ColorFormat = formatColor;
            m_SHFormat = formatSh;
            m_Cameras = cameraInfos;
            m_BoundsMin = bMin;
            m_BoundsMax = bMax;
        }

        // 写入数据哈希，用于运行时快速判断资产数据是否变更。
        public void SetDataHash(Hash128 hash)
        {
            m_DataHash = hash;
        }

        // 绑定导入后的二进制数据文件到资产。
        // 这些TextAsset会在运行时上传到GPU缓冲区/纹理。
        public void SetAssetFiles(TextAsset dataChunk, TextAsset dataPos, TextAsset dataOther, TextAsset dataColor, TextAsset dataSh)
        {
            m_ChunkData = dataChunk;
            m_PosData = dataPos;
            m_OtherData = dataOther;
            m_ColorData = dataColor;
            m_SHData = dataSh;
        }

        // 计算Other流中“旋转+缩放”部分大小(不含可选SH索引)。
        // 旋转固定4字节，缩放随VectorFormat变化。
        public static int GetOtherSizeNoSHIndex(VectorFormat scaleFormat)
        {
            return 4 + GetVectorSize(scaleFormat);
        }

        // 根据SH存储策略返回SH表项数量：
        // 1) 非聚类格式：每个溅射1条SH记录。
        // 2) 聚类格式：使用固定大小码本(4k~64k)。
        public static int GetSHCount(SHFormat fmt, int splatCount)
        {
            return fmt switch
            {
                SHFormat.Float32 => splatCount,
                SHFormat.Float16 => splatCount,
                SHFormat.Norm11 => splatCount,
                SHFormat.Norm6 => splatCount,
                SHFormat.Cluster64k => 64 * 1024,
                SHFormat.Cluster32k => 32 * 1024,
                SHFormat.Cluster16k => 16 * 1024,
                SHFormat.Cluster8k => 8 * 1024,
                SHFormat.Cluster4k => 4 * 1024,
                _ => throw new ArgumentOutOfRangeException(nameof(fmt), fmt, null)
            };
        }

        // 计算颜色纹理尺寸。
        // 颜色数据按固定宽度排布，同时高度对齐到16以匹配Morton分块布局。
        public static (int,int) CalcTextureSize(int splatCount)
        {
            int width = kTextureWidth;
            int height = math.max(1, (splatCount + width - 1) / width);
            // swizzle tile是16x16，因此高度需要对齐到16的整数倍。
            int blockHeight = 16;
            height = (height + blockHeight - 1) / blockHeight * blockHeight;
            return (width, height);
        }

        // 将资产颜色格式映射为Unity图形格式，供纹理创建/采样使用。
        public static GraphicsFormat ColorFormatToGraphics(ColorFormat format)
        {
            return format switch
            {
                ColorFormat.Float32x4 => GraphicsFormat.R32G32B32A32_SFloat,
                ColorFormat.Float16x4 => GraphicsFormat.R16G16B16A16_SFloat,
                ColorFormat.Norm8x4 => GraphicsFormat.R8G8B8A8_UNorm,
                ColorFormat.BC7 => GraphicsFormat.RGBA_BC7_UNorm,
                _ => throw new ArgumentOutOfRangeException(nameof(format), format, null)
            };
        }

        // 计算位置数据总字节数。
        public static long CalcPosDataSize(int splatCount, VectorFormat formatPos)
        {
            return splatCount * GetVectorSize(formatPos);
        }
        // 计算Other数据总字节数(旋转+缩放，不含SH索引扩展)。
        public static long CalcOtherDataSize(int splatCount, VectorFormat formatScale)
        {
            return splatCount * GetOtherSizeNoSHIndex(formatScale);
        }
        // 计算颜色数据总字节数(基于纹理尺寸而不是简单splatCount相乘)。
        public static long CalcColorDataSize(int splatCount, ColorFormat formatColor)
        {
            var (width, height) = CalcTextureSize(splatCount);
            return width * height * GetColorSize(formatColor);
        }
        // 计算SH数据总字节数。
        // 聚类格式除码本外，还需要每个溅射一个ushort索引。
        public static long CalcSHDataSize(int splatCount, SHFormat formatSh)
        {
            int shCount = GetSHCount(formatSh, splatCount);
            return formatSh switch
            {
                SHFormat.Float32 => shCount * UnsafeUtility.SizeOf<SHTableItemFloat32>(),
                SHFormat.Float16 => shCount * UnsafeUtility.SizeOf<SHTableItemFloat16>(),
                SHFormat.Norm11 => shCount * UnsafeUtility.SizeOf<SHTableItemNorm11>(),
                SHFormat.Norm6 => shCount * UnsafeUtility.SizeOf<SHTableItemNorm6>(),
                _ => shCount * UnsafeUtility.SizeOf<SHTableItemFloat16>() + splatCount * 2
            };
        }
        // 计算Chunk元数据总字节数。
        // 每个chunk存一条范围记录，chunk大小固定kChunkSize。
        public static long CalcChunkDataSize(int splatCount)
        {
            int chunkCount = (splatCount + kChunkSize - 1) / kChunkSize;
            return chunkCount * UnsafeUtility.SizeOf<ChunkInfo>();
        }

        [SerializeField] VectorFormat m_PosFormat = VectorFormat.Norm11;
        [SerializeField] VectorFormat m_ScaleFormat = VectorFormat.Norm11;
        [SerializeField] SHFormat m_SHFormat = SHFormat.Norm11;
        [SerializeField] ColorFormat m_ColorFormat;

        [SerializeField] TextAsset m_PosData;
        [SerializeField] TextAsset m_ColorData;
        [SerializeField] TextAsset m_OtherData;
        [SerializeField] TextAsset m_SHData;
        // Chunk data is optional (if data formats are fully lossless then there's no chunking)
        [SerializeField] TextAsset m_ChunkData;

        [SerializeField] CameraInfo[] m_Cameras;

        public VectorFormat posFormat => m_PosFormat;
        public VectorFormat scaleFormat => m_ScaleFormat;
        public SHFormat shFormat => m_SHFormat;
        public ColorFormat colorFormat => m_ColorFormat;

        public TextAsset posData => m_PosData;
        public TextAsset colorData => m_ColorData;
        public TextAsset otherData => m_OtherData;
        public TextAsset shData => m_SHData;
        public TextAsset chunkData => m_ChunkData;
        public CameraInfo[] cameras => m_Cameras;

        // ==================== 数据块信息结构体 ====================
        // 当使用压缩格式时，需要记录每个块内数据的最大/最小值范围
        // 这样可以在解码时正确地反量化(Dequantize)压缩的数据到原始范围
        public struct ChunkInfo
        {
            // 颜色RGBA的最小/最大值用float16编码
            public uint colR, colG, colB, colA;
            // 3D位置XYZ的最小/最大值用float32编码
            public float2 posX, posY, posZ;
            // 缩放因子XYZ的最小/最大值用float16编码
            public uint sclX, sclY, sclZ;
            // 球谐函数RGB通道的最小/最大值用float16编码
            public uint shR, shG, shB;
        }

        // ==================== 相机信息结构体 ====================
        // 存储在点云扫描时使用的相机参数
        // 允许直接跳转到特定视角进行编辑或操作
        [Serializable]
        public struct CameraInfo
        {
            public Vector3 pos;    // 相机世界空间位置
            public Vector3 axisX, axisY, axisZ; // 相机的3个坐标轴方向(旋转矩阵的列向量)
            public float fov;      // 竖直方向的视场角(Field of View)
        }
    }
}
