// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianToolContext.cs
// 功能: 高斯溅射编辑上下文（Scene 视图交互）
// 说明:
//   - 将 Unity 标准工具上下文接入 GaussianSplatRenderer 的编辑流程
//   - 处理矩形框选、键盘快捷键、裁剪体与选中包围盒可视化
// ============================================================================

using System;
using GaussianSplatting.Runtime;
using UnityEditor;
using UnityEditor.EditorTools;
using UnityEngine;

namespace GaussianSplatting.Editor
{
    /// <summary>
    /// 高斯溅射编辑上下文
    /// 负责场景视图中的选择与可视化交互
    /// </summary>
    [EditorToolContext("GaussianSplats", typeof(GaussianSplatRenderer)), Icon(k_IconPath)]
    class GaussianToolContext : EditorToolContext
    {
        const string k_IconPath = "Packages/org.nesnausk.gaussian-splatting/Editor/Icons/GaussianContext.png";

        Vector2 m_MouseStartDragPos;

        // 根据当前 Unity 工具类型返回对应的 Gaussian 编辑工具。
        protected override Type GetEditorToolType(Tool tool)
        {
            if (tool == Tool.Move)
                return typeof(GaussianMoveTool);
            //if (tool == Tool.Rotate)
            //    return typeof(GaussianRotateTool); // not correctly working yet
            //if (tool == Tool.Scale)
            //    return typeof(GaussianScaleTool); // not working correctly yet when the GS itself has scale
            return null;
        }

        // 退出该上下文时清空选择，避免残留编辑状态。
        public override void OnWillBeDeactivated()
        {
            var gs = target as GaussianSplatRenderer;
            if (!gs)
                return;
            gs.EditDeselectAll();
        }

        // 处理场景视图中的编辑快捷命令（删除、全选、反选等）。
        static void HandleKeyboardCommands(Event evt, GaussianSplatRenderer gs)
        {
            // 仅处理 ValidateCommand / ExecuteCommand 两类命令事件。
            if (evt.type != EventType.ValidateCommand && evt.type != EventType.ExecuteCommand)
                return;
            bool execute = evt.type == EventType.ExecuteCommand;
            switch (evt.commandName)
            {
                // ugh, EventCommandNames string constants is internal :(
                case "SoftDelete":
                case "Delete":
                    if (execute)
                    {
                        gs.EditDeleteSelected();
                        GaussianSplatRendererEditor.RepaintAll();
                    }
                    evt.Use();
                    break;
                case "SelectAll":
                    if (execute)
                    {
                        gs.EditSelectAll();
                        GaussianSplatRendererEditor.RepaintAll();
                    }
                    evt.Use();
                    break;
                case "DeselectAll":
                    if (execute)
                    {
                        gs.EditDeselectAll();
                        GaussianSplatRendererEditor.RepaintAll();
                    }
                    evt.Use();
                    break;
                case "InvertSelection":
                    if (execute)
                    {
                        gs.EditInvertSelection();
                        GaussianSplatRendererEditor.RepaintAll();
                    }
                    evt.Use();
                    break;
            }
        }

        // 判断当前是否处于视图导航状态（不应触发选择编辑）。
        static bool IsViewToolActive()
        {
            return Tools.viewToolActive || Tools.current == Tool.View || (Event.current != null && Event.current.alt);
        }

