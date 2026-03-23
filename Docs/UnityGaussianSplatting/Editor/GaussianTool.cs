// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianTool.cs
// 功能: 高斯溅射编辑工具基类
// 说明: 所有高斯溅射编辑工具（移动/旋转/缩放）的抽象基类
// ============================================================================

using GaussianSplatting.Runtime;
using UnityEditor.EditorTools;
using UnityEngine;

namespace GaussianSplatting.Editor
{
    /// <summary>
    /// 高斯溅射编辑工具基类
    /// 提供共同的验证和获取方法
    /// </summary>
    abstract class GaussianTool : EditorTool
    {
        // 获取当前工具目标上的GaussianSplatRenderer，并校验资产与GPU资源是否可用
        protected GaussianSplatRenderer GetRenderer()
        {
            var gs = target as GaussianSplatRenderer;
            // 目标不存在、资产无效或GPU资源未准备好时，不允许进入编辑逻辑
            if (!gs || !gs.HasValidAsset || !gs.HasValidRenderSetup)
                return null;
            return gs;
        }

        // 判断当前对象是否支持编辑。
        // 仅无 chunk（无损/高质量）资产支持点级编辑。
        protected bool CanBeEdited()
        {
            var gs = GetRenderer();
            if (!gs)
                return false;
            return gs.asset.chunkData == null; // need to be lossless / non-chunked for editing
        }

        // 判断当前是否存在已选中的溅射点
        protected bool HasSelection()
        {
            var gs = GetRenderer();
            if (!gs)
                return false;
            return gs.editSelectedSplats > 0;
        }

        // 获取选中集合中心（局部空间）。
        protected Vector3 GetSelectionCenterLocal()
        {
            var gs = GetRenderer();
            if (!gs || gs.editSelectedSplats == 0)
                return Vector3.zero;
            return gs.editSelectedBounds.center;
        }
    }
}



