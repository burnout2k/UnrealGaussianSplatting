// SPDX-License-Identifier: MIT
// ============================================================================
// Shader: Unlit/BlackSkybox
// 功能: 黑色天空盒着色器 - 为背景提供纯黑色
// 说明:
//   在高斯溅射渲染中，背景通常使用黑色(alpha=0)
//   这样可以在合成时只显示溅射贡献的像素
//   这是一个简单的unlit着色器，直接返回设定的颜色
// ============================================================================
Shader "Unlit/BlackSkybox"
{
    Properties
    {
        _Color ("Color", Color) = (0,0,0,0)
    }
    SubShader
    {
        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #include "UnityCG.cginc"

            struct appdata
            {
                float4 vertex : POSITION;
            };

            struct v2f
            {
                float4 vertex : SV_POSITION;
            };

            // 顶点着色器：常规模型空间到裁剪空间变换
            v2f vert (appdata v)
            {
                v2f o;
                o.vertex = UnityObjectToClipPos(v.vertex);
                return o;
            }

            half4 _Color;

            // 片元着色器：输出固定颜色（默认纯黑）
            half4 frag (v2f i) : SV_Target
            {
                return _Color;
            }
            ENDCG
        }
    }
}



