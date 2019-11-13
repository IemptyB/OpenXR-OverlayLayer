#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <set>

#include <cassert>
#include <cstring>
#include <cstdio>

#define XR_USE_GRAPHICS_API_D3D11 1

#include "xr_overlay_dll.h"
#include "xr_generated_dispatch_table.h"
#include "xr_linear.h"

#include <d3d11_4.h>

static std::string fmt(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int size = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if(size >= 0) {
        int provided = size + 1;
        std::unique_ptr<char[]> buf(new char[provided]);

        va_start(args, fmt);
        int size = vsnprintf(buf.get(), provided, fmt, args);
        va_end(args);

        return std::string(buf.get());
    }
    return "(fmt() failed, vsnprintf returned -1)";
}

static void CheckResultWithLastError(bool success, const char* what, const char *file, int line)
{
    if(!success) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        std::string str = fmt("%s at %s:%d failed with %d (%s)\n", what, file, line, lastError, messageBuf);
        OutputDebugStringA(str.data());
        DebugBreak();
        LocalFree(messageBuf);
    }
}

static void CheckResult(HRESULT result, const char* what, const char *file, int line)
{
    if(result != S_OK) {
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, result, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        std::string str = fmt("%s at %s:%d failed with %d (%s)\n", what, file, line, result, messageBuf);
        OutputDebugStringA(str.data());
        DebugBreak();
        LocalFree(messageBuf);
    }
}

static void CheckXrResult(XrResult a, const char* what, const char *file, int line)
{
    if(a != XR_SUCCESS) {
        std::string str = fmt("%s at %s:%d failed with %d\n", what, file, line, a);
        OutputDebugStringA(str.data());
        DebugBreak();
    }
}


// Use this macro to test if HANDLE or pointer functions succeeded that also update LastError
#define CHECK_NOT_NULL(a) CheckResultWithLastError(((a) != NULL), #a, __FILE__, __LINE__)

// Use this macro to test if functions succeeded that also update LastError
#define CHECK_LAST_ERROR(a) CheckResultWithLastError((a), #a, __FILE__, __LINE__)

// Use this macro to test Direct3D functions
#define CHECK(a) CheckResult(a, #a, __FILE__, __LINE__)

// Use this macro to test OpenXR functions
#define CHECK_XR(a) CheckXrResult(a, #a, __FILE__, __LINE__)

// Supports only a single overlay / RPC session at this time

const char *kOverlayLayerName = "XR_EXT_overlay_api_layer";

DWORD gOverlayWorkerThreadId;
HANDLE gOverlayWorkerThread;

// Semaphore for blocking Overlay CreateSession until Main CreateSession has occurred
LPCSTR kOverlayCreateSessionSemaName = "XR_EXT_overlay_overlay_create_session_sema";
bool gMainSessionCreated = false;
HANDLE gOverlayCreateSessionSema;

// Semaphore for blocking Overlay WaitFrame while Main WaitFrame is occuring
LPCSTR kOverlayWaitFrameSemaName = "XR_EXT_overlay_overlay_wait_frame_sema";
HANDLE gOverlayWaitFrameSema;

// Semaphore for blocking Main DestroySession until Overlay DestroySession has occurred
LPCSTR kMainDestroySessionSemaName = "XR_EXT_overlay_main_destroy_session_sema";
HANDLE gMainDestroySessionSema;

// Main Session context that we hold on to for processing and interleaving
// Overlay Session commands
XrSession gSavedMainSession;
ID3D11Device *gSavedD3DDevice;
XrInstance gSavedInstance;
XrSystemId gSavedSystemId;
unsigned int overlaySessionStandin;
XrSession kOverlayFakeSession = reinterpret_cast<XrSession>(&overlaySessionStandin);

bool gExitIPCLoop = false;
bool gSerializeEverything = true;

enum { MAX_OVERLAY_LAYER_COUNT = 2 };

// WaitFrame state from Main Session for handing back to Overlay Session
XrFrameState gSavedWaitFrameState;

// Quad layers from Overlay Session to overlay on Main Session's layers
uint32_t gOverlayQuadLayerCount = 0;
XrCompositionLayerQuad gOverlayQuadLayers[MAX_OVERLAY_LAYER_COUNT];
std::set<XrSwapchain> gSwapchainsDestroyPending;

