// Native DLSS integration for NPBDoom3BFG. GPL-3.0-or-later.
#include "precompiled.h"
#pragma hdrstop
#include "../RenderCommon.h"
#include "DlssPass.h"

bool R_DLSSRequested()
{
    return r_renderMode.GetInteger() == RENDERMODE_DOOM &&
        r_antiAliasing.GetInteger() >= ANTI_ALIASING_DLAA &&
        r_antiAliasing.GetInteger() <= ANTI_ALIASING_DLSS_PERFORMANCE;
}

#if defined( USE_STREAMLINE ) && defined( USE_DX12 )
#include <d3d12.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_security.h>
#include <mutex>

namespace
{
    HMODULE library = nullptr;
    bool initialized = false;
    bool available = false;
    const char* statusText = "DLSS runtime not loaded";
    std::mutex apiMutex;
    std::wstring pluginDirectory;
    sl::ViewportHandle viewport( 0 );
    idRenderMatrix previousMVP = renderMatrix_identity;
    idVec3 previousOrigin = vec3_origin;
    int previousFrame = -1;
    int previousTime = -1;
    int previousMode = -1;
    int previousWidth = 0, previousHeight = 0;
    int cachedMode = -1, cachedOutputWidth = 0, cachedOutputHeight = 0;
    int cachedWidth = 0, cachedHeight = 0;

#define DLSS_API( name ) decltype( &name ) p_##name = nullptr
    DLSS_API( slInit );
    DLSS_API( slShutdown );
    DLSS_API( slSetD3DDevice );
    DLSS_API( slUpgradeInterface );
    DLSS_API( slIsFeatureSupported );
    DLSS_API( slGetFeatureFunction );
    DLSS_API( slGetNewFrameToken );
    DLSS_API( slSetConstants );
    DLSS_API( slSetTagForFrame );
    DLSS_API( slEvaluateFeature );
#undef DLSS_API
    PFun_slDLSSGetOptimalSettings* getOptimalSettings = nullptr;
    PFun_slDLSSSetOptions* setOptions = nullptr;

    bool Check( sl::Result result, const char* operation )
    {
        if( result == sl::Result::eOk ) return true;
        common->Warning( "DLSS: %s failed (%d); using TAA", operation, int( result ) );
        available = false;
        statusText = "DLSS unavailable; using TAA (see console)";
        return false;
    }

    sl::DLSSOptions Options( int width, int height )
    {
        sl::DLSSOptions options;
        switch( r_antiAliasing.GetInteger() )
        {
            case ANTI_ALIASING_DLSS_QUALITY: options.mode = sl::DLSSMode::eMaxQuality; break;
            case ANTI_ALIASING_DLSS_BALANCED: options.mode = sl::DLSSMode::eBalanced; break;
            case ANTI_ALIASING_DLSS_PERFORMANCE: options.mode = sl::DLSSMode::eMaxPerformance; break;
            default: options.mode = sl::DLSSMode::eDLAA; break;
        }
        options.outputWidth = width;
        options.outputHeight = height;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.useAutoExposure = sl::Boolean::eTrue;
        // Default presets permit NVIDIA's supported model updates and user overrides.
        return options;
    }

    sl::float4x4 Matrix( const idRenderMatrix& matrix )
    {
        // idRenderMatrix uses column vectors; Streamline uses row vectors.
        sl::float4x4 result;
        for( int row = 0; row < 4; ++row )
            result[row] = { matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row] };
        return result;
    }
}

bool R_DLSSAvailable() { std::lock_guard<std::mutex> lock( apiMutex ); return available; }
const char* R_DLSSStatus() { std::lock_guard<std::mutex> lock( apiMutex ); return statusText; }

