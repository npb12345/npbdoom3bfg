/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <blit.cb.h>
#ifdef SPIRV
[[vk::push_constant]] ConstantBuffer<BlitConstants> g_Blit;
#else
cbuffer c_Blit : register( b0 ) { BlitConstants g_Blit; }
#endif

// Absolute luminance in nits -> SMPTE ST 2084 (PQ).
float3 EncodePQ( float3 nits )
{
	float3 p = pow( saturate( nits / 10000.0 ), 2610.0 / 16384.0 );
	return pow( ( 3424.0 / 4096.0 + ( 2413.0 / 128.0 ) * p ) /
		( 1.0 + ( 2392.0 / 128.0 ) * p ), 2523.0 / 32.0 );
}

// *INDENT-OFF*
#if TEXTURE_ARRAY
Texture2DArray tex : register( t0 );
#else
Texture2D tex : register( t0 );
#endif
SamplerState samp : register( s0 );

struct PS_IN
{
	float4 posClip	: SV_Position;
	float2 uv		: UV;
};
// *INDENT-ON*

void main(
	PS_IN fragment,
	out float4 o_rgba : SV_Target )
{
#if TEXTURE_ARRAY
	o_rgba = tex.Sample( samp, float3( fragment.uv, 0 ) );
#else
	o_rgba = tex.Sample( samp, fragment.uv );
#endif
	if( g_Blit.hdrPaperWhiteNits > 0.0 )
	{
		// Legacy GUI blending stays in extended gamma 2.2 space. Decode only
		// once, after composition; FP16 retains values above diffuse white.
		float3 rgb = min( pow( max( o_rgba.rgb, 0.0 ), 2.2 ) * g_Blit.hdrPaperWhiteNits, g_Blit.hdrPeakNits );
		float3 rec2020 = float3(
			dot( rgb, float3( 0.627404, 0.329283, 0.043313 ) ),
			dot( rgb, float3( 0.069097, 0.919540, 0.011363 ) ),
			dot( rgb, float3( 0.016391, 0.088013, 0.895596 ) ) );
		o_rgba = float4( EncodePQ( rec2020 ), 1.0 );
	}
}