// Mutex synchronizing access to Main session and Overlay session commands
HANDLE gOverlayCallMutex = NULL;      // handle to sync object
LPCWSTR kOverlayMutexName = TEXT("XR_EXT_overlay_call_mutex");

static XrGeneratedDispatchTable *downchain = nullptr;

// Bookkeeping of SwapchainImages for copying remote SwapchainImages on ReleaseSwapchainImage
struct SwapchainCachedData
{
    XrSwapchain swapchain;
    std::vector<ID3D11Texture2D*> swapchainImages;
    std::set<HANDLE> remoteImagesAcquired;
    std::map<HANDLE, ID3D11Texture2D*> handleTextureMap;
    std::vector<uint32_t>   acquired;

    SwapchainCachedData(XrSwapchain swapchain_, const std::vector<ID3D11Texture2D*>& swapchainImages_) :
        swapchain(swapchain_),
        swapchainImages(swapchainImages_)
    {
        for(auto texture : swapchainImages) {
            texture->AddRef();
        }
    }

    ~SwapchainCachedData()
    {
        for(HANDLE acquired : remoteImagesAcquired) {
            IDXGIKeyedMutex* keyedMutex;
            {
                ID3D11Texture2D *sharedTexture = getSharedTexture(acquired);
                CHECK(sharedTexture->QueryInterface( __uuidof(IDXGIKeyedMutex), (LPVOID*)&keyedMutex));
            }
            CHECK(keyedMutex->ReleaseSync(KEYED_MUTEX_IPC_REMOTE));
            keyedMutex->Release();
        }
        remoteImagesAcquired.clear();
        for(auto shared : handleTextureMap) {
            shared.second->Release();
            CloseHandle(shared.first);
        }
        for(auto texture : swapchainImages) {
            texture->Release();
        }
        handleTextureMap.clear();
    }

    ID3D11Texture2D* getSharedTexture(HANDLE sourceHandle)
    {
        ID3D11Texture2D *sharedTexture;

        ID3D11Device1 *device1;
        CHECK(gSavedD3DDevice->QueryInterface(__uuidof (ID3D11Device1), (void **)&device1));
        auto it = handleTextureMap.find(sourceHandle);
        if(it == handleTextureMap.end()) {
            CHECK(device1->OpenSharedResource1(sourceHandle, __uuidof(ID3D11Texture2D), (LPVOID*) &sharedTexture));
            handleTextureMap[sourceHandle] = sharedTexture;
        } else  {
            sharedTexture = it->second;
        }
        device1->Release();

        return sharedTexture;
    }
};

typedef std::unique_ptr<SwapchainCachedData> SwapchainCachedDataPtr;
std::map<XrSwapchain, SwapchainCachedDataPtr> gSwapchainMap;

