/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
* Copyright (C) 2022-2025 Robert Beckebans (id Tech 4x integration)
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

#include "precompiled.h"
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "TonemapPass.h"
#include "TonemapPass_cb.h"

#include <sys/DeviceManager.h>
extern DeviceManager* deviceManager;
idCVar r_bodycamActive( "r_bodycamActive", "0", CVAR_RENDERER | CVAR_BOOL, "Bodycam active for the local player view" );
idCVar r_bodycamVignetteX( "r_bodycamVignetteX", "0", CVAR_RENDERER | CVAR_FLOAT, "Local bodycam lens offset X" );
idCVar r_bodycamVignetteY( "r_bodycamVignetteY", "0", CVAR_RENDERER | CVAR_FLOAT, "Local bodycam lens offset Y" );
idCVar r_grimdark( "r_grimdark", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
	"Grimdark color grade strength; 0 disables", 0.0f, 1.0f );

idCVar r_cameraMotionBlurStrength( "r_cameraMotionBlurStrength", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
	"Camera shutter strength; 0 disables, 1 is a 1/60 second exposure", 0.f, 1.f );
idCVar r_cameraMotionBlurQuality( "r_cameraMotionBlurQuality", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"Camera motion blur quality: 0 low, 1 medium, 2 high, 3 ultra", 0, 3 );

TonemapPass::TonemapPass()
	: isLoaded( false )
	, colorLut( nullptr )
	, colorLutSize( 0 )
	, commonPasses( nullptr )
	, pcEnabledHistogram( false )
	, pcEnabledExposure( false )
	, pcEnabledTonemap( false )
{
}

