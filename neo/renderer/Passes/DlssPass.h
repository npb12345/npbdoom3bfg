// Native DLSS integration for NPBDoom3BFG. GPL-3.0-or-later.
#pragma once

#include <nvrhi/nvrhi.h>

struct viewDef_t;
class idCVar;
extern idCVar r_dlssFrameGeneration;

// Optional at build time and runtime. Non-DX12 builds use the same TAA fallback.
bool R_DLSSRequested();
bool R_DLSSAvailable();
const char* R_DLSSStatus();
void R_DLSSInit();
void R_DLSSSetDevice( void* device );
void R_DLSSUpgradeSwapChain( void** swapChain );
void R_DLSSUpgradeFactory( void** factory );
bool R_DLSSFrameGenerationAvailable();
bool R_DLSSFrameGenerationLoaded();
void R_DLSSSimulationStart( bool allowFrameGeneration );
void R_DLSSSimulationEnd();
void R_DLSSRenderStart( int frameIndex );
void R_DLSSRenderEnd();
void R_DLSSCaptureHudless( nvrhi::IDevice* device, nvrhi::ICommandList* commands, nvrhi::ITexture* backBuffer );
void R_DLSSBeforePresent( unsigned width, unsigned height );
void R_DLSSAfterPresent();
void R_DLSSSuspendFrameGeneration();
void R_DLSSShutdown();
bool R_DLSSRenderSize( int outputWidth, int outputHeight, int& width, int& height );
bool R_DLSSEvaluate( nvrhi::ICommandList* commands, const viewDef_t* view,
    nvrhi::ITexture* color, nvrhi::ITexture* depth, nvrhi::ITexture* motion,
    nvrhi::ITexture* output, const idVec2& jitter, bool historyValid );