#ifdef __cplusplus    // If used by C++ code, 
extern "C" {          // we need to export the C interface
#endif


// Negotiate an interface with the loader 
XR_OVERLAY_EXT_API XrResult Overlay_xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *loaderInfo, 
                                                    const char *layerName,
                                                    XrNegotiateApiLayerRequest *layerRequest) 
{
    if (nullptr != layerName)
    {
        if (0 != strncmp(kOverlayLayerName, layerName, strnlen_s(kOverlayLayerName, XR_MAX_API_LAYER_NAME_SIZE)))
        {
            return XR_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (nullptr == loaderInfo || 
        nullptr == layerRequest || 
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || 
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        layerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        layerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        layerRequest->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->minApiVersion < XR_MAKE_VERSION(0, 9, 0) || 
        loaderInfo->minApiVersion >= XR_MAKE_VERSION(1, 1, 0))
    {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    layerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    layerRequest->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
    layerRequest->getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(Overlay_xrGetInstanceProcAddr);
    layerRequest->createApiLayerInstance = reinterpret_cast<PFN_xrCreateApiLayerInstance>(Overlay_xrCreateApiLayerInstance);

    return XR_SUCCESS;
}

XrResult Overlay_xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance) 
{ 
    // Layer initialization here
    return XR_SUCCESS; 
}

XrResult Overlay_xrDestroyInstance(XrInstance instance) 
{ 
    // Layer cleanup here
    return XR_SUCCESS; 
}

DWORD WINAPI ThreadBody(LPVOID)
{
    void *shmem = IPCGetSharedMemory();
    bool connectionIsActive = false;
    HANDLE remoteProcessHandle = 0;

    do {
        // XXX Super Awkward, should probably have a separate Handshake loop
        IPCWaitResult result;
        if(connectionIsActive) {
            result = IPCWaitForGuestRequestOrTermination(remoteProcessHandle);
        } else {
            result = IPCWaitForGuestRequest();
        }

        if(result == IPC_REMOTE_PROCESS_TERMINATED) {

            DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
            if(waitresult == WAIT_TIMEOUT) {
                OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
                DebugBreak();
            }
            gSwapchainMap.clear();
            gOverlayQuadLayerCount = 0;
            ReleaseMutex(gOverlayCallMutex);
            connectionIsActive = false;
            continue;

        } else if(result == IPC_WAIT_ERROR) {

            DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
            if(waitresult == WAIT_TIMEOUT) {
                OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
                DebugBreak();
            }
            gSwapchainMap.clear();
            gOverlayQuadLayerCount = 0;
            ReleaseMutex(gOverlayCallMutex);
            connectionIsActive = false;
            OutputDebugStringA("IPC Wait Error\n");
            break;
        }

        IPCBuffer ipcbuf = IPCGetBuffer();
        IPCXrHeader *hdr;
        hdr = ipcbuf.getAndAdvance<IPCXrHeader>();

        hdr->makePointersAbsolute(ipcbuf.base);

        switch(hdr->requestType) {

            case IPC_HANDSHAKE: {
                // Establish IPC parameters and make initial handshake
                auto args = ipcbuf.getAndAdvance<IPCXrHandshake>();

                // Wait on main session
                if(!gMainSessionCreated) {
                    DWORD waitresult = WaitForSingleObject(gOverlayCreateSessionSema, INFINITE);
                    if(waitresult == WAIT_TIMEOUT) {
                        OutputDebugStringA("**OVERLAY** create session timeout\n");
                        DebugBreak();
                    }
                }

                hdr->result = XR_SUCCESS;

                connectionIsActive = true;
                DWORD remote = args->remoteProcessId;
                CHECK_NOT_NULL(remoteProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, TRUE, args->remoteProcessId));

                *(args->instance) = gSavedInstance;
                *(args->systemId) = gSavedSystemId;

                {
                    IDXGIDevice * dxgiDevice;
                    CHECK(gSavedD3DDevice->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice));

                    IDXGIAdapter *adapter;
                    CHECK(dxgiDevice->GetAdapter(&adapter));

                    DXGI_ADAPTER_DESC desc;
                    CHECK(adapter->GetDesc(&desc));

                    *(args->adapterLUID) = desc.AdapterLuid;

                    *(args->hostProcessId) = GetCurrentProcessId();

                    dxgiDevice->Release();
                }

                break;
            }

            case IPC_XR_CREATE_SESSION: {
                auto args = ipcbuf.getAndAdvance<IPCXrCreateSession>();
                hdr->result = Overlay_xrCreateSession(args->instance, args->createInfo, args->session);
                break;
            }

            case IPC_XR_CREATE_REFERENCE_SPACE: {
                auto args = ipcbuf.getAndAdvance<IPCXrCreateReferenceSpace>();
                hdr->result = Overlay_xrCreateReferenceSpace(args->session, args->createInfo, args->space);
                break;
            }

            case IPC_XR_ENUMERATE_SWAPCHAIN_FORMATS: { 
                auto args = ipcbuf.getAndAdvance<IPCXrEnumerateSwapchainFormats>();
                hdr->result = Overlay_xrEnumerateSwapchainFormats(args->session, args->formatCapacityInput, args->formatCountOutput, args->formats);
                break;
            }

            case IPC_XR_CREATE_SWAPCHAIN: {
                auto args = ipcbuf.getAndAdvance<IPCXrCreateSwapchain>();
                hdr->result = Overlay_xrCreateSwapchain(args->session, args->createInfo, args->swapchain);
                if(hdr->result == XR_SUCCESS) {

                    uint32_t count;
                    CHECK_XR(downchain->EnumerateSwapchainImages(*args->swapchain, 0, &count, nullptr));

                    std::vector<XrSwapchainImageD3D11KHR> swapchainImages(count);
                    std::vector<ID3D11Texture2D*> swapchainTextures(count);
                    for(uint32_t i = 0; i < count; i++) {
                        swapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                        swapchainImages[i].next = nullptr;
                    }
                    CHECK_XR(downchain->EnumerateSwapchainImages(*args->swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data())));

                    for(uint32_t i = 0; i < count; i++) {
                        swapchainTextures[i] = swapchainImages[i].texture;
                    }
                  
                    gSwapchainMap[*args->swapchain] = SwapchainCachedDataPtr(new SwapchainCachedData(*args->swapchain, swapchainTextures));
                    *args->swapchainCount = count;
                }
                break;
            }

            case IPC_XR_BEGIN_FRAME: {
                auto args = ipcbuf.getAndAdvance<IPCXrBeginFrame>();
                hdr->result = Overlay_xrBeginFrame(args->session, args->frameBeginInfo);
                break;
            }

            case IPC_XR_WAIT_FRAME: {
                auto args = ipcbuf.getAndAdvance<IPCXrWaitFrame>();
                hdr->result = Overlay_xrWaitFrame(args->session, args->frameWaitInfo, args->frameState);
                break;
            }

            case IPC_XR_END_FRAME: {
                auto args = ipcbuf.getAndAdvance<IPCXrEndFrame>();
                hdr->result = Overlay_xrEndFrame(args->session, args->frameEndInfo);
                break;
            }

            case IPC_XR_ACQUIRE_SWAPCHAIN_IMAGE: {
                auto args = ipcbuf.getAndAdvance<IPCXrAcquireSwapchainImage>();
                hdr->result = downchain->AcquireSwapchainImage(args->swapchain, args->acquireInfo, args->index);
                if(hdr->result == XR_SUCCESS) {
                    auto& cache = gSwapchainMap[args->swapchain];
                    cache->acquired.push_back(*args->index);
                }
                break;
            }

            case IPC_XR_WAIT_SWAPCHAIN_IMAGE: {
                auto args = ipcbuf.getAndAdvance<IPCXrWaitSwapchainImage>();
                hdr->result = downchain->WaitSwapchainImage(args->swapchain, args->waitInfo);
                auto& cache = gSwapchainMap[args->swapchain];
                if(cache->remoteImagesAcquired.find(args->sourceImage) != cache->remoteImagesAcquired.end()) {
                    IDXGIKeyedMutex* keyedMutex;
                    {
                        ID3D11Texture2D *sharedTexture = cache->getSharedTexture(args->sourceImage);
                        CHECK(sharedTexture->QueryInterface( __uuidof(IDXGIKeyedMutex), (LPVOID*)&keyedMutex));
                    }
                    cache->remoteImagesAcquired.erase(args->sourceImage);
                    CHECK(keyedMutex->ReleaseSync(KEYED_MUTEX_IPC_REMOTE));
                    keyedMutex->Release();
                }
                break;
            }

            case IPC_XR_RELEASE_SWAPCHAIN_IMAGE: {
                auto args = ipcbuf.getAndAdvance<IPCXrReleaseSwapchainImage>();
                auto& cache = gSwapchainMap[args->swapchain];

                ID3D11Texture2D *sharedTexture = cache->getSharedTexture(args->sourceImage);

                {
                    IDXGIKeyedMutex* keyedMutex;
                    CHECK(sharedTexture->QueryInterface( __uuidof(IDXGIKeyedMutex), (LPVOID*)&keyedMutex));
                    CHECK(keyedMutex->AcquireSync(KEYED_MUTEX_IPC_HOST, INFINITE));
                    keyedMutex->Release();
                }

                cache->remoteImagesAcquired.insert(args->sourceImage);
                int which = cache->acquired[0];
                cache->acquired.erase(cache->acquired.begin());

                ID3D11DeviceContext* d3dContext;
                gSavedD3DDevice->GetImmediateContext(&d3dContext);
                d3dContext->CopyResource(cache->swapchainImages[which], sharedTexture);
                hdr->result = downchain->ReleaseSwapchainImage(args->swapchain, args->releaseInfo);
                break;
            }

            case IPC_XR_DESTROY_SESSION: { 
                auto args = ipcbuf.getAndAdvance<IPCXrDestroySession>();
                hdr->result = Overlay_xrDestroySession(args->session);
                DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
                if(waitresult == WAIT_TIMEOUT) {
                    OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
                    DebugBreak();
                }
                gSwapchainMap.clear();
                ReleaseMutex(gOverlayCallMutex);
                break;
            }

            case IPC_XR_ENUMERATE_VIEW_CONFIGURATIONS: {
                auto args = ipcbuf.getAndAdvance<IPCXrEnumerateViewConfigurations>();
                hdr->result = downchain->EnumerateViewConfigurations(args->instance, args->systemId, args->viewConfigurationTypeCapacityInput, args->viewConfigurationTypeCountOutput, args->viewConfigurationTypes);
                break;
            }

            case IPC_XR_ENUMERATE_VIEW_CONFIGURATION_VIEWS: {
                auto args = ipcbuf.getAndAdvance<IPCXrEnumerateViewConfigurationViews>();
                hdr->result = downchain->EnumerateViewConfigurationViews(args->instance, args->systemId, args->viewConfigurationType, args->viewCapacityInput, args->viewCountOutput, args->views);
                break;
            }

            case IPC_XR_GET_VIEW_CONFIGURATION_PROPERTIES: {
                auto args = ipcbuf.getAndAdvance<IPCXrGetViewConfigurationProperties>();
                hdr->result = downchain->GetViewConfigurationProperties(args->instance, args->systemId, args->viewConfigurationType, args->configurationProperties);
                break;
            }

            case IPC_XR_DESTROY_SWAPCHAIN: {
                auto args = ipcbuf.getAndAdvance<IPCXrDestroySwapchain>();
                hdr->result = Overlay_xrDestroySwapchain(args->swapchain);
                gSwapchainMap.erase(args->swapchain);
                break;
            }

            case IPC_XR_DESTROY_SPACE: {
                auto args = ipcbuf.getAndAdvance<IPCXrDestroySpace>();
                hdr->result = Overlay_xrDestroySpace(args->space);
                break;
            }

            case IPC_XR_BEGIN_SESSION: {
                auto args = ipcbuf.getAndAdvance<IPCXrBeginSession>();
                hdr->result = Overlay_xrBeginSession(args->session, args->beginInfo);
                break;
            }

            case IPC_XR_END_SESSION: {
                auto args = ipcbuf.getAndAdvance<IPCXrEndSession>();
                hdr->result = Overlay_xrEndSession(args->session);
                break;
            }

            default:
                OutputDebugStringA("unknown request type in IPC");
                abort();
                break;
        }

        hdr->makePointersRelative(ipcbuf.base);
        IPCFinishHostResponse();

    } while(!gExitIPCLoop);

    return 0;
}

