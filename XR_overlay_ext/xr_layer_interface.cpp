#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

#include <cassert>
#include <cstring>
#include <cstdio>

#define XR_USE_GRAPHICS_API_D3D11 1

#include "xr_overlay_dll.h"
#include "xr_generated_dispatch_table.h"
#include "xr_linear.h"

#include <d3d11_4.h>

const char *kOverlayLayerName = "XR_EXT_overlay_api_layer";
DWORD gOverlayWorkerThreadId;
HANDLE gOverlayWorkerThread;
LPCSTR kOverlayCreateSessionSemaName = "XR_EXT_overlay_overlay_create_session_sema";
HANDLE gOverlayCreateSessionSema;
LPCSTR kOverlayWaitFrameSemaName = "XR_EXT_overlay_overlay_wait_frame_sema";
HANDLE gOverlayWaitFrameSema;
LPCSTR kMainDestroySessionSemaName = "XR_EXT_overlay_main_destroy_session_sema";
HANDLE gMainDestroySessionSema;

ID3D11Device *gSavedD3DDevice;
XrInstance gSavedInstance;
unsigned int overlaySessionStandin;
XrSession kOverlayFakeSession = reinterpret_cast<XrSession>(&overlaySessionStandin);
XrSession gSavedMainSession;
XrSession gOverlaySession;
bool gExitOverlay = false;
bool gSerializeEverything = true;

const uint64_t ONE_SECOND_IN_NANOSECONDS = 1000000000;

enum { MAX_OVERLAY_LAYER_COUNT = 2 };

XrFrameState gSavedWaitFrameState;

HANDLE gOverlayCallMutex = NULL;      // handle to sync object
LPCWSTR kOverlayMutexName = TEXT("XR_EXT_overlay_call_mutex");

namespace Math {
namespace Pose {
XrPosef Identity() {
    XrPosef t{};
    t.orientation.w = 1;
    return t;
}

XrPosef Translation(const XrVector3f& translation) {
    XrPosef t = Identity();
    t.position = translation;
    return t;
}

XrPosef RotateCCWAboutYAxis(float radians, XrVector3f translation) {
    XrPosef t = Identity();
    t.orientation.x = 0.f;
    t.orientation.y = std::sin(radians * 0.5f);
    t.orientation.z = 0.f;
    t.orientation.w = std::cos(radians * 0.5f);
    t.position = translation;
    return t;
}
}  // namespace Pose
}  // namespace Math

static XrGeneratedDispatchTable *downchain = nullptr;

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
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        std::string str = fmt("%s at %s:%d failed with %d (%s)\n", what, file, line, lastError, messageBuf);
        OutputDebugStringA(str.data());
        DebugBreak();
        LocalFree(messageBuf);
    }
}

#define CHECK_NOT_NULL(a) CheckResultWithLastError(((a) != NULL), #a, __FILE__, __LINE__)

#define CHECK(a) CheckResult(a, #a, __FILE__, __LINE__)

#define CHECK_XR(a) CheckXrResult(a, #a, __FILE__, __LINE__)

static void CheckXrResult(XrResult a, const char* what, const char *file, int line)
{
    if(a != XR_SUCCESS) {
        std::string str = fmt("%s at %s:%d failed with %d\n", what, file, line, a);
        OutputDebugStringA(str.data());
        DebugBreak();
    }
}

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

    bool continueIPC = true;
    do {
        IPCWaitForGuestRequest(); // XXX TODO check response
        OutputDebugStringA("**OVERLAY** success in waiting for guest request\n");

        IPCBuffer ipcbuf = IPCGetBuffer();
        IPCXrHeader *hdr;
        hdr = ipcbuf.getAndAdvance<IPCXrHeader>();

        hdr->makePointersAbsolute(ipcbuf.base);

        switch(hdr->requestType) {

            case IPC_HANDSHAKE: {
                // Establish IPC parameters and make initial handshake
                IPCXrHandshake *args = ipcbuf.getAndAdvance<IPCXrHandshake>();
                hdr->result = XR_SUCCESS;
                *(args->instance) = gSavedInstance;
                break;
            }

            case IPC_XR_CREATE_SESSION: {
                IPCXrCreateSession *args = ipcbuf.getAndAdvance<IPCXrCreateSession>();
                hdr->result = Overlay_xrCreateSession(args->instance, args->createInfo, args->session);
                break;
            }

            case IPC_XR_CREATE_REFERENCE_SPACE: {
                IPCXrCreateReferenceSpace *args = ipcbuf.getAndAdvance<IPCXrCreateReferenceSpace>();
                hdr->result = Overlay_xrCreateReferenceSpace(args->session, args->createInfo, args->space);
                break;
            }

            case IPC_XR_ENUMERATE_SWAPCHAIN_FORMATS: { 
                IPCXrEnumerateSwapchainFormats *args = ipcbuf.getAndAdvance<IPCXrEnumerateSwapchainFormats>();
                hdr->result = Overlay_xrEnumerateSwapchainFormats(args->session, args->formatCapacityInput, args->formatCountOutput, args->formats);
                break;
            }

            case IPC_XR_DESTROY_SESSION: { 
                continueIPC = false;
            }

            default:
                OutputDebugStringA("unknown request type in IPC");
                abort();
                break;
        }

        hdr->makePointersRelative(ipcbuf.base);
        IPCFinishHostResponse();

    } while(continueIPC);
    OutputDebugStringA("**OVERLAY** exited IPC loop\n");

    // gOverlaySession was saved off when we proxied the IPC call to CreateSession

    OutputDebugStringA("**OVERLAY** destroyed session, exiting\n");
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
    OutputDebugStringA("**OVERLAY** success\n");
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

