// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianCutout.cs
// 功能: 高斯溅射裁剪体 - 定义不可见区域
// 说明: 
//   在高斯溅射优化中，可以定义椭球体或立方体形状的区域
//   位于这些区域内的溅射会被标记为删除(Cutout)
//   支持反向物业(Invert)来定义需要保留的区域而不是删除的区域
// ============================================================================

using System.Linq;
using UnityEditor;
using UnityEngine;

namespace GaussianSplatting.Runtime
{
    /// <summary>
    /// 高斯溅射裁剪元件
    /// 用于定义一个空间区域，该区域内的溅射将被自动删除
    /// 支持椭球体(Ellipsoid)和立方体(Box)两种形状
    /// </summary>
    public class GaussianCutout : MonoBehaviour
    {
        // 裁剪形状类型
        public enum Type
        {
            Ellipsoid = 0, // 椭球体形状 - 用于有机的曲面裁剪
            Box = 1        // 立方体形状 - 用于直线的几何体裁剪
        }

        public Type m_Type = Type.Ellipsoid; // 当前裁剪体的形状
        public bool m_Invert = false; // 如果true，则反向逻辑:保留内部的溅射，删除外部的

        // 传给ComputeShader的裁剪体数据结构。
        // 字段布局需要与着色器侧结构严格一致。
        public struct ShaderData // match GaussianCutoutShaderData in CS
        {
            public Matrix4x4 matrix;
            public uint typeAndFlags;
        }

        // 生成单个裁剪体对应的GPU数据。
        // rendererMatrix用于把裁剪体空间转换到当前高斯对象空间。
        public static ShaderData GetShaderData(GaussianCutout self, Matrix4x4 rendererMatrix)
        {
            ShaderData sd = default;
            if (self && self.isActiveAndEnabled)
            {
                // 有效裁剪体：写入从渲染对象空间到裁剪体局部空间的变换矩阵。
                var tr = self.transform;
                sd.matrix = tr.worldToLocalMatrix * rendererMatrix;
                // 低位存类型，高位标记是否反向。
                sd.typeAndFlags = ((uint)self.m_Type) | (self.m_Invert ? 0x100u : 0u);
            }
            else
            {
                // 无效裁剪体：用全1标记，着色器侧可直接忽略。
                sd.typeAndFlags = ~0u;
            }
            return sd;
        }

#if UNITY_EDITOR
        // 在Scene视图绘制裁剪体Gizmo，方便编辑时观察影响范围。
        public void OnDrawGizmos()
        {
            Gizmos.matrix = transform.localToWorldMatrix;
            var color = Color.magenta;
            color.a = 0.2f;
            if (Selection.Contains(gameObject))
                // 自身被选中时提高可见性。
                color.a = 0.9f;
            else
            {
                // 若选中了引用本裁剪体的高斯对象，则用中等透明度提示关联关系。
                var activeGo = Selection.activeGameObject;
                if (activeGo != null)
                {
                    var activeSplat = activeGo.GetComponent<GaussianSplatRenderer>();
                    if (activeSplat != null)
                    {
                        if (activeSplat.m_Cutouts != null && activeSplat.m_Cutouts.Contains(this))
                            color.a = 0.5f;
                    }
                }
            }

            Gizmos.color = color;
            if (m_Type == Type.Ellipsoid)
            {
                // 椭球模式下使用球线框表示(后续由transform缩放成椭球)。
                Gizmos.DrawWireSphere(Vector3.zero, 1.0f);
            }
            if (m_Type == Type.Box)
            {
                // 盒体模式下绘制边长2的单位立方体线框。
                Gizmos.DrawWireCube(Vector3.zero, Vector3.one * 2);
            }
        }
#endif // #if UNITY_EDITOR
    }
}