void CreateOverlaySessionThread()
{
    CHECK_NOT_NULL(gOverlayCreateSessionSema =
        CreateSemaphoreA(nullptr, 0, 1, kOverlayCreateSessionSemaName));
    CHECK_NOT_NULL(gOverlayWaitFrameSema =
        CreateSemaphoreA(nullptr, 0, 1, kOverlayWaitFrameSemaName));
    CHECK_NOT_NULL(gMainDestroySessionSema =
        CreateSemaphoreA(nullptr, 0, 1, kMainDestroySessionSemaName));
    CHECK_NOT_NULL(gOverlayCallMutex = CreateMutex(nullptr, TRUE, kOverlayMutexName));
    ReleaseMutex(gOverlayCallMutex);

    CHECK_NOT_NULL(gOverlayWorkerThread =
        CreateThread(nullptr, 0, ThreadBody, nullptr, 0, &gOverlayWorkerThreadId));
    OutputDebugStringA("**OVERLAY** success creating IPC thread\n");
}

XrResult Overlay_xrCreateApiLayerInstance(const XrInstanceCreateInfo *info, const struct XrApiLayerCreateInfo *apiLayerInfo, XrInstance *instance) 
{
    assert(0 == strncmp(kOverlayLayerName, apiLayerInfo->nextInfo->layerName, strnlen_s(kOverlayLayerName, XR_MAX_API_LAYER_NAME_SIZE)));
    assert(nullptr != apiLayerInfo->nextInfo);

    // Copy the contents of the layer info struct, but then move the next info up by
    // one slot so that the next layer gets information.
    XrApiLayerCreateInfo local_api_layer_info = {};
    memcpy(&local_api_layer_info, apiLayerInfo, sizeof(XrApiLayerCreateInfo));
    local_api_layer_info.nextInfo = apiLayerInfo->nextInfo->next;

    // Get the function pointers we need
    PFN_xrGetInstanceProcAddr       pfn_next_gipa = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance    pfn_next_cali = apiLayerInfo->nextInfo->nextCreateApiLayerInstance;

    // Create the instance
    XrInstance returned_instance = *instance;
    XrResult result = pfn_next_cali(info, &local_api_layer_info, &returned_instance);
    *instance = returned_instance;
    gSavedInstance = returned_instance;

    // Create the dispatch table to the next levels
    downchain = new XrGeneratedDispatchTable();
    GeneratedXrPopulateDispatchTable(downchain, returned_instance, pfn_next_gipa);

    // TBD where should the layer's dispatch table live? File global for now...

    //std::unique_lock<std::mutex> mlock(g_instance_dispatch_mutex);
    //g_instance_dispatch_map[returned_instance] = next_dispatch;

    CreateOverlaySessionThread();

    return result;
}

