// SPDX-License-Identifier: MIT
// ============================================================================
// Shader: Gaussian Splatting/Debug/Render Points
// 功能: 高斯溅射调试着色器 - 将溅射显示为彩色
// 说明:
//   用于调试模式，显示每个溅射为一个点
//   可以显示原始颜色或显示溅射索引号
//   有助于理解溅射分布和排序结果
// ============================================================================
Shader "Gaussian Splatting/Debug/Render Points"
{
    SubShader
    {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass
        {
            ZWrite On
            Cull Off
            
CGPROGRAM
#pragma vertex vert
#pragma fragment frag
#pragma require compute
#pragma use_dxc

#include "GaussianSplatting.hlsl"

struct v2f
{
    half3 color : TEXCOORD0;
    float4 vertex : SV_POSITION;
};

float _SplatSize;
bool _DisplayIndex;
int _SplatCount;

// 调试顶点着色器：把每个溅射渲染成屏幕小方片
v2f vert (uint vtxID : SV_VertexID, uint instID : SV_InstanceID)
{
    v2f o;
    uint splatIndex = instID;

    // 读取溅射原始数据
    SplatData splat = LoadSplatData(splatIndex);

    float3 centerWorldPos = splat.pos;
    centerWorldPos = mul(unity_ObjectToWorld, float4(centerWorldPos,1)).xyz;

    // 计算中心点裁剪空间位置
    float4 centerClipPos = mul(UNITY_MATRIX_VP, float4(centerWorldPos, 1));

    // 根据点大小扩展为屏幕空间四边形
    o.vertex = centerClipPos;
	uint idx = vtxID;
    float2 quadPos = float2(idx&1, (idx>>1)&1) * 2.0 - 1.0;
    o.vertex.xy += (quadPos * _SplatSize / _ScreenParams.xy) * o.vertex.w;

    // 默认显示基础颜色
    o.color.rgb = saturate(splat.sh.col);
    if (_DisplayIndex)
    {
        // 调试模式：把索引编码为颜色以观察排序与分布
        o.color.r = frac((float)splatIndex / (float)_SplatCount * 100);
        o.color.g = frac((float)splatIndex / (float)_SplatCount * 10);
        o.color.b = (float)splatIndex / (float)_SplatCount;
    }

    FlipProjectionIfBackbuffer(o.vertex);
    return o;
}

// 调试片元：直接输出颜色，不做高斯衰减
half4 frag (v2f i) : SV_Target
{
    return half4(i.color.rgb, 1);
}
ENDCG
        }
    }
}



