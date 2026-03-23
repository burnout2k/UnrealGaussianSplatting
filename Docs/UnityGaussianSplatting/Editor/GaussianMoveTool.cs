// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianMoveTool.cs
// 功能: 高斯溅射移动工具 - 在场景中拖动选中的溅射
// ============================================================================

using GaussianSplatting.Runtime;
using UnityEditor;
using UnityEditor.EditorTools;
using UnityEngine;

namespace GaussianSplatting.Editor
{
    /// <summary>
    /// 高斯溅射移动工具
    /// 使用Unity位置手柄(Position Handle)移动当前选中的溅射点集合
    /// </summary>
    [EditorTool("Gaussian Move Tool", typeof(GaussianSplatRenderer), typeof(GaussianToolContext))]
    class GaussianMoveTool : GaussianTool
    {
        // Scene工具入口：显示移动手柄并提交位移结果。
        public override void OnToolGUI(EditorWindow window)
        {
            var gs = GetRenderer();
            // 渲染器无效、不可编辑或无选中时，不显示手柄。
            if (!gs || !CanBeEdited() || !HasSelection())
                return;
            var tr = gs.transform;

            // 记录手柄是否发生位移。
            EditorGUI.BeginChangeCheck();
            var selCenterLocal = GetSelectionCenterLocal();
            var selCenterWorld = tr.TransformPoint(selCenterLocal);
            var newPosWorld = Handles.DoPositionHandle(selCenterWorld, Tools.handleRotation);
            if (EditorGUI.EndChangeCheck())
            {
                // 世界位移转回局部空间后提交给GPU编辑系统。
                var newPosLocal = tr.InverseTransformPoint(newPosWorld);
                var wasModified = gs.editModified;
                gs.EditTranslateSelection(newPosLocal - selCenterLocal);

                // 首次进入修改态时刷新Inspector显示。
                if (!wasModified)
                    GaussianSplatRendererEditor.RepaintAll();
                Event.current.Use();
            }
        }
    }
}



