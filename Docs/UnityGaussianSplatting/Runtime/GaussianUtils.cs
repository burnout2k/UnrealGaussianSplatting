// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianUtils.cs
// 功能: 高斯溅射工具函数库
// 说明:
//   提供与3DGS数学相关的工具函数:
//   - Sigmoid函数: 从原始值转换为[0,1]范围
//   - 球谐函数转颜色: 从0阶基础颜色系数计算最终颜色
//   - 旋转和缩放编码: 处理四元数和对数缩放向量
//   - Morton编码: 用于空间数据的SFC(空间填充曲线)排序
// ============================================================================

using Unity.Mathematics;

namespace GaussianSplatting.Runtime
{
    /// <summary>
    /// 高斯溅射数学工具函数集合
    /// </summary>
    public static class GaussianUtils
    {
        // Sigmoid激活函数，将实数映射到(0,1)。
        // 主要用于将网络输出/中间参数转换到概率或归一化区间。
        public static float Sigmoid(float v)
        {
            return math.rcp(1.0f + math.exp(-v));
        }

        // 将SH的0阶系数转换为基础颜色。
        // 公式: color = dc0 * SH_C0 + 0.5
        public static float3 SH0ToColor(float3 dc0)
        {
            const float kSH_C0 = 0.2820948f;
            return dc0 * kSH_C0 + 0.5f;
        }

        // 将对数缩放恢复到线性缩放。
        // 使用exp并取绝对值，避免负尺度带来的不稳定。
        public static float3 LinearScale(float3 logScale)
        {
            return math.abs(math.exp(logScale));
        }

        // 对[0,1]区间做“中心保持、两端拉伸”的非线性变换。
        // 在0.5附近变化平缓，靠近两端变化更明显。
        public static float SquareCentered01(float x)
        {
            x -= 0.5f;
            x *= x * math.sign(x);
            return x * 2.0f + 0.5f;
        }

        // SquareCentered01的逆变换，用于解码回原始空间。
        public static float InvSquareCentered01(float x)
        {
            x -= 0.5f;
            x *= 0.5f;
            x = math.sqrt(math.abs(x)) * math.sign(x);
            return x + 0.5f;
        }

        // 归一化四元数并重排分量顺序。
        // 输入为wxyz，输出为xyzw(通过yzwx完成重排)。
        public static float4 NormalizeSwizzleRotation(float4 wxyz)
        {
            return math.normalize(wxyz).yzwx;
        }

        // “Smallest-3”四元数压缩：
        // 返回最小的3个分量到xyz(映射到0..1)，w中存最大分量索引(0或1，除以3后)。
        public static float4 PackSmallest3Rotation(float4 q)
        {
            // 1) 找到绝对值最大的分量索引，后续会丢弃该分量并在解码时重建。
            float4 absQ = math.abs(q);
            int index = 0;
            float maxV = absQ.x;
            if (absQ.y > maxV)
            {
                index = 1;
                maxV = absQ.y;
            }
            if (absQ.z > maxV)
            {
                index = 2;
                maxV = absQ.z;
            }
            if (absQ.w > maxV)
            {
                index = 3;
                maxV = absQ.w;
            }

            // 2) 将“要丢弃的最大分量”旋转到w位，保留xyz三分量。
            if (index == 0) q = q.yzwx;
            if (index == 1) q = q.xzwy;
            if (index == 2) q = q.xywz;

            // 3) 统一符号并把[-1/sqrt(2), +1/sqrt(2)]映射到[0,1]便于量化存储。
            float3 three = q.xyz * (q.w >= 0 ? 1 : -1); // -1/sqrt2..+1/sqrt2 range
            three = (three * math.SQRT2) * 0.5f + 0.5f; // 0..1 range

            return new float4(three, index / 3.0f);
        }


        // 基于Morton编码的位扩展步骤：
        // 对输入的低21位，每位后插入2个0，为3D交错编码做准备。
        static ulong MortonPart1By2(ulong x)
        {
            x &= 0x1fffff;
            x = (x ^ (x << 32)) & 0x1f00000000ffffUL;
            x = (x ^ (x << 16)) & 0x1f0000ff0000ffUL;
            x = (x ^ (x << 8)) & 0x100f00f00f00f00fUL;
            x = (x ^ (x << 4)) & 0x10c30c30c30c30c3UL;
            x = (x ^ (x << 2)) & 0x1249249249249249UL;
            return x;
        }
        // 将3个21位坐标编码成3D Morton码(Z-order curve)。
        // 用于空间局部性更好的重排。
        public static ulong MortonEncode3(uint3 v)
        {
            return (MortonPart1By2(v.z) << 2) | (MortonPart1By2(v.y) << 1) | MortonPart1By2(v.x);
        }

        // 解码16x16 tile内的2D Morton索引。
        // 与GPU端GaussianSplatting.hlsl保持一致。
        public static uint2 DecodeMorton2D_16x16(uint t)
        {
            t = (t & 0xFF) | ((t & 0xFE) << 7); // -EAFBGCHEAFBGCHD
            t &= 0x5555;                        // -E-F-G-H-A-B-C-D
            t = (t ^ (t >> 1)) & 0x3333;        // --EF--GH--AB--CD
            t = (t ^ (t >> 2)) & 0x0f0f;        // ----EFGH----ABCD
            return new uint2(t & 0xF, t >> 8);  // --------EFGHABCD
        }
    }
}
