// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianRotateTool.cs
// 功能: 高斯溅射旋转工具（当前未启用）
// 说明: 
//   该工具用于旋转选中的溅射点，但目前实现尚未稳定，因此保留在注释块中。
// ============================================================================

using GaussianSplatting.Runtime;
using UnityEditor;
using UnityEditor.EditorTools;
using UnityEngine;

namespace GaussianSplatting.Editor
{
    /* not working correctly yet
    /// <summary>
    /// 高斯溅射旋转工具（未启用）
    /// 使用旋转手柄对选中溅射执行旋转变换
    /// </summary>
    [EditorTool("Gaussian Rotate Tool", typeof(GaussianSplatRenderer), typeof(GaussianToolContext))]
    class GaussianRotateTool : GaussianTool
    {
        Quaternion m_CurrentRotation = Quaternion.identity;
        Vector3 m_FrozenSelCenterLocal = Vector3.zero;
        bool m_FreezePivot = false;

        // 激活工具时重置内部状态。
        public override void OnActivated()
        {
            m_FreezePivot = false;
        }

        // Scene工具入口：绘制旋转手柄并提交旋转结果。
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
                // 记录鼠标按下时的原始数据，避免增量误差累积。
                gs.EditStorePosMouseDown();
                gs.EditStoreOtherMouseDown();
                m_FrozenSelCenterLocal = selCenterLocal;
                m_FreezePivot = true;
            }
            if (evt.type == EventType.MouseUp)
            {
                // 鼠标释放后重置旋转增量状态。
                m_CurrentRotation = Quaternion.identity;
                m_FreezePivot = false;
            }

            // 拖拽过程中固定旋转中心，避免包围盒变化导致枢轴漂移。
            if (m_FreezePivot)
                selCenterLocal = m_FrozenSelCenterLocal;

            EditorGUI.BeginChangeCheck();
            var selCenterWorld = tr.TransformPoint(selCenterLocal);
            var newRotation = Handles.DoRotationHandle(m_CurrentRotation, selCenterWorld);
            if (EditorGUI.EndChangeCheck())
            {
                // 提交旋转到GPU编辑系统。
                Matrix4x4 localToWorld = gs.transform.localToWorldMatrix;
                Matrix4x4 worldToLocal = gs.transform.worldToLocalMatrix;
                var wasModified = gs.editModified;
                var rotToApply = newRotation;
                gs.EditRotateSelection(selCenterLocal, localToWorld, worldToLocal, rotToApply);
                m_CurrentRotation = newRotation;
                if (!wasModified)
                    GaussianSplatRendererEditor.RepaintAll();

                // 手柄结束操作后与Unity手柄旋转状态保持同步。
                if(GUIUtility.hotControl == 0)
                {
                    m_CurrentRotation = Tools.handleRotation;
                }
            }
        }
    }
    */
}