void R_DLSSInit()
{
    if( library ) return;
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW( nullptr, executable, MAX_PATH );
    pluginDirectory = executable;
    pluginDirectory.resize( pluginDirectory.find_last_of( L"\\/" ) );
    const std::wstring path = pluginDirectory + L"\\sl.interposer.dll";
    if( GetFileAttributesW( path.c_str() ) == INVALID_FILE_ATTRIBUTES ) return;
    if( !sl::security::verifyEmbeddedSignature( path.c_str() ) )
    {
        statusText = "Streamline signature verification failed";
        common->Warning( "%s", statusText );
        return;
    }
    library = LoadLibraryExW( path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );
    if( !library ) return;
#define LOAD_DLSS_API( name ) \
    p_##name = reinterpret_cast<decltype( p_##name )>( GetProcAddress( library, #name ) ); \
    if( !p_##name ) { statusText = "Incompatible Streamline runtime"; return; }
    LOAD_DLSS_API( slInit );
    LOAD_DLSS_API( slShutdown );
    LOAD_DLSS_API( slSetD3DDevice );
    LOAD_DLSS_API( slUpgradeInterface );
    LOAD_DLSS_API( slIsFeatureSupported );
    LOAD_DLSS_API( slGetFeatureFunction );
    LOAD_DLSS_API( slGetNewFrameToken );
    LOAD_DLSS_API( slSetConstants );
    LOAD_DLSS_API( slSetTagForFrame );
    LOAD_DLSS_API( slEvaluateFeature );
#undef LOAD_DLSS_API
    const sl::Feature features[] = { sl::kFeatureDLSS };
    const wchar_t* paths[] = { pluginDirectory.c_str() };
    sl::Preferences preferences;
    preferences.flags = sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = pluginDirectory.c_str();
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "NPBDoom3BFG-1";
    preferences.projectId = "8f9473e2-c271-4c7a-90de-c581fb0a23b6";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    initialized = Check( p_slInit( preferences, sl::kSDKVersion ), "initialization" );
}

void R_DLSSSetDevice( void* nativeDevice )
{
    if( !initialized ) return;
    auto* device = static_cast<ID3D12Device*>( nativeDevice );
    LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter;
    adapter.deviceLUID = reinterpret_cast<uint8_t*>( &luid );
    adapter.deviceLUIDSizeInBytes = sizeof( luid );
    if( !Check( p_slIsFeatureSupported( sl::kFeatureDLSS, adapter ), "hardware/driver check" ) ||
        !Check( p_slSetD3DDevice( device ), "device setup" ) ) return;
    if( !Check( p_slGetFeatureFunction( sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>( getOptimalSettings ) ), "settings API" ) ||
        !Check( p_slGetFeatureFunction( sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>( setOptions ) ), "options API" ) ) return;
    available = true;
    statusText = "Native DLSS available (DX12)";
    common->Printf( "%s\n", statusText );
}

void R_DLSSUpgradeSwapChain( void** swapChain )
{
    if( initialized ) Check( p_slUpgradeInterface( swapChain ), "swap chain setup" );
}

void R_DLSSShutdown()
{
    if( initialized ) p_slShutdown();
    initialized = available = false;
    getOptimalSettings = nullptr;
    setOptions = nullptr;
    previousFrame = previousTime = previousMode = cachedMode = -1;
    cachedWidth = cachedHeight = 0;
    statusText = "DLSS runtime not loaded";
    if( library ) { FreeLibrary( library ); library = nullptr; }
}

bool R_DLSSRenderSize( int outputWidth, int outputHeight, int& width, int& height )
{
    std::lock_guard<std::mutex> lock( apiMutex );
    if( !available || !R_DLSSRequested() || outputWidth <= 0 || outputHeight <= 0 ) return false;
    if( cachedMode != r_antiAliasing.GetInteger() || cachedOutputWidth != outputWidth || cachedOutputHeight != outputHeight )
    {
        sl::DLSSOptimalSettings settings;
        auto options = Options( outputWidth, outputHeight );
        if( !Check( getOptimalSettings( options, settings ), "optimal render size" ) ) return false;
        cachedMode = r_antiAliasing.GetInteger();
        cachedOutputWidth = outputWidth;
        cachedOutputHeight = outputHeight;
        cachedWidth = settings.optimalRenderWidth;
        cachedHeight = settings.optimalRenderHeight;
        if( options.mode == sl::DLSSMode::eDLAA ) { cachedWidth = outputWidth; cachedHeight = outputHeight; }
        if( cachedWidth <= 0 || cachedHeight <= 0 || cachedWidth > outputWidth || cachedHeight > outputHeight )
        {
            available = false;
            statusText = "DLSS returned an invalid render size; using TAA";
            return false;
        }
        common->Printf( "DLSS: render %dx%d -> output %dx%d\n", cachedWidth, cachedHeight, outputWidth, outputHeight );
    }
    width = cachedWidth;
    height = cachedHeight;
    return true;
}

bool R_DLSSEvaluate( nvrhi::ICommandList* commands, const viewDef_t* view,
    nvrhi::ITexture* color, nvrhi::ITexture* depth, nvrhi::ITexture* motion,
    nvrhi::ITexture* output, const idVec2& jitter, bool historyValid )
{
    std::lock_guard<std::mutex> lock( apiMutex );
    if( !available || !R_DLSSRequested() ) return false;
    const int width = view->viewport.x2 - view->viewport.x1 + 1;
    const int height = view->viewport.y2 - view->viewport.y1 + 1;
    auto options = Options( output->getDesc().width, output->getDesc().height );
    if( !Check( setOptions( viewport, options ), "setting mode" ) ) return false;
    sl::FrameToken* token = nullptr;
    if( !Check( p_slGetNewFrameToken( token, nullptr ), "frame token" ) ) return false;
    bool reset = !historyValid || previousFrame + 1 != view->taaFrameCount ||
        previousMode != r_antiAliasing.GetInteger() || previousWidth != width || previousHeight != height ||
        view->renderView.time[0] < previousTime || view->renderView.time[0] - previousTime > 250 ||
        ( view->renderView.vieworg - previousOrigin ).LengthSqr() > 256.0f * 256.0f;
    idRenderMatrix inverse, reprojection, inverseReprojection, projection, inverseProjection;
    idRenderMatrix::Inverse( view->worldSpace.unjitteredMVP, inverse );
    idRenderMatrix::Multiply( reset ? view->worldSpace.unjitteredMVP : previousMVP, inverse, reprojection );
    idRenderMatrix::Inverse( reprojection, inverseReprojection );
    for( int row = 0; row < 4; ++row )
        for( int col = 0; col < 4; ++col ) projection[row][col] = view->unjitteredProjectionMatrix[col * 4 + row];
    idRenderMatrix::Inverse( projection, inverseProjection );
    sl::Constants constants;
    constants.cameraViewToClip = Matrix( projection );
    constants.clipToCameraView = Matrix( inverseProjection );
    constants.clipToLensClip = Matrix( renderMatrix_identity );
    constants.clipToPrevClip = Matrix( reprojection );
    constants.prevClipToClip = Matrix( inverseReprojection );
    constants.jitterOffset = { jitter.x, -jitter.y };
    constants.mvecScale = { 1.0f / width, 1.0f / height };
    constants.cameraPinholeOffset = { 0, 0 };
    const auto& camera = view->renderView;
    constants.cameraPos = { camera.vieworg.x, camera.vieworg.y, camera.vieworg.z };
    constants.cameraFwd = { camera.viewaxis[0].x, camera.viewaxis[0].y, camera.viewaxis[0].z };
    constants.cameraRight = { -camera.viewaxis[1].x, -camera.viewaxis[1].y, -camera.viewaxis[1].z };
    constants.cameraUp = { camera.viewaxis[2].x, camera.viewaxis[2].y, camera.viewaxis[2].z };
    constants.cameraNear = r_znear.GetFloat() * ( camera.cramZNear ? 0.25f : 1.0f );
    constants.cameraFar = 100000.0f;
    constants.cameraFOV = DEG2RAD( camera.fov_y );
    constants.cameraAspectRatio = float( width ) / height;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if( !Check( p_slSetConstants( constants, *token, viewport ), "camera constants" ) ) return false;

    // Establish known states before crossing from NVRHI to native DX12. Streamline
    // restores tagged resource states; clearState restores descriptor/pipeline tracking.
    commands->setTextureState( color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
    commands->setTextureState( depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
    commands->setTextureState( motion, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
    commands->setTextureState( output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess );
    commands->commitBarriers();
    const uint32_t srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sl::Resource inputColor( sl::ResourceType::eTex2d, color->getNativeObject( nvrhi::ObjectTypes::D3D12_Resource ), srv );
    sl::Resource inputDepth( sl::ResourceType::eTex2d, depth->getNativeObject( nvrhi::ObjectTypes::D3D12_Resource ), srv );
    sl::Resource inputMotion( sl::ResourceType::eTex2d, motion->getNativeObject( nvrhi::ObjectTypes::D3D12_Resource ), srv );
    sl::Resource outputColor( sl::ResourceType::eTex2d, output->getNativeObject( nvrhi::ObjectTypes::D3D12_Resource ), D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
    sl::Extent inputExtent{ uint32_t( view->viewport.y1 ), uint32_t( view->viewport.x1 ), uint32_t( width ), uint32_t( height ) };
    sl::Extent outputExtent{ 0, 0, options.outputWidth, options.outputHeight };
    sl::ResourceTag tags[] = {
        { &inputColor, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
        { &inputDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
        { &inputMotion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
        { &outputColor, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent }
    };
    auto* nativeCommands = static_cast<ID3D12GraphicsCommandList*>( commands->getNativeObject( nvrhi::ObjectTypes::D3D12_GraphicsCommandList ) );
    bool success = Check( p_slSetTagForFrame( *token, viewport, tags, 4, nativeCommands ), "resource tagging" );
    if( success )
    {
        const sl::BaseStructure* inputs[] = { &viewport };
        success = Check( p_slEvaluateFeature( sl::kFeatureDLSS, *token, inputs, 1, nativeCommands ), "evaluation" );
    }
    commands->clearState();
    if( success && ( previousMode != r_antiAliasing.GetInteger() || previousWidth != width || previousHeight != height ) )
    {
        common->Printf( "DLSS: native evaluation succeeded, mode %d, %dx%d -> %ux%u\n",
            r_antiAliasing.GetInteger(), width, height, options.outputWidth, options.outputHeight );
    }
    previousMVP = view->worldSpace.unjitteredMVP;
    previousOrigin = camera.vieworg;
    previousTime = camera.time[0];
    previousFrame = view->taaFrameCount;
    previousMode = r_antiAliasing.GetInteger();
    previousWidth = width;
    previousHeight = height;
    return success;
}
#else
bool R_DLSSAvailable() { return false; }
const char* R_DLSSStatus() { return "Native DLSS requires a Windows DX12 build with Streamline"; }
void R_DLSSInit() {}
void R_DLSSSetDevice( void* ) {}
void R_DLSSUpgradeSwapChain( void** ) {}
void R_DLSSShutdown() {}
bool R_DLSSRenderSize( int, int, int&, int& ) { return false; }
bool R_DLSSEvaluate( nvrhi::ICommandList*, const viewDef_t*, nvrhi::ITexture*, nvrhi::ITexture*,
    nvrhi::ITexture*, nvrhi::ITexture*, const idVec2&, bool ) { return false; }
#endif