void TonemapPass::Init( nvrhi::DeviceHandle _device, CommonRenderPasses* _commonPasses, const CreateParameters& _params, nvrhi::IFramebuffer* _sampleFramebuffer )
{
	assert( _params.histogramBins <= 256 );

	device = _device;
	commonPasses = _commonPasses;

	auto histogramShaderInfo = ( _params.isTextureArray ) ? renderProgManager.GetProgramInfo( BUILTIN_HISTOGRAM_TEX_ARRAY_CS ) : renderProgManager.GetProgramInfo( BUILTIN_HISTOGRAM_CS );
	auto exposureShaderInfo = renderProgManager.GetProgramInfo( BUILTIN_EXPOSURE_CS );
	auto tonemapShaderInfo = renderProgManager.GetProgramInfo( BUILTIN_TONEMAPPING );

	histogramShader = histogramShaderInfo.cs;
	exposureShader = exposureShaderInfo.cs;
	tonemapShader = tonemapShaderInfo.ps;
	fullscreenVertexShader = tonemapShaderInfo.vs;

	// Determine if push constants can be used
	size_t pcSize = sizeof( ToneMappingConstants );
	pcEnabledHistogram = histogramShaderInfo.usesPushConstants;
	pcEnabledExposure = exposureShaderInfo.usesPushConstants;
	pcEnabledTonemap = tonemapShaderInfo.usesPushConstants;

	if( !pcEnabledHistogram || !pcEnabledExposure || !pcEnabledTonemap )
	{
		nvrhi::BufferDesc constantBufferDesc;
		constantBufferDesc.byteSize = pcSize;
		constantBufferDesc.debugName = "ToneMappingConstants";
		constantBufferDesc.isConstantBuffer = true;
		constantBufferDesc.isVolatile = true;
		constantBufferDesc.maxVersions = _params.numConstantBufferVersions;
		toneMappingCb = device->createBuffer( constantBufferDesc );
	}

	nvrhi::BufferDesc storageBufferDesc;
	storageBufferDesc.byteSize = sizeof( uint ) * _params.histogramBins;
	storageBufferDesc.format = nvrhi::Format::R32_UINT;
	storageBufferDesc.canHaveUAVs = true;
	storageBufferDesc.debugName = "HistogramBuffer";
	storageBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
	storageBufferDesc.keepInitialState = true;
	storageBufferDesc.canHaveTypedViews = true;
	histogramBuffer = device->createBuffer( storageBufferDesc );

	if( _params.exposureBufferOverride )
	{
		exposureBuffer = _params.exposureBufferOverride;
	}
	else
	{
		storageBufferDesc.byteSize = sizeof( uint );
		storageBufferDesc.format = nvrhi::Format::R32_UINT;
		storageBufferDesc.debugName = "ExposureBuffer";
		exposureBuffer = device->createBuffer( storageBufferDesc );
	}

	colorLut = globalImages->blackImage;

	if( _params.colorLUT )
	{
		int w = _params.colorLUT->GetOpts().width;
		int h = _params.colorLUT->GetOpts().height;

		colorLutSize = h;

		if( w != h * h )
		{
			common->Error( "Color LUT texture size must be: width = (n*n), height = (n)" );
			colorLutSize = 0.f;
		}
		else
		{
			colorLut = _params.colorLUT;
		}
	}

	// histogram pipeline
	{
		nvrhi::BindingLayoutDesc histogramLayout;
		histogramLayout.visibility = nvrhi::ShaderType::Compute;
		if( pcEnabledHistogram )
		{
			histogramLayout.bindings =
			{
				nvrhi::BindingLayoutItem::PushConstants( 0, pcSize ),
				nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_UAV( 0 )
			};
		}
		else
		{
			histogramLayout.bindings =
			{
				nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_UAV( 0 )
			};
		}

		histogramBindingLayout = device->createBindingLayout( histogramLayout );

		nvrhi::ComputePipelineDesc computePipelineDesc;
		computePipelineDesc.CS = histogramShader;
		computePipelineDesc.bindingLayouts = { histogramBindingLayout };
		histogramPipeline = device->createComputePipeline( computePipelineDesc );
	}

	// exposure pipeline
	{
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		if( pcEnabledExposure )
		{
			layoutDesc.bindings =
			{
				nvrhi::BindingLayoutItem::PushConstants( 0, pcSize ),
				nvrhi::BindingLayoutItem::TypedBuffer_SRV( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_UAV( 0 )
			};
		}
		else
		{
			// Use volatile constant buffer for exposure
			layoutDesc.bindings =
			{
				nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_SRV( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_UAV( 0 )
			};
		}

		nvrhi::BindingSetDesc bindingSetDesc;
		if( pcEnabledExposure )
		{
			bindingSetDesc.bindings =
			{
				nvrhi::BindingSetItem::PushConstants( 0, pcSize ),
				nvrhi::BindingSetItem::TypedBuffer_SRV( 0, histogramBuffer ),
				nvrhi::BindingSetItem::TypedBuffer_UAV( 0, exposureBuffer )
			};
		}
		else
		{
			bindingSetDesc.bindings =
			{
				nvrhi::BindingSetItem::ConstantBuffer( 0, toneMappingCb ),
				nvrhi::BindingSetItem::TypedBuffer_SRV( 0, histogramBuffer ),
				nvrhi::BindingSetItem::TypedBuffer_UAV( 0, exposureBuffer )
			};
		}
		exposureBindingLayout = device->createBindingLayout( layoutDesc );
		exposureBindingSet = device->createBindingSet( bindingSetDesc, exposureBindingLayout );

		nvrhi::ComputePipelineDesc computePipelineDesc;
		computePipelineDesc.CS = exposureShader;
		computePipelineDesc.bindingLayouts = { exposureBindingLayout };
		exposurePipeline = device->createComputePipeline( computePipelineDesc );
	}

	{
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Pixel;
		if( pcEnabledTonemap )
		{
			layoutDesc.bindings =
			{
				nvrhi::BindingLayoutItem::PushConstants( 0, pcSize ),
				nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 2 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 3 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 4 ),
				nvrhi::BindingLayoutItem::Sampler( 0 )
			};
		}
		else
		{
			layoutDesc.bindings =
			{
				nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
				nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 2 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 3 ),
				nvrhi::BindingLayoutItem::Texture_SRV( 4 ),
				nvrhi::BindingLayoutItem::Sampler( 0 )
			};
		}
		renderBindingLayout = device->createBindingLayout( layoutDesc );

		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
		pipelineDesc.VS = tonemapShaderInfo.vs;
		pipelineDesc.PS = tonemapShaderInfo.ps;
		pipelineDesc.bindingLayouts = { renderBindingLayout };

		pipelineDesc.renderState.rasterState.setCullNone();
		pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
		pipelineDesc.renderState.depthStencilState.stencilEnable = false;

		renderPipeline = device->createGraphicsPipeline( pipelineDesc, _sampleFramebuffer );
	}

	isLoaded = true;
}

