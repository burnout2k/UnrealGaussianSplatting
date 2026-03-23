// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianScaleTool.cs
// 功能: 高斯溅射缩放工具（当前未启用）
// 说明:
//   该工具用于缩放选中的溅射点，但在对象自身存在缩放时结果不稳定，暂未启用。
// ============================================================================

using GaussianSplatting.Runtime;
using UnityEditor;
using UnityEditor.EditorTools;
using UnityEngine;

namespace GaussianSplatting.Editor
{
    /* // not working correctly yet when the GS itself has scale
    /// <summary>
    /// 高斯溅射缩放工具（未启用）
    /// 使用缩放手柄对选中溅射执行缩放变换
    /// </summary>
    [EditorTool("Gaussian Scale Tool", typeof(GaussianSplatRenderer), typeof(GaussianToolContext))]
    class GaussianScaleTool : GaussianTool
    {
        Vector3 m_CurrentScale = Vector3.one;
        Vector3 m_FrozenSelCenterLocal = Vector3.zero;
        bool m_FreezePivot = false;

        // 激活工具时重置内部状态。
        public override void OnActivated()
        {
            m_FreezePivot = false;
        }

        // Scene工具入口：绘制缩放手柄并提交缩放结果。
        public override void OnToolGUI(EditorWindow window)
        {
            var gs = GetRenderer();
            // 资源无效、不可编辑或无选中时直接退出。
            if (!gs || !CanBeEdited() || !HasSelection())
                return;
            var tr = gs.transform;
            var evt = Event.current;

            var selCenterLocal = GetSelectionCenterLocal();
            if (evt.type == EventType.MouseDown)
            {
                // 记录鼠标按下时的原始位置数据，避免累计误差。
                gs.EditStorePosMouseDown();
                m_FrozenSelCenterLocal = selCenterLocal;
                m_FreezePivot = true;
            }
            if (evt.type == EventType.MouseUp)
            {
                // 鼠标释放后重置缩放增量。
                m_CurrentScale = Vector3.one;
                m_FreezePivot = false;
            }

            // 拖拽过程中固定缩放中心。
            if (m_FreezePivot)
                selCenterLocal = m_FrozenSelCenterLocal;

            EditorGUI.BeginChangeCheck();
            var selCenterWorld = tr.TransformPoint(selCenterLocal);
            m_CurrentScale = Handles.DoScaleHandle(m_CurrentScale, selCenterWorld, Tools.handleRotation, HandleUtility.GetHandleSize(selCenterWorld));
            if (EditorGUI.EndChangeCheck())
            {
                // 全局枢轴模式下需要传入对象变换矩阵用于空间换算。
                Matrix4x4 localToWorld = Matrix4x4.identity;
                Matrix4x4 worldToLocal = Matrix4x4.identity;
                if (Tools.pivotRotation == PivotRotation.Global)
                {
                    localToWorld = gs.transform.localToWorldMatrix;
                    worldToLocal = gs.transform.worldToLocalMatrix;
                }

                // 提交缩放到GPU编辑系统。
                var wasModified = gs.editModified;
                gs.EditScaleSelection(selCenterLocal, localToWorld, worldToLocal, m_CurrentScale);
                if (!wasModified)
                    GaussianSplatRendererEditor.RepaintAll();
                evt.Use();
            }
        }
    }
    */
}