XrResult Overlay_xrGetSystemProperties(
    XrInstance instance,
    XrSystemId systemId,
    XrSystemProperties* properties)
{
    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    XrResult result;

    result = downchain->GetSystemProperties(instance, systemId, properties);

    if(result == XR_SUCCESS) {

        // Reserve one for overlay
        // TODO : should remove for main session, should return only max overlay layers for overlay session
        properties->graphicsProperties.maxLayerCount =
            properties->graphicsProperties.maxLayerCount - MAX_OVERLAY_LAYER_COUNT;
    }

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrBeginSession(
    XrSession session,
    const XrSessionBeginInfo*                   beginInfo)
{
    XrResult result;

    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    if(session == kOverlayFakeSession) {

        // TODO life cycle events to Overlay Session
        result = XR_SUCCESS;

    } else {

        result = downchain->BeginSession(session, beginInfo);

    }

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrEndSession(
    XrSession session)
{
    XrResult result;

    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    if(session == kOverlayFakeSession) {

        // TODO life cycle events to Overlay Session
        result = XR_SUCCESS;

    } else {

        result = downchain->EndSession(session);

    }

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrDestroySession(
    XrSession session)
{
    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    XrResult result;

    if(session == kOverlayFakeSession) {
        // overlay session

        gOverlayQuadLayerCount = 0;
        ReleaseSemaphore(gMainDestroySessionSema, 1, nullptr);
        result = XR_SUCCESS;

    } else {
        // main session

        ReleaseSemaphore(gOverlayWaitFrameSema, 1, nullptr);
        DWORD waitresult = WaitForSingleObject(gMainDestroySessionSema, 1000000);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA("**OVERLAY** main destroy session timeout\n");
            DebugBreak();
        }
        result = downchain->DestroySession(session);
        gExitIPCLoop = true;
    }

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }
    return result;
}

XrResult Overlay_xrDestroySwapchain(XrSwapchain swapchain)
{
    XrResult result;

    DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
    if(waitresult == WAIT_TIMEOUT) {
        OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
        DebugBreak();
    }

    bool isSubmitted = false;
    // XXX there's probably an elegant C++ find with lambda that would do this:
    for(uint32_t i = 0; i < gOverlayQuadLayerCount; i++) {
        if(gOverlayQuadLayers[i].subImage.swapchain == swapchain) {
            isSubmitted = true;
        }
    }

    if(isSubmitted) {
        gSwapchainsDestroyPending.insert(swapchain);
        result = XR_SUCCESS;
    } else {
        result = downchain->DestroySwapchain(swapchain);
    }

    ReleaseMutex(gOverlayCallMutex);

    return result;
}

XrResult Overlay_xrDestroySpace(XrSpace space)
{
    XrResult result;

    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    result = downchain->DestroySpace(space);

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrCreateSession(
    XrInstance instance,
    const XrSessionCreateInfo* createInfo,
    XrSession* session)
{
    XrResult result;

    const XrBaseInStructure* p = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    const XrSessionCreateInfoOverlayEXT* cio = nullptr;
    const XrGraphicsBindingD3D11KHR* d3dbinding = nullptr;
    while(p != nullptr) {
        if(p->type == XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXT) {
            cio = reinterpret_cast<const XrSessionCreateInfoOverlayEXT*>(p);
        }
        if( (p->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR) ||
            (p->type == XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR)) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        if(p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            d3dbinding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p);
        }
        p = reinterpret_cast<const XrBaseInStructure*>(p->next);
    }

    // TODO handle the case where Main session passes the
    // overlaycreateinfo but overlaySession = FALSE
    if(cio == nullptr) {

        // Main session

        // TODO : remake chain without InfoOverlayEXT

        result = downchain->CreateSession(instance, createInfo, session);
        if(result != XR_SUCCESS)
            return result;

        gSavedSystemId = createInfo->systemId;
        gSavedMainSession = *session;
        gSavedD3DDevice = d3dbinding->device;
        ID3D11Multithread* d3dMultithread;
        CHECK(gSavedD3DDevice->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&d3dMultithread)));
        d3dMultithread->SetMultithreadProtected(TRUE);
        d3dMultithread->Release();

        // Let overlay session continue
        gMainSessionCreated = true;
        ReleaseSemaphore(gOverlayCreateSessionSema, 1, nullptr);
		 
    } else {

        // TODO should store any kind of failure in main XrCreateSession and then fall through here
        *session = kOverlayFakeSession;
        result = XR_SUCCESS;
    }

    return result;
}

