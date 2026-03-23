// SPDX-License-Identifier: MIT
// ============================================================================
// 文件: GaussianSplatAssetEditor.cs
// 功能: 高斯溅射资产编辑器（Inspector）
// 说明: 自定义 Inspector 面板，显示资产元数据（数量、大小、数据格式等）
// ============================================================================

using GaussianSplatting.Runtime;
using Unity.Collections.LowLevel.Unsafe;
using UnityEditor;
using UnityEngine;

namespace GaussianSplatting.Editor
{
    /// <summary>
    /// 高斯溅射资产编辑
    /// 为 Inspector 显示资产详细信息
    /// </summary>
    [CustomEditor(typeof(GaussianSplatAsset))]
    [CanEditMultipleObjects]
    public class GaussianSplatAssetEditor : UnityEditor.Editor
    {
        // 绘制资产Inspector界面
        // 单选时展示详细信息，多选时显示汇总统计
        public override void OnInspectorGUI()
        {
            var gs = target as GaussianSplatAsset;
            if (!gs)
                return;

            // 资产信息只读展示，不允许直接编辑
            using var _ = new EditorGUI.DisabledScope(true);

            if (targets.Length == 1)
                SingleAssetGUI(gs);
            else
            {
                int totalCount = 0;
                foreach (var tgt in targets)
                {
                    var gss = tgt as GaussianSplatAsset;
                    if (gss)
                    {
                        totalCount += gss.splatCount;
                    }
                }
                EditorGUILayout.TextField("Total Splats", $"{totalCount:N0}");
            }
        }

        // 单个资产的详细信息面板
        static void SingleAssetGUI(GaussianSplatAsset gs)
        {
            var splatCount = gs.splatCount;
            EditorGUILayout.TextField("Splats", $"{splatCount:N0}");
            var prevBackColor = GUI.backgroundColor;
            // 版本不匹配时高亮提醒需要重建资产
            if (gs.formatVersion != GaussianSplatAsset.kCurrentVersion)
                GUI.backgroundColor *= Color.red;
            EditorGUILayout.IntField("Version", gs.formatVersion);
            GUI.backgroundColor = prevBackColor;

            long sizePos = gs.posData != null ? gs.posData.dataSize : 0;
            long sizeOther = gs.otherData != null ? gs.otherData.dataSize : 0;
            long sizeCol = gs.colorData != null ? gs.colorData.dataSize : 0;
            long sizeSH = GaussianSplatAsset.CalcSHDataSize(gs.splatCount, gs.shFormat);
            long sizeChunk = gs.chunkData != null ? gs.chunkData.dataSize : 0;

            // 计算各数据流内存占用并展示
            EditorGUILayout.TextField("Memory", EditorUtility.FormatBytes(sizePos + sizeOther + sizeSH + sizeCol + sizeChunk));
            EditorGUI.indentLevel++;
            EditorGUILayout.TextField("Positions", $"{EditorUtility.FormatBytes(sizePos)}  ({gs.posFormat})");
            EditorGUILayout.TextField("Other", $"{EditorUtility.FormatBytes(sizeOther)}  ({gs.scaleFormat})");
            EditorGUILayout.TextField("Base color", $"{EditorUtility.FormatBytes(sizeCol)}  ({gs.colorFormat})");
            EditorGUILayout.TextField("SHs", $"{EditorUtility.FormatBytes(sizeSH)}  ({gs.shFormat})");
            EditorGUILayout.TextField("Chunks",
                $"{EditorUtility.FormatBytes(sizeChunk)}  ({UnsafeUtility.SizeOf<GaussianSplatAsset.ChunkInfo>()} B/chunk)");
            EditorGUI.indentLevel--;

            EditorGUILayout.Vector3Field("Bounds Min", gs.boundsMin);
            EditorGUILayout.Vector3Field("Bounds Max", gs.boundsMax);

            EditorGUILayout.TextField("Data Hash", gs.dataHash.ToString());
        }
    }
}



