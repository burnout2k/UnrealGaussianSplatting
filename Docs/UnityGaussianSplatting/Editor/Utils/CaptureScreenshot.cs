// SPDX-License-Identifier: MIT

using UnityEditor;
using UnityEngine;

namespace GaussianSplatting.Editor.Utils
{
    // 编辑器调试工具：自动生成不重复文件名并截图
    public class CaptureScreenshot : MonoBehaviour
    {
        [MenuItem("Tools/Gaussian Splats/Debug/Capture Screenshot %g")]
        // 捕获当前Game视图截图
        // 快捷 Ctrl/Cmd + G
        public static void CaptureShot()
        {
            // 递增查找第一个不存在的截图文件名
            int counter = 0;
            string path;
            while(true)
            {
                path = $"Shot-{counter:0000}.png";
                if (!System.IO.File.Exists(path))
                    break;
                ++counter;
            }

            // 调用Unity截图API并输出日志
            ScreenCapture.CaptureScreenshot(path);
            Debug.Log($"Captured {path}");
        }
    }
}



