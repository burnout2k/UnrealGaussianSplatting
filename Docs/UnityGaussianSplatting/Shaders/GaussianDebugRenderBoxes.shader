// SPDX-License-Identifier: MIT
// ============================================================================
// Shader: Gaussian Splatting/Debug/Render Boxes
// 功能: 高斯溅射边界框调试着色器 - 显示溅射和数据块的包围盒
// 说明:
//   用于调试模式，可显示：
//   1. 每个溅射的边界框（由旋转、缩放、位置决定）
//   2. 数据块边界（显示压缩分块范围）
//   使用调色板函数为不同的块上色，便于可视化
// ============================================================================
Shader "Gaussian Splatting/Debug/Render Boxes"
{
    SubShader
    {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass
        {
            ZWrite Off
            Blend OneMinusDstAlpha One
            Cull Front

CGPROGRAM
#pragma vertex vert
#pragma fragment frag
#pragma require compute
#pragma use_dxc

#include "UnityCG.cginc"
#include "GaussianSplatting.hlsl"

StructuredBuffer<uint> _OrderBuffer;

bool _DisplayChunks;

struct v2f
{
    half4 col : COLOR0;
    float4 vertex : SV_POSITION;
};

float _SplatScale;
float _SplatOpacityScale;

// 基于余弦的调色板函数，用于给不同chunk分配可区分颜色
half3 palette(float t, half3 a, half3 b, half3 c, half3 d)
{
    return a + b*cos(6.28318*(c*t+d));
}

// 顶点着色器：支持两种调试绘制模式
// 1) 每个溅射的局部包围盒
// 2) 每个chunk的数据包围盒
v2f vert (uint vtxID : SV_VertexID, uint instID : SV_InstanceID)
{
    v2f o;
    bool chunks = _DisplayChunks;
	uint idx = vtxID;
	float3 localPos = float3(idx&1, (idx>>1)&1, (idx>>2)&1) * 2.0 - 1.0;

    float3 centerWorldPos = 0;

    if (!chunks)
    {
        // 模式A：显示单溅射包围盒
        instID = _OrderBuffer[instID];
        SplatData splat = LoadSplatData(instID);

        float4 boxRot = splat.rot;
        float3 boxSize = splat.scale;
        boxSize *= _SplatScale;

        // 由旋缩放构建包围盒方向与尺寸
        float3x3 splatRotScaleMat = CalcMatrixFromRotationScale(boxRot, boxSize);
        splatRotScaleMat = mul((float3x3)unity_ObjectToWorld, splatRotScaleMat);

        centerWorldPos = splat.pos;
        centerWorldPos = mul(unity_ObjectToWorld, float4(centerWorldPos,1)).xyz;

        // 颜色与透明度继承溅射属性
        o.col.rgb = saturate(splat.sh.col);
        o.col.a = saturate(splat.opacity * _SplatOpacityScale);

        localPos = mul(splatRotScaleMat, localPos) * 2;
    }
    else
    {
        // 模式B：显示chunk包围盒
        localPos = localPos * 0.5 + 0.5;
        SplatChunkInfo chunk = _SplatChunks[instID];
        float3 posMin = float3(chunk.posX.x, chunk.posY.x, chunk.posZ.x);
        float3 posMax = float3(chunk.posX.y, chunk.posY.y, chunk.posZ.y);

        localPos = lerp(posMin, posMax, localPos);
        localPos = mul(unity_ObjectToWorld, float4(localPos,1)).xyz;

        // 使用调色板按chunk索引着色，方便区分分块
        o.col.rgb = palette((float)instID / (float)_SplatChunkCount, half3(0.5,0.5,0.5), half3(0.5,0.5,0.5), half3(1,1,1), half3(0.0, 0.33, 0.67));
        o.col.a = 0.1;
    }

    float3 worldPos = centerWorldPos + localPos;
    o.vertex = UnityWorldToClipPos(worldPos);
    FlipProjectionIfBackbuffer(o.vertex);
    return o;
}

// 片元着色器：输出预乘alpha颜色
half4 frag (v2f i) : SV_Target
{
    half4 res = half4(i.col.rgb * i.col.a, i.col.a);
    return res;
}
ENDCG
        }
    }
}