/*

Final
    add "external synchronization" around all funcs needing external synch
    add special conditional "always synchronize" in case runtime misbehaves
    catch and pass all functions
    run with validation layer?
*/

// TODO catch PollEvent
// if not overlay
//     normal
// else if overlay
//     transmit STOPPING

// TODO WaitFrame
// if main
//     mark have waitframed since beginsession
//     chain waitframe and save off results
// if overlay
//     wait on main waitframe if not waitframed since beginsession
//     use results saved from main

XrResult Overlay_xrGetSystemProperties(
    XrInstance instance,
    XrSystemId systemId,
    XrSystemProperties* properties)
{
    XrResult result;

    result = downchain->GetSystemProperties(instance, systemId, properties);
    if(result != XR_SUCCESS)
	return result;

    // Reserve one for overlay
    // TODO : should remove for main session, should return only max overlay layers for overlay session
    properties->graphicsProperties.maxLayerCount =
        properties->graphicsProperties.maxLayerCount - MAX_OVERLAY_LAYER_COUNT;

    return result;
}

XrResult Overlay_xrDestroySession(
    XrSession session)
{
    XrResult result;

    if(session == kOverlayFakeSession) {
        // overlay session

        ReleaseSemaphore(gMainDestroySessionSema, 1, nullptr);
        result = XR_SUCCESS;

    } else {
        // main session

        gExitOverlay = true;
        ReleaseSemaphore(gOverlayWaitFrameSema, 1, nullptr);
        DWORD waitresult = WaitForSingleObject(gMainDestroySessionSema, 1000000);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA("**OVERLAY** main destroy session timeout\n");
            DebugBreak();
        }
        OutputDebugStringA("**OVERLAY** main destroy session okay\n");
        result = downchain->DestroySession(session);
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

        gSavedMainSession = *session;
        gSavedD3DDevice = d3dbinding->device;

        ID3D11Multithread* d3dMultithread;
        CHECK(gSavedD3DDevice->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&d3dMultithread)));
        d3dMultithread->SetMultithreadProtected(TRUE);

        // Let overlay session continue
        ReleaseSemaphore(gOverlayCreateSessionSema, 1, nullptr);
		 
    } else {

        // Wait on main session
        DWORD waitresult = WaitForSingleObject(gOverlayCreateSessionSema, 10000);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA("**OVERLAY** overlay create session timeout\n");
            DebugBreak();
        }
        // TODO should store any kind of failure in main XrCreateSession and then fall through here
        *session = kOverlayFakeSession;
        gOverlaySession = *session; // XXX as loop is transferred to IPC
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
            OutputDebugStringA("**OVERLAY** timeout waiting in EnumerateSwapchainImages on gOverlayCallMutex\n");
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
            OutputDebugStringA("**OVERLAY** timeout waiting in EnumerateSwapchainImages on gOverlayCallMutex\n");
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
            OutputDebugStringA("**OVERLAY** timeout waiting in Overlay_xrCreateReferenceSpace on gOverlayCallMutex\n");
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
            OutputDebugStringA("**OVERLAY** timeout waiting in CreateSwapchain on gOverlayCallMutex\n");
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
            OutputDebugStringA("**OVERLAY** timeout waiting in WaitFrame in main session on gOverlayCallMutex\n");
            DebugBreak();
        }

        // TODO pass back any failure recorded by main session waitframe
        *state = gSavedWaitFrameState;

        ReleaseMutex(gOverlayCallMutex);

        result = XR_SUCCESS;

    } else {

        DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
        if(waitresult == WAIT_TIMEOUT) {
            OutputDebugStringA("**OVERLAY** timeout waiting in WaitFrame in main session on gOverlayCallMutex\n");
            DebugBreak();
        }

        result = downchain->WaitFrame(session, info, state);

        ReleaseMutex(gOverlayCallMutex);

        OutputDebugStringA("**OVERLAY** main session wait frame returned\n");

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
            OutputDebugStringA("**OVERLAY** timeout waiting in CreateSwapchain on gOverlayCallMutex\n");
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

uint32_t gOverlayQuadLayerCount = 0;
XrCompositionLayerQuad gOverlayQuadLayers[MAX_OVERLAY_LAYER_COUNT];

XrResult Overlay_xrEndFrame(XrSession session, const XrFrameEndInfo *info) 
{ 
    XrResult result;

    DWORD waitresult = WaitForSingleObject(gOverlayCallMutex, INFINITE);
    if(waitresult == WAIT_TIMEOUT) {
        OutputDebugStringA("**OVERLAY** timeout waiting in CreateSwapchain on gOverlayCallMutex\n");
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
