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

#pragma pack_matrix(row_major)

#include "vulkan.hlsli"
#include "tonemapping_cb.h"

// *INDENT-OFF*
#if SOURCE_ARRAY
Texture2DArray t_Source : register(t0);
#else
Texture2D t_Source : register(t0);
#endif
Buffer<uint> t_Exposure : register(t1);

Texture2D t_ColorLUT : register(t2);
Texture2D<float> t_CameraDepth : register(t3);
Texture2D<float4> t_CameraMask : register(t4);
SamplerState s_ColorLUTSampler : register(s0);

#if USE_PUSH_CONSTANTS
VK_PUSH_CONSTANT ConstantBuffer<ToneMappingConstants> g_ToneMapping : register( b0 );
#else
cbuffer c_ToneMapping : register( b0 )
{
    ToneMappingConstants g_ToneMapping;
};
#endif
// *INDENT-ON*

float3 ACESFilm( float3 x )
{
	float a = 2.51;
	float b = 0.03;
	float c = 2.43;
	float d = 0.59;
	float e = 0.14;
	return saturate( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ) );
}

float Luminance( float3 color )
{
	return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
}

float3 ConvertToLDR( float3 color )
{
	float srcLuminance = Luminance( color );

	if( srcLuminance <= 0 )
	{
		return 0;
	}

	float adaptedLuminance = asfloat( t_Exposure[0] );
	if( adaptedLuminance <= 0 )
	{
		adaptedLuminance = g_ToneMapping.minAdaptedLuminance;
	}

	float scaledLuminance = g_ToneMapping.exposureScale * srcLuminance / adaptedLuminance;
	float mappedLuminance = ( scaledLuminance * ( 1 + scaledLuminance * g_ToneMapping.whitePointInvSquared ) ) / ( 1 + scaledLuminance );

	return color * ( mappedLuminance / srcLuminance );
}

float3 ApplyColorLUT( float3 color )
{
	const float size = g_ToneMapping.colorLUTTextureSize.y;

	color = saturate( color );

	float r = color.r * ( size - 1 ) + 0.5;
	float g = color.g * ( size - 1 ) + 0.5;

	float b = color.b * ( size - 1 );

	float2 uv1 = float2( ( floor( b ) + 0 ) * size + r, g ) * g_ToneMapping.colorLUTTextureSizeInv.xy;
	float2 uv2 = float2( ( floor( b ) + 1 ) * size + r, g ) * g_ToneMapping.colorLUTTextureSizeInv.xy;

	float3 c1 = t_ColorLUT.SampleLevel( s_ColorLUTSampler, uv1, 0 ).rgb;
	float3 c2 = t_ColorLUT.SampleLevel( s_ColorLUTSampler, uv2, 0 ).rgb;

	return lerp( c1, c2, frac( b ) );
}


float4 BodycamSample( float2 lensUV )
{
	uint width, height;
#if SOURCE_ARRAY
	uint layers;
	t_Source.GetDimensions( width, height, layers );
#else
	t_Source.GetDimensions( width, height );
#endif
	float2 pixel = g_ToneMapping.viewOrigin + clamp( lensUV * g_ToneMapping.viewSize,
		0.5, float2( g_ToneMapping.viewSize ) - 0.5 );
	float2 sampleUV = pixel / float2( width, height );
#if SOURCE_ARRAY
	return t_Source.SampleLevel( s_ColorLUTSampler, float3( sampleUV, g_ToneMapping.sourceSlice ), 0 );
#else
	return t_Source.SampleLevel( s_ColorLUTSampler, sampleUV, 0 );
#endif
}

int2 CameraPixel( float2 uv )
{
	return int2( g_ToneMapping.cameraViewport.xy + clamp( uv * g_ToneMapping.cameraViewport.zw,
		0.0, g_ToneMapping.cameraViewport.zw - 1.0 ) );
}