XrResult Overlay_xrEnumerateSwapchainFormats(
    XrSession                                   session,
    uint32_t                                    formatCapacityInput,
    uint32_t*                                   formatCountOutput,
    int64_t*                                    formats)
{ 
    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    if(session == kOverlayFakeSession) {
        session = gSavedMainSession;
    }

    XrResult result = downchain->EnumerateSwapchainFormats(session, formatCapacityInput, formatCountOutput, formats);

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images)
{ 
    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    XrResult result = downchain->EnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space)
{ 
    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    if(session == kOverlayFakeSession) {
        session = gSavedMainSession;
    }

    XrResult result = downchain->CreateReferenceSpace(session, createInfo, space);

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrCreateSwapchain(XrSession session, const  XrSwapchainCreateInfo *createInfo, XrSwapchain *swapchain) 
{ 
    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    if(session == kOverlayFakeSession) {
        session = gSavedMainSession;
    }

    XrResult result = downchain->CreateSwapchain(session, createInfo, swapchain);
    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }

    return result;
}

XrResult Overlay_xrWaitFrame(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state) 
{
    XrResult result;

    if(session == kOverlayFakeSession) {

        // Wait on main session
        // TODO - make first wait be long and subsequent waits be short,
        // since it looks like WaitFrame may wait a long time on runtime.
        DWORD waitresult = WaitForSingleObject(gOverlayWaitFrameSema, 10000);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA("**OVERLAY** overlay session wait frame timeout\n");
            DebugBreak();
        }

        waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }

        // TODO pass back any failure recorded by main session waitframe
        *state = gSavedWaitFrameState;

        ReleaseMutex(gOverlayCallMutex);

        result = XR_SUCCESS;

    } else {

        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }

        result = downchain->WaitFrame(session, info, state);

        ReleaseMutex(gOverlayCallMutex);

        gSavedWaitFrameState = *state;
        ReleaseSemaphore(gOverlayWaitFrameSema, 1, nullptr);
    }

    return result;
}