void TonemapPass::Render(
	nvrhi::ICommandList* commandList,
	const ToneMappingParameters& params,
	const viewDef_t* viewDef,
	nvrhi::ITexture* sourceTexture,
	nvrhi::FramebufferHandle _targetFb )
{
	size_t renderHash = std::hash<nvrhi::ITexture*>()( sourceTexture );
	nvrhi::BindingSetHandle renderBindingSet;
	for( int i = renderBindingHash.First( renderHash ); i != -1; i = renderBindingHash.Next( i ) )
	{
		nvrhi::BindingSetHandle bindingSet = renderBindingSets[i];
		if( sourceTexture == bindingSet->getDesc()->bindings[1].resourceHandle )
		{
			renderBindingSet = bindingSet;
			break;
		}
	}

	if( !renderBindingSet )
	{
		nvrhi::BindingSetDesc bindingSetDesc;
		if( pcEnabledTonemap )
		{
			bindingSetDesc.bindings =
			{
				nvrhi::BindingSetItem::PushConstants( 0, sizeof( ToneMappingConstants ) ),
				nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
				nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
				nvrhi::BindingSetItem::Texture_SRV( 2, colorLut->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 3, R_GetMSAASamples() > 1 ? globalImages->blackImage->GetTextureHandle() : globalImages->currentDepthImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 4, R_GetMSAASamples() > 1 ? globalImages->blackImage->GetTextureHandle() : globalImages->currentRenderHDRImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Sampler( 0, commonPasses->m_LinearClampSampler )
			};
		}
		else
		{
			bindingSetDesc.bindings =
			{
				nvrhi::BindingSetItem::ConstantBuffer( 0, toneMappingCb ),
				nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
				nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
				nvrhi::BindingSetItem::Texture_SRV( 2, colorLut->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 3, R_GetMSAASamples() > 1 ? globalImages->blackImage->GetTextureHandle() : globalImages->currentDepthImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 4, R_GetMSAASamples() > 1 ? globalImages->blackImage->GetTextureHandle() : globalImages->currentRenderHDRImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Sampler( 0, commonPasses->m_LinearClampSampler )
			};
		}
		renderBindingSet = device->createBindingSet( bindingSetDesc, renderBindingLayout );
		renderBindingSets.Append( renderBindingSet );
		renderBindingHash.Add( renderHash, renderBindingSets.Num() - 1 );
	}

	{
		nvrhi::GraphicsState state;
		state.pipeline = params.cameraBlurOnly ? cameraBlurPipeline : renderPipeline;
		state.framebuffer = _targetFb;
		state.bindings = { renderBindingSet };
		nvrhi::Viewport viewport{ ( float )viewDef->viewport.x1,
								  ( float )viewDef->viewport.x2 + 1,
								  ( float )viewDef->viewport.y1,
								  ( float )viewDef->viewport.y2 + 1,
								  viewDef->viewport.zmin,
								  viewDef->viewport.zmax };
		state.viewport.addViewportAndScissorRect( viewport );

		bool enableColorLUT = params.enableColorLUT && colorLutSize > 0;

		ToneMappingConstants toneMappingConstants = {};
		if( params.cameraBlurOnly )
		{
			for( int row = 0; row < 4; ++row )
				memcpy( toneMappingConstants.cameraReprojection[row].ToFloatPtr(), params.cameraReprojection[row], sizeof( idVec4 ) );
			toneMappingConstants.cameraBlur = params.cameraBlur;
			toneMappingConstants.cameraViewport = params.cameraViewport;
		}
		toneMappingConstants.bodycam = r_bodycamActive.GetBool() && viewDef->renderView.viewID > 0 && r_renderMode.GetInteger() == RENDERMODE_DOOM ? 1.f : 0.f;
		toneMappingConstants.viewOrigin = idVec2i( viewDef->viewport.x1, viewDef->viewport.y1 );
		toneMappingConstants.viewSize = idVec2i( viewDef->viewport.x2 - viewDef->viewport.x1 + 1, viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
		toneMappingConstants.grimdarkStrength = r_renderMode.GetInteger() == RENDERMODE_DOOM ? r_grimdark.GetFloat() : 0.f;
		toneMappingConstants.grainTime = floorf( viewDef->renderView.time[0] / 33.3333f );
		toneMappingConstants.bodycamVignette.Set( r_bodycamVignetteX.GetFloat(), r_bodycamVignetteY.GetFloat(), 0.f, 0.f );
		toneMappingConstants.hdrPeakRatio = deviceManager->IsHDROutputActive() && r_renderMode.GetInteger() == RENDERMODE_DOOM ? deviceManager->GetHDRPeakNits() / deviceManager->GetHDRPaperWhiteNits() : 0.f;
		toneMappingConstants.exposureScale = ::exp2f( r_exposure.GetFloat() );
		toneMappingConstants.whitePointInvSquared = 1.f / powf( params.whitePoint, 2.f );
		toneMappingConstants.minAdaptedLuminance = r_hdrMinLuminance.GetFloat();
		toneMappingConstants.maxAdaptedLuminance = r_hdrMaxLuminance.GetFloat();
		toneMappingConstants.sourceSlice = 0;
		toneMappingConstants.colorLUTTextureSize = enableColorLUT ? idVec2( colorLutSize * colorLutSize, colorLutSize ) : idVec2( 0.f, 0.f );
		toneMappingConstants.colorLUTTextureSizeInv = enableColorLUT ? 1.f / toneMappingConstants.colorLUTTextureSize : idVec2( 0.f, 0.f );

		if( !pcEnabledTonemap )
		{
			commandList->writeBuffer( toneMappingCb, &toneMappingConstants, sizeof( toneMappingConstants ) );
		}

		commandList->setGraphicsState( state );

		if( pcEnabledTonemap )
		{
			commandList->setPushConstants( &toneMappingConstants, sizeof( toneMappingConstants ) );
		}

		nvrhi::DrawArguments args;
		args.instanceCount = 1;
		args.vertexCount = 4;
		commandList->draw( args );
	}
}

void TonemapPass::SimpleRender( nvrhi::ICommandList* commandList, const ToneMappingParameters& params, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture, nvrhi::FramebufferHandle _fbHandle )
{
	commandList->beginMarker( "ToneMapping" );
	ResetHistogram( commandList );
	AddFrameToHistogram( commandList, viewDef, sourceTexture );
	ComputeExposure( commandList, params );

#if 0 // Shelved: visual quality needs further work. Ignore saved strength settings.
	const int now = viewDef->renderView.time[0];
	const int frame = tr.GetFrameCount();
	const int elapsed = now - previousCameraTime;
	const idVec2i size( viewDef->viewport.x2 - viewDef->viewport.x1 + 1, viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
	const bool eligible = !viewDef->isSubview && !viewDef->targetRender && viewDef->viewEntitys &&
		viewDef->renderView.viewID > 0 && !( viewDef->renderView.rdflags & ( RDF_IRRADIANCE | RDF_NOAMBIENT ) ) && R_GetMSAASamples() <= 1;
	const bool continuous = previousCameraTime >= 0 && elapsed > 0 && elapsed < 100 &&
		frame == previousCameraFrame + 1 && size == previousCameraSize &&
		( viewDef->renderView.vieworg - previousCameraOrigin ).LengthSqr() < 64.f * 64.f &&
		viewDef->renderView.viewaxis[0] * previousCameraAxis[0] > 0.7f &&
		idMath::Fabs( viewDef->renderView.fov_x - previousCameraFov ) < 0.1f;
	if( eligible && continuous && r_cameraMotionBlurStrength.GetFloat() > 0.f &&
		memcmp( &previousCameraMVP, &viewDef->worldSpace.unjitteredMVP, sizeof( idRenderMatrix ) ) != 0 )
	{
		const auto& desc = sourceTexture->getDesc();
		if( !cameraBlurTexture || cameraBlurTexture->getDesc().width != desc.width || cameraBlurTexture->getDesc().height != desc.height )
		{
			// Drop cached references before replacing a resized intermediate.
			renderBindingSets.Clear();
			renderBindingHash.Clear();
			nvrhi::TextureDesc blurDesc;
			blurDesc.width = desc.width;
			blurDesc.height = desc.height;
			blurDesc.format = nvrhi::Format::RGBA16_FLOAT;
			blurDesc.isRenderTarget = true;
			blurDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			blurDesc.keepInitialState = true;
			blurDesc.debugName = "CameraMotionBlurHDR";
			cameraBlurTexture = device->createTexture( blurDesc );
			cameraBlurFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( cameraBlurTexture ) );
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.primType = nvrhi::PrimitiveType::TriangleStrip;
			pipeline.VS = fullscreenVertexShader;
			pipeline.PS = tonemapShader;
			pipeline.bindingLayouts = { renderBindingLayout };
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.depthTestEnable = false;
			pipeline.renderState.depthStencilState.stencilEnable = false;
			cameraBlurPipeline = device->createGraphicsPipeline( pipeline, cameraBlurFramebuffer );
		}
		ToneMappingParameters blurParams = params;
		blurParams.cameraBlurOnly = true;
		idRenderMatrix inverseMVP;
		idRenderMatrix::Inverse( viewDef->worldSpace.unjitteredMVP, inverseMVP );
		idRenderMatrix::Multiply( previousCameraMVP, inverseMVP, blurParams.cameraReprojection );
		const int samples[] = { 8, 12, 24, 40 };
		blurParams.cameraBlur.Set( Min( 2.f, r_cameraMotionBlurStrength.GetFloat() * ( 1000.f / 60.f ) / elapsed ),
			float( samples[idMath::ClampInt( 0, 3, r_cameraMotionBlurQuality.GetInteger() )] ),
			viewDef->unjitteredProjectionRenderMatrix[2][2], viewDef->unjitteredProjectionRenderMatrix[2][3] );
		commandList->beginMarker( "Camera Motion Blur" );
		Render( commandList, blurParams, viewDef, sourceTexture, cameraBlurFramebuffer );
		commandList->endMarker();
		sourceTexture = cameraBlurTexture;
	}
	if( !viewDef->isSubview && !viewDef->targetRender )
	{
		previousCameraTime = eligible ? now : -1;
		previousCameraFrame = frame;
		previousCameraMVP = viewDef->worldSpace.unjitteredMVP;
		previousCameraOrigin = viewDef->renderView.vieworg;
		previousCameraAxis = viewDef->renderView.viewaxis;
		previousCameraSize = size;
		previousCameraFov = viewDef->renderView.fov_x;
	}
#endif
	Render( commandList, params, viewDef, sourceTexture, _fbHandle );
	commandList->endMarker();
}

void TonemapPass::ResetExposure( nvrhi::ICommandList* commandList, float initialExposure )
{
	uint32_t uintValue = *( uint32_t* )&initialExposure;
	commandList->clearBufferUInt( exposureBuffer, uintValue );
}

void TonemapPass::ResetHistogram( nvrhi::ICommandList* commandList )
{
	commandList->clearBufferUInt( histogramBuffer, 0 );
}

void TonemapPass::AddFrameToHistogram( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture )
{
	size_t renderHash = std::hash<nvrhi::ITexture*>()( sourceTexture );
	nvrhi::BindingSetHandle bindingSet;
	for( int i = histogramBindingHash.First( renderHash ); i != -1; i = histogramBindingHash.Next( i ) )
	{
		nvrhi::BindingSetHandle foundSet = histogramBindingSets[i];
		if( sourceTexture == foundSet->getDesc()->bindings[1].resourceHandle )
		{
			bindingSet = foundSet;
			break;
		}
	}

	if( !bindingSet )
	{
		nvrhi::BindingSetDesc bindingSetDesc;
		if( pcEnabledHistogram )
		{
			bindingSetDesc.bindings =
			{
				nvrhi::BindingSetItem::PushConstants( 0, sizeof( ToneMappingConstants ) ),
				nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
				nvrhi::BindingSetItem::TypedBuffer_UAV( 0, histogramBuffer )
			};
		}
		else
		{
			bindingSetDesc.bindings =
			{
				nvrhi::BindingSetItem::ConstantBuffer( 0, toneMappingCb ),
				nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
				nvrhi::BindingSetItem::TypedBuffer_UAV( 0, histogramBuffer )
			};
		}

		bindingSet = device->createBindingSet( bindingSetDesc, histogramBindingLayout );
		histogramBindingSets.Append( bindingSet );
		histogramBindingHash.Add( renderHash, histogramBindingSets.Num() - 1 );
	}

	nvrhi::ViewportState viewportState;
	nvrhi::Viewport viewport{ ( float )viewDef->viewport.x1,
							  ( float )viewDef->viewport.x2,
							  ( float )viewDef->viewport.y1,
							  ( float )viewDef->viewport.y2,
							  viewDef->viewport.zmin,
							  viewDef->viewport.zmax };
	viewportState.addViewportAndScissorRect( viewport );

	for( uint viewportIndex = 0; viewportIndex < viewportState.scissorRects.size(); viewportIndex++ )
	{
		ToneMappingConstants toneMappingConstants = {};
		toneMappingConstants.logLuminanceScale = 1.0f / ( r_hdrMinLuminance.GetFloat() - r_hdrMaxLuminance.GetFloat() );
		toneMappingConstants.logLuminanceBias = -r_hdrMinLuminance.GetFloat() * toneMappingConstants.logLuminanceScale;

		nvrhi::Rect& scissor = viewportState.scissorRects[viewportIndex];
		toneMappingConstants.viewOrigin = idVec2i( scissor.minX, scissor.minY );
		toneMappingConstants.viewSize = idVec2i( scissor.maxX - scissor.minX, scissor.maxY - scissor.minY );
		toneMappingConstants.sourceSlice = 0;

		if( !pcEnabledHistogram )
		{
			commandList->writeBuffer( toneMappingCb, &toneMappingConstants, sizeof( toneMappingConstants ) );
		}

		nvrhi::ComputeState state;
		state.pipeline = histogramPipeline;
		state.bindings = { bindingSet };
		commandList->setComputeState( state );

		if( pcEnabledHistogram )
		{
			commandList->setPushConstants( &toneMappingConstants, sizeof( toneMappingConstants ) );
		}

		idVec2i numGroups = ( toneMappingConstants.viewSize + idVec2i( 15, 15 ) ) / idVec2i( 16, 16 );
		commandList->dispatch( numGroups.x, numGroups.y, 1 );
	}
}

static const float maxLuminance = 4.0f;
static const float minLuminance = -10.0f;

void TonemapPass::ComputeExposure( nvrhi::ICommandList* commandList, const ToneMappingParameters& params )
{
	ToneMappingConstants toneMappingConstants = {};
	toneMappingConstants.logLuminanceScale = maxLuminance - minLuminance;
	toneMappingConstants.logLuminanceBias = minLuminance;
	toneMappingConstants.histogramLowPercentile = Min( 0.99f, Max( 0.f, params.histogramLowPercentile ) );
	toneMappingConstants.histogramHighPercentile = Min( 1.f, Max( toneMappingConstants.histogramLowPercentile, params.histogramHighPercentile ) );
	toneMappingConstants.eyeAdaptationSpeedUp = r_hdrAdaptionRate.GetFloat();
	toneMappingConstants.eyeAdaptationSpeedDown = r_hdrAdaptionRate.GetFloat() / 2.f;
	toneMappingConstants.minAdaptedLuminance = r_hdrMinLuminance.GetFloat();
	toneMappingConstants.maxAdaptedLuminance = r_hdrMaxLuminance.GetFloat();
	toneMappingConstants.frameTime = Sys_Milliseconds() / 1000.0f;

	if( !pcEnabledExposure )
	{
		commandList->writeBuffer( toneMappingCb, &toneMappingConstants, sizeof( toneMappingConstants ) );
	}

	nvrhi::ComputeState state;
	state.pipeline = exposurePipeline;
	state.bindings = { exposureBindingSet };
	commandList->setComputeState( state );

	if( pcEnabledExposure )
	{
		commandList->setPushConstants( &toneMappingConstants, sizeof( toneMappingConstants ) );
	}

	commandList->dispatch( 1 );
}