float CameraMask( float2 uv )
{
	int2 p = CameraPixel( uv );
	int2 lo = int2( g_ToneMapping.cameraViewport.xy );
	int2 hi = lo + int2( g_ToneMapping.cameraViewport.zw ) - 1;
	// Expand the exclusion slightly to protect reconstructed weapon edges.
	float mask = t_CameraMask.Load( int3( p, 0 ) ).a;
	mask = min( mask, t_CameraMask.Load( int3( clamp( p + int2( 1, 0 ), lo, hi ), 0 ) ).a );
	mask = min( mask, t_CameraMask.Load( int3( clamp( p - int2( 1, 0 ), lo, hi ), 0 ) ).a );
	mask = min( mask, t_CameraMask.Load( int3( clamp( p + int2( 0, 1 ), lo, hi ), 0 ) ).a );
	return min( mask, t_CameraMask.Load( int3( clamp( p - int2( 0, 1 ), lo, hi ), 0 ) ).a );
}

float CameraViewDepth( float depth )
{
	return abs( g_ToneMapping.cameraBlur.w ) / max( abs( depth + g_ToneMapping.cameraBlur.z ), 0.000001 );
}

float4 CameraBlur( float2 uv )
{
	float4 center = BodycamSample( uv );
	if( CameraMask( uv ) < 0.99 ) return center;
	float depth = t_CameraDepth.Load( int3( CameraPixel( uv ), 0 ) );
	float4 clip = float4( uv * float2( 2, -2 ) + float2( -1, 1 ), depth, 1 );
	float4 previous = float4( dot( g_ToneMapping.cameraReprojection[0], clip ),
		dot( g_ToneMapping.cameraReprojection[1], clip ), dot( g_ToneMapping.cameraReprojection[2], clip ),
		dot( g_ToneMapping.cameraReprojection[3], clip ) );
	if( previous.w <= 0.00001 ) return center;
	float2 previousUV = previous.xy / previous.w * float2( 0.5, -0.5 ) + 0.5;
	float2 velocity = ( previousUV - uv ) * g_ToneMapping.cameraBlur.x;
	float pixels = length( velocity * g_ToneMapping.viewSize );
	if( pixels < 0.5 ) return center;
	float maxPixels = min( 80.0, g_ToneMapping.viewSize.y * 0.04 );
	velocity *= min( 1.0, maxPixels / pixels );
	pixels = min( pixels, maxPixels );
	int samples = min( int( g_ToneMapping.cameraBlur.y ), max( 4, int( ceil( pixels ) ) ) );
	float centerZ = CameraViewDepth( depth );
	float3 sum = center.rgb;
	float weightSum = 1.0;
	[loop] for( int i = 0; i < samples; ++i )
	{
		float t = ( i + 0.5 ) / samples - 0.5;
		float2 sampleUV = uv + velocity * t;
		if( any( sampleUV < 0 ) || any( sampleUV > 1 ) || CameraMask( sampleUV ) < 0.99 ) continue;
		float sampleZ = CameraViewDepth( t_CameraDepth.Load( int3( CameraPixel( sampleUV ), 0 ) ) );
		// Bilateral weights avoid dragging nearby walls and weapons over the background.
		float weight = exp2( -abs( sampleZ - centerZ ) / max( centerZ * 0.05, 0.5 ) );
		weight *= 1.0 - abs( t );
		sum += BodycamSample( sampleUV ).rgb * weight;
		weightSum += weight;
	}
	return float4( sum / weightSum, center.a );
}