        // Scene GUI 主入口：处理拖拽框选与可视化绘制。
        public override void OnToolGUI(EditorWindow window)
        {
            if (!(window is SceneView sceneView))
                return;
            var gs = target as GaussianSplatRenderer;
            if (!gs)
                return;

            // 驱动编辑统计的节流刷新计数器。
            GaussianSplatRendererEditor.BumpGUICounter();

            int id = GUIUtility.GetControlID(FocusType.Passive);
            Event evt = Event.current;
            HandleKeyboardCommands(evt, gs);
            var evtType = evt.GetTypeForControl(id);
            switch (evtType)
            {
                case EventType.Layout:
                    // 注册默认控制权，确保点击空白区域也能进入本工具处理。
                    HandleUtility.AddDefaultControl(id);
                    break;
                case EventType.MouseDown:
                    // 视图导航状态下不处理编辑输入。
                    if (IsViewToolActive())
                        break;
                    if (HandleUtility.nearestControl == id && evt.button == 0)
                    {
                        // 无组合键时，开始新选择；有组合键时走增删模式。
                        if (!evt.shift && !EditorGUI.actionKey && !evt.control)
                            gs.EditDeselectAll();

                        // 记录鼠标按下时的选择状态，供拖拽阶段增量计算。
                        gs.EditStoreSelectionMouseDown();
                        GaussianSplatRendererEditor.RepaintAll();

                        GUIUtility.hotControl = id;
                        m_MouseStartDragPos = evt.mousePosition;
                        evt.Use();
                    }
                    break;
                case EventType.MouseDrag:
                    if (GUIUtility.hotControl == id && evt.button == 0)
                    {
                        // 将屏幕拖拽矩形转为像素坐标并提交给 GPU 选择逻辑。
                        Rect rect = FromToRect(m_MouseStartDragPos, evt.mousePosition);
                        Vector2 rectMin = HandleUtility.GUIPointToScreenPixelCoordinate(rect.min);
                        Vector2 rectMax = HandleUtility.GUIPointToScreenPixelCoordinate(rect.max);
                        gs.EditUpdateSelection(rectMin, rectMax, sceneView.camera, evt.control);
                        GaussianSplatRendererEditor.RepaintAll();
                        evt.Use();
                    }
                    break;
                case EventType.MouseUp:
                    if (GUIUtility.hotControl == id && evt.button == 0)
                    {
                        // 结束拖拽并释放热控件。
                        m_MouseStartDragPos = Vector2.zero;
                        GUIUtility.hotControl = 0;
                        evt.Use();
                    }
                    break;
                case EventType.Repaint:
                    // 1) 绘制裁剪体线框（椭球/盒体）。
                    Handles.color = new Color(1,0,1,0.7f);
                    var prevMatrix = Handles.matrix;
                    foreach (var cutout in gs.m_Cutouts)
                    {
                        if (!cutout)
                            continue;
                        Handles.matrix = cutout.transform.localToWorldMatrix;
                        if (cutout.m_Type == GaussianCutout.Type.Ellipsoid)
                        {
                            Handles.DrawWireDisc(Vector3.zero, Vector3.up, 1.0f);
                            Handles.DrawWireDisc(Vector3.zero, Vector3.right, 1.0f);
                            Handles.DrawWireDisc(Vector3.zero, Vector3.forward, 1.0f);
                        }
                        if (cutout.m_Type == GaussianCutout.Type.Box)
                            Handles.DrawWireCube(Vector3.zero, Vector3.one * 2);
                    }

                    Handles.matrix = prevMatrix;
                    // 2) 绘制当前选中集合的包围盒。
                    if (gs.editSelectedSplats > 0)
                    {
                        var selBounds = GaussianSplatRendererEditor.TransformBounds(gs.transform, gs.editSelectedBounds);
                        Handles.DrawWireCube(selBounds.center, selBounds.size);
                    }
                    // 3) 绘制屏幕空间拖拽选择框。
                    if (GUIUtility.hotControl == id && evt.mousePosition != m_MouseStartDragPos)
                    {
                        GUIStyle style = "SelectionRect";
                        Handles.BeginGUI();
                        style.Draw(FromToRect(m_MouseStartDragPos, evt.mousePosition), false, false, false, false);
                        Handles.EndGUI();
                    }
                    break;
            }
        }

        // 根据起止点生成规范化矩形（宽高始终为正）。
        static Rect FromToRect(Vector2 from, Vector2 to)
        {
            if (from.x > to.x)
                (from.x, to.x) = (to.x, from.x);
            if (from.y > to.y)
                (from.y, to.y) = (to.y, from.y);
            return new Rect(from.x, from.y, to.x - from.x, to.y - from.y);
        }
    }
}



