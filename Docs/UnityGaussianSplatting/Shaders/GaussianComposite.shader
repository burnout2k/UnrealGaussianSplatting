// SPDX-License-Identifier: MIT
// ============================================================================
// Shader: Hidden/Gaussian Splatting/Composite
// 功能: 高斯溅射合成着色器 - 将渲染结果合成到最终图
// 说明:
//   高斯溅射渲染流程:
//   1. 高斯点渲染到临时纹理 (_GaussianSplatRT)
//   2. 本着色器将临时纹理结果（预乘 alpha）合成到最终画面
//   
//   操作步骤:
//   - 从高斯溅射临时RT读取预乘后的颜色(RGBA)
//   - 反向gamma校正从线性空间转换到sRGB
//   - 还原颜色与透明度，得到非预乘颜色
//   - 使用 SrcAlpha / OneMinusSrcAlpha 混合到背景
// ============================================================================
Shader "Hidden/Gaussian Splatting/Composite"
{
    SubShader
    {
        Pass
        {
            ZWrite Off
            ZTest Always
            Cull Off
            Blend SrcAlpha OneMinusSrcAlpha

CGPROGRAM
#pragma vertex vert
#pragma fragment frag
#pragma require compute
#pragma use_dxc
#include "UnityCG.cginc"

struct v2f
{
    float4 vertex : SV_POSITION;
};

// 全屏三角形顶点着色器
v2f vert (uint vtxID : SV_VertexID)
{
    v2f o;
    // 通过顶点ID构造覆盖全屏的三角形坐标
    float2 quadPos = float2(vtxID&1, (vtxID>>1)&1) * 4.0 - 1.0;
	o.vertex = float4(quadPos, 1, 1);
    return o;
}

Texture2D _GaussianSplatRT;

// 合成片元：读取高斯中间RT并还原颜色后输出
half4 frag (v2f i) : SV_Target
{
    // 读取预乘alpha颜色
    half4 col = _GaussianSplatRT.Load(int3(i.vertex.xy, 0));
    // 反预乘并从gamma空间转换到线性空间
    return float4(GammaToLinearSpace(col.rgb/col.a),col.a);
}
ENDCG
        }
    }
}