XrResult Overlay_xrBeginFrame(XrSession session, const XrFrameBeginInfo *info) 
{ 
    XrResult result;

    if(gSerializeEverything) {
        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
            DebugBreak();
        }
    }

    if(session == kOverlayFakeSession) {

        // Do nothing in overlay session
        result = XR_SUCCESS;

    } else {

        result = downchain->BeginFrame(session, info);

    }

    if(gSerializeEverything) {
        ReleaseMutex(gOverlayCallMutex);
    }
    return result;
}

XrResult Overlay_xrEndFrame(XrSession session, const XrFrameEndInfo *info) 
{
    XrResult result;

    DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
    if(waitresult == WAIT_TIMEOUT) {
        OutputDebugStringA(fmt("**OVERLAY** timeout waiting at %s:%d on gOverlayCallMutex\n", __FILE__, __LINE__).c_str());
        DebugBreak();
    }

    if(session == kOverlayFakeSession) {

        // TODO: validate blend mode matches main session

        if(info->layerCount > MAX_OVERLAY_LAYER_COUNT) {
            gOverlayQuadLayerCount = 0;
            result = XR_ERROR_LAYER_LIMIT_EXCEEDED;
        } else {
            bool valid = true;
            for(uint32_t i = 0; i < info->layerCount; i++) {
                if(info->layers[0]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    result = XR_ERROR_LAYER_INVALID;
                    valid = false;
                    break;
                }
            }
            if(valid) {
                gOverlayQuadLayerCount = info->layerCount;
                for(uint32_t i = 0; i < info->layerCount; i++) {
                    gOverlayQuadLayers[i] = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
                }
                result = XR_SUCCESS;
            } else {
                gOverlayQuadLayerCount = 0;
            }
        }

    } else {

        XrFrameEndInfo info2 = *info;
        std::unique_ptr<const XrCompositionLayerBaseHeader*> layers2(new const XrCompositionLayerBaseHeader*[info->layerCount + gOverlayQuadLayerCount]);
        memcpy(layers2.get(), info->layers, sizeof(const XrCompositionLayerBaseHeader*) * info->layerCount);
        for(uint32_t i = 0; i < gOverlayQuadLayerCount; i++)
            layers2.get()[info->layerCount + i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&gOverlayQuadLayers);

        info2.layerCount = info->layerCount + gOverlayQuadLayerCount;
        info2.layers = layers2.get();
        result = downchain->EndFrame(session, &info2);

        // XXX there's probably an elegant C++ find with lambda that would do this:
        auto copyOfPendingDestroys = gSwapchainsDestroyPending;
        for(auto swapchain : copyOfPendingDestroys) {
            bool isSubmitted = false;
            for(uint32_t i = 0; i < gOverlayQuadLayerCount; i++) {
                if(gOverlayQuadLayers[i].subImage.swapchain == swapchain) {
                    isSubmitted = true;
                }
            }
            if(!isSubmitted) {
                result = downchain->DestroySwapchain(swapchain);
                gSwapchainsDestroyPending.erase(swapchain);
            }
        }
    }

    ReleaseMutex(gOverlayCallMutex);
    return result;
}

