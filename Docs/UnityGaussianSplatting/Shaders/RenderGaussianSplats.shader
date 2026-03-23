// SPDX-License-Identifier: MIT
// ============================================================================
// Shader: Gaussian Splatting/Render Splats
// 功能: 高斯溅射渲染着色器 - 绘制高斯椭球体点
// 说明:
//   这是3D高斯溅射的核心渲染着色器
//   功能包括:
//   1. 顶点着色器: 
//      - 从排序缓冲区获取溅射索引
//      - 将 3D 高斯椭球体投影到屏幕空间 2D 椭圆
//      - 计算高斯函数参数（供片段着色器使用）
//   2. 片段着色器:
//      - 计算像素到椭圆中心的高斯衰减(alpha混合)
//      - 计算最终颜色（基础颜色 + 球谐函数贡献）
//      - 处理选中溅射的高亮显示
// ============================================================================
Shader "Gaussian Splatting/Render Splats"
{
    SubShader
    {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass
        {
            ZWrite Off
            Blend OneMinusDstAlpha One
            Cull Off
            
CGPROGRAM
#pragma vertex vert
#pragma fragment frag
#pragma require compute
#pragma use_dxc

#include "GaussianSplatting.hlsl"

StructuredBuffer<uint> _OrderBuffer;

struct v2f
{
    half4 col : COLOR0;
    float2 pos : TEXCOORD0;
    float4 vertex : SV_POSITION;
};

StructuredBuffer<SplatViewData> _SplatViewData;
ByteAddressBuffer _SplatSelectedBits;
uint _SplatBitsValid;

// 顶点着色器
// 1) 根据排序索引读取当前溅射
// 2) 计算屏幕空间四边形顶点
// 3) 传递颜色与选中状态到片元阶段
v2f vert (uint vtxID : SV_VertexID, uint instID : SV_InstanceID)
{
    v2f o = (v2f)0;
    // 通过排序后的索引访问溅射，保证混合顺序正确
    instID = _OrderBuffer[instID];
	SplatViewData view = _SplatViewData[instID];
	float4 centerClipPos = view.pos;
	bool behindCam = centerClipPos.w <= 0;
	if (behindCam)
	{
		// 在相机后方：写入NaN丢弃该图元
		o.vertex = asfloat(0x7fc00000); // NaN discards the primitive
	}
	else
	{
		// 解包打包后的half颜色
		o.col.r = f16tof32(view.color.x >> 16);
		o.col.g = f16tof32(view.color.x);
		o.col.b = f16tof32(view.color.y >> 16);
		o.col.a = f16tof32(view.color.y);

		// 通过vtxID构造quad四个角[-1,1]
		uint idx = vtxID;
		float2 quadPos = float2(idx&1, (idx>>1)&1) * 2.0 - 1.0;
		quadPos *= 2;

		o.pos = quadPos;

		// 使用屏幕空间主轴把圆盘拉伸为椭圆
		float2 deltaScreenPos = (quadPos.x * view.axis1 + quadPos.y * view.axis2) * 2 / _ScreenParams.xy;
		o.vertex = centerClipPos;
		o.vertex.xy += deltaScreenPos * centerClipPos.w;

		// 若该溅射被选中，使用负alpha标记给片元阶段做高亮
		if (_SplatBitsValid)
		{
			uint wordIdx = instID / 32;
			uint bitIdx = instID & 31;
			uint selVal = _SplatSelectedBits.Load(wordIdx * 4);
			if (selVal & (1 << bitIdx))
			{
				o.col.a = -1;				
			}
		}
	}
	FlipProjectionIfBackbuffer(o.vertex);
    return o;
}

// 片元着色器：计算高斯权重alpha并输出预乘颜色
half4 frag (v2f i) : SV_Target
{
	// 高斯函数值：exp(-r^2)
	float power = -dot(i.pos, i.pos);
	half alpha = exp(power);
	if (i.col.a >= 0)
	{
		// 普通溅射：按原始alpha调制
		alpha = saturate(alpha * i.col.a);
	}
	else
	{
		// 选中溅射：提高可见度并加洋红色边缘与染色
		half3 selectedColor = half3(1,0,1);
		if (alpha > 7.0/255.0)
		{
			if (alpha < 10.0/255.0)
			{
				alpha = 1;
				i.col.rgb = selectedColor;
			}
			alpha = saturate(alpha + 0.3);
		}
		i.col.rgb = lerp(i.col.rgb, selectedColor, 0.5);
	}
	// 过小alpha直接丢弃，减少无效混合开销
    if (alpha < 1.0/255.0)
        discard;

	// 输出预乘alpha颜色，配合Blend OneMinusDstAlpha One
    half4 res = half4(i.col.rgb * alpha, alpha);
    return res;
}
ENDCG
        }
    }
}