float3 BodycamDisplayColor( float3 color )
{
 color = ConvertToLDR( color );
if( g_ToneMapping.hdrPeakRatio > 0.0 )
	{
		// Keep the SDR color balance. Extending each channel independently
		// exaggerates warm lights: red can enter the HDR shoulder while green
		// and blue are still below paper white. Instead extend luminance and
		// apply one gain to all channels, including the peak-luminance limit.
		float3 sceneColor = max( color, 0.0 );
		float3 baseColor = g_ToneMapping.colorLUTTextureSize.x > 0.0
			? ApplyColorLUT( sceneColor ) : ACESFilm( sceneColor );
		float baseLuminance = Luminance( baseColor );
		// Anchor middle gray while deepening the toe and separating bright
		// whites from the midtones. Zero stays zero; there is no black cutoff.
		float contrastLuminance = 0.18 * pow( max( baseLuminance, 0.0 ) / 0.18, 1.18 );
		contrastLuminance = min( contrastLuminance, g_ToneMapping.hdrPeakRatio );
		// Enter the HDR shoulder earlier so ordinary bright lights use the
		// display's range, instead of reserving it for extreme scene values.
		float headroom = max( g_ToneMapping.hdrPeakRatio - contrastLuminance, 0.0 );
		float highlightLuminance = max( Luminance( sceneColor ) - 0.6, 0.0 );
		float extraLuminance = headroom * ( 1.0 - exp( -2.0 * highlightLuminance / max( headroom, 0.001 ) ) );
		float gain = ( contrastLuminance + extraLuminance ) / max( baseLuminance, 0.000001 );
		float maxChannel = max( baseColor.r, max( baseColor.g, baseColor.b ) );
		gain = min( gain, g_ToneMapping.hdrPeakRatio / max( maxChannel, 0.000001 ) );
		color = baseColor * gain;
	}
	else if( g_ToneMapping.colorLUTTextureSize.x > 0 )
	{
		color = ApplyColorLUT( color );
	}
	else
	{
		// Tonemapping curve is applied after exposure.
		color = ACESFilm( color );
	}

	return color;
}
float2 BodycamLensUV( float2 uv )
{
	float2 centered = uv - 0.5;
	float gradient = pow( saturate( 1.0 - length( centered ) / 0.75 ), 0.5 );
	// Limit inward displacement: full strength collapses the center's scale
	// toward zero and creates extreme magnification instead of a mild lens.
	return uv - centered * gradient * 0.26;
}
float4 BodycamSensorSample( float2 uv )
{
	float2 lensUV = BodycamLensUV( uv );
	float2 edge = uv * 2.0 - 1.0;
	// Small radial channel separation and horizontal color bleed at the edge.
	float2 fringe = edge * dot( edge, edge ) * 0.00035;
	float4 color = BodycamSample( lensUV );
	color.r = BodycamSample( lensUV + fringe + float2( 0.00025, 0 ) ).r;
	color.b = BodycamSample( lensUV - fringe - float2( 0.00025, 0 ) ).b;
	return color;
}
float3 BodycamDisplaySample( float2 uv )
{
	return BodycamDisplayColor( BodycamSensorSample( uv ).rgb );
}
void main(
	in float4 pos : SV_Position,
	in float2 uv : UV,
	out float4 o_rgba : SV_Target )
{
	if( g_ToneMapping.cameraBlur.y > 0 )
	{
		o_rgba = CameraBlur( ( pos.xy - g_ToneMapping.viewOrigin ) / g_ToneMapping.viewSize );
		return;
	}
#if SOURCE_ARRAY
	float4 HdrColor = t_Source[uint3( pos.xy, g_ToneMapping.sourceSlice )];
#else
	float4 HdrColor = t_Source[pos.xy];
#endif
	if( g_ToneMapping.bodycam > 0.0 )
	{
		HdrColor = BodycamSensorSample( uv );
	}

	o_rgba.a = HdrColor.a;

	o_rgba.rgb = BodycamDisplayColor( HdrColor.rgb );
	// Grade scene color before display encoding, leaving HDR headroom intact.
	// No lens distortion, channel offsets, or chromatic aberration.
	float strength = saturate( g_ToneMapping.grimdarkStrength );
	if( strength > 0.0 )
	{
		float3 original = max( o_rgba.rgb, 0.0 );
		float luminance = Luminance( original );
		float redAccent = saturate( ( original.r - max( original.g, original.b ) ) / max( luminance, 0.001 ) );
		float brightAccent = smoothstep( 0.5, 1.5, luminance );
		float saturation = lerp( 0.32, 0.88, max( redAccent, brightAccent ) );
		if( g_ToneMapping.bodycam > 0.0 ) saturation = lerp( saturation, 1.0, 0.5 );
		float3 graded = lerp( luminance.xxx, original, saturation );
		float shadow = 1.0 - smoothstep( 0.03, 0.45, luminance );
		graded *= lerp( float3( 1.025, 0.97, 0.92 ), float3( 0.68, 0.82, 1.0 ), shadow );
		// A gentle toe, without clipping blacks or flattening HDR highlights.
		graded *= lerp( 0.42, 1.02, smoothstep( 0.0, 1.0, luminance ) );
		float2 edge = uv * 2.0 - 1.0;
		float vignette = smoothstep( 0.25, 1.8, dot( edge, edge ) );
		graded *= 1.0 - 0.34 * vignette;
		float grain = frac( sin( dot( floor( pos.xy ), float2( 12.9898, 78.233 ) ) +
			g_ToneMapping.grainTime * 37.719 ) * 43758.5453 ) - 0.5;
		graded *= 1.0 + grain * 0.24 * ( 1.0 - brightAccent );
		float peak = g_ToneMapping.hdrPeakRatio > 0.0 ? g_ToneMapping.hdrPeakRatio : 1.0;
		float maxChannel = max( graded.r, max( graded.g, graded.b ) );
		graded *= min( 1.0, peak / max( maxChannel, 0.000001 ) );
		o_rgba.rgb = lerp( original, graded, strength );
	}

	if( g_ToneMapping.bodycam > 0.0 )
	{
		float3 cameraColor = max( o_rgba.rgb, 0.0 );
		float3 center = BodycamDisplaySample( uv );
		float3 average = ( BodycamDisplaySample( uv + float2( 0.001, 0 ) ) +
			BodycamDisplaySample( uv - float2( 0.001, 0 ) ) +
			BodycamDisplaySample( uv + float2( 0, 0.001 ) ) +
			BodycamDisplaySample( uv - float2( 0, 0.001 ) ) ) * 0.25;
		// Gentler detail enhancement, with a little lens softness in the color.
		cameraColor = max( cameraColor + ( average - center ) * 0.15 +
			dot( center - average, float3( 0.3, 0.59, 0.11 ) ) * 2.0, 0.0 );
		// Restrained video texture: fine sensor noise, faint scanlines and
		// low-level line-to-line exposure variation. Keep black at zero.
		float frame = g_ToneMapping.grainTime;
		float noise = frac( sin( dot( floor( pos.xy ), float2( 12.9898, 78.233 ) ) + frame * 17.13 ) * 43758.5453 ) - 0.5;
		float lineNoise = frac( sin( floor( uv.y * 720.0 ) * 19.17 + frame * 0.73 ) * 13758.5453 ) - 0.5;
		float scanline = 0.5 + 0.5 * sin( uv.y * 720.0 * 6.2831853 );
		cameraColor *= ( 1.0 + noise * 0.06 + lineNoise * 0.025 ) * ( 1.0 - scanline * 0.025 );
		// Use the viewport aspect so the lens border stays circular on ultrawide.
		float aspect = float( g_ToneMapping.viewSize.x ) / max( float( g_ToneMapping.viewSize.y ), 1.0 );
		float d = length( ( uv - 0.5 ) * float2( aspect, 1.0 ) - g_ToneMapping.bodycamVignette.xy );
		float lensMask = 1.0 - smoothstep( 0.76, 0.82, d );
		// A black lens housing, without the reference material's gray fill.
		cameraColor *= lensMask;
		float peak = g_ToneMapping.hdrPeakRatio > 0.0 ? g_ToneMapping.hdrPeakRatio : 1.0;
		cameraColor *= min( 1.0, peak / max( max( cameraColor.r, max( cameraColor.g, cameraColor.b ) ), 0.000001 ) );
		o_rgba.rgb = cameraColor;
	}

	// Gamma correction since we are not rendering to an sRGB render target.
	const float hdrGamma = 2.2;
	float gamma = 1.0 / hdrGamma;
	o_rgba.r = pow( o_rgba.r, gamma );
	o_rgba.g = pow( o_rgba.g, gamma );
	o_rgba.b = pow( o_rgba.b, gamma );
}