XrResult Overlay_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
  if (0 == strcmp(name, "xrGetInstanceProcAddr")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrGetInstanceProcAddr);
  } else if (0 == strcmp(name, "xrCreateInstance")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrCreateInstance);
  } else if (0 == strcmp(name, "xrDestroyInstance")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrDestroyInstance);
  } else if (0 == strcmp(name, "xrCreateSwapchain")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrCreateSwapchain);
  } else if (0 == strcmp(name, "xrBeginFrame")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrBeginFrame);
  } else if (0 == strcmp(name, "xrEndFrame")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrEndFrame);
  } else if (0 == strcmp(name, "xrGetSystemProperties")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrGetSystemProperties);
  } else if (0 == strcmp(name, "xrWaitFrame")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrWaitFrame);
  } else if (0 == strcmp(name, "xrCreateSession")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrCreateSession);
  } else if (0 == strcmp(name, "xrDestroySession")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrDestroySession);
  } else if (0 == strcmp(name, "xrCreateReferenceSpace")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrCreateReferenceSpace);
  } else if (0 == strcmp(name, "xrEnumerateSwapchainFormats")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrEnumerateSwapchainFormats);
  } else if (0 == strcmp(name, "xrEnumerateSwapchainImages")) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Overlay_xrEnumerateSwapchainImages);
  } else {
    *function = nullptr;
  }

      // If we setup the function, just return
    if (*function != nullptr) {
        return XR_SUCCESS;
    }

    if(downchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    return downchain->GetInstanceProcAddr(instance, name, function);
}

#ifdef __cplusplus
}   // extern "C"
#endif
