//
// Copyright 2019-2020 LunarG Inc. and PlutoVR Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Author: Brad Grantham <brad@lunarg.com>
//

#define NOMINMAX
#include <windows.h>
#include <tchar.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <map>
#include <memory>
#include <chrono>
#include <thread>

#define XR_USE_GRAPHICS_API_D3D11 1

#include "../XR_overlay_ext/xr_overlay_dll.h"
#include "../include/util.h"
#include <openxr/openxr_platform.h>

#include <dxgi1_2.h>
#include <d3d11_1.h>


// Local bookkeeping information associated with an XrSession (Mostly just a place to cache D3D11 device)
struct LocalSession
{
    XrSession       session;
    ID3D11Device*   d3d11;
    LocalSession(XrSession session_, ID3D11Device *d3d11_) :
        session(session_),
        d3d11(d3d11_)
    {}
    LocalSession(const LocalSession& l) :
        session(l.session),
        d3d11(l.d3d11)
    {}
};

typedef std::unique_ptr<LocalSession> LocalSessionPtr;
std::map<XrSession, LocalSessionPtr> gLocalSessionMap;

// The Id of the RPC Host Process
DWORD gHostProcessId;

// Local "Swapchain" in Xr parlance - others would call it RenderTarget
struct LocalSwapchain
{
    XrSwapchain             swapchain;
    std::vector<ID3D11Texture2D*> swapchainTextures;
    std::vector<HANDLE>          swapchainHandles;
    std::vector<uint32_t>   acquired;
    bool                    waited;

    LocalSwapchain(XrSwapchain sc, size_t count, ID3D11Device* d3d11, const XrSwapchainCreateInfo* createInfo) :
        swapchain(sc),
        swapchainTextures(count),
        swapchainHandles(count),
        waited(false)
    {
        for(int i = 0; i < count; i++) {
            D3D11_TEXTURE2D_DESC desc;
            desc.Width = createInfo->width;
            desc.Height = createInfo->height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = static_cast<DXGI_FORMAT>(createInfo->format);
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

            CHECK(d3d11->CreateTexture2D(&desc, NULL, &swapchainTextures[i]));

            {
                IDXGIResource1* sharedResource = NULL;
                CHECK(swapchainTextures[i]->QueryInterface(__uuidof(IDXGIResource1), (LPVOID*) &sharedResource));

                HANDLE thisProcessHandle;
                CHECK_NOT_NULL(thisProcessHandle = GetCurrentProcess());
                HANDLE hostProcessHandle;
                CHECK_NOT_NULL(hostProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, TRUE, gHostProcessId));

                HANDLE handle;

                // Get the Shared Handle for the texture. This is still local to this process but is an actual HANDLE
                CHECK(sharedResource->CreateSharedHandle(NULL,
                    DXGI_SHARED_RESOURCE_READ, // GENERIC_ALL | DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    NULL, &handle));
                
                // Duplicate the handle so "Host" RPC service process can use it
                CHECK_LAST_ERROR(DuplicateHandle(thisProcessHandle, handle, hostProcessHandle, &swapchainHandles[i], 0, TRUE, DUPLICATE_SAME_ACCESS));
                CHECK_LAST_ERROR(CloseHandle(handle));
                sharedResource->Release();
            }
        }
    }

    ~LocalSwapchain()
    {
        // XXX Need to AcquireSync from remote side?
        for(int i = 0; i < swapchainTextures.size(); i++) {
            swapchainTextures[i]->Release();
        }
    }
};

typedef std::unique_ptr<LocalSwapchain> LocalSwapchainPtr;
std::map<XrSwapchain, LocalSwapchainPtr> gLocalSwapchainMap;


// Serialization helpers ----------------------------------------------------

// MUST BE DEFAULT ONLY FOR LEAF OBJECTS (no pointers in them)
template <typename T>
T* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const T* p)
{
    if(!p)
        return nullptr;

    T* t = new(ipcbuf) T;
    if(!t)
        return nullptr;

    *t = *p;
    return t;
}

// MUST BE DEFAULT ONLY FOR LEAF OBJECTS (no pointers in them)
template <typename T>
T* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const T* p, size_t count)
{
    if(!p)
        return nullptr;

    T* t = new(ipcbuf) T[count];
    if(!t)
        return nullptr;

    for(size_t i = 0; i < count; i++)
        t[i] = p[i];

    return t;
}

// MUST BE DEFAULT ONLY FOR LEAF OBJECTS (no pointers in them)
template <typename T>
T* IPCSerializeNoCopy(IPCBuffer& ipcbuf, IPCXrHeader* header, const T* p)
{
    if(!p)
        return nullptr;

    T* t = new(ipcbuf) T;
    if(!t)
        return nullptr;

    return t;
}

// MUST BE DEFAULT ONLY FOR LEAF OBJECTS (no pointers in them)
template <typename T>
T* IPCSerializeNoCopy(IPCBuffer& ipcbuf, IPCXrHeader* header, const T* p, size_t count)
{
    if(!p)
        return nullptr;

    T* t = new(ipcbuf) T[count];
    if(!t)
        return nullptr;

    return t;
}

// MUST BE DEFAULT ONLY FOR LEAF OBJECTS (no pointers in them)
template <typename T>
void IPCCopyOut(T* dst, const T* src)
{
    if(!src)
        return;

    *dst = *src;
}

// MUST BE DEFAULT ONLY FOR LEAF OBJECTS (no pointers in them)
template <typename T>
void IPCCopyOut(T* dst, const T* src, size_t count)
{
    if(!src)
        return;

    for(size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

// Serialization of XR structs ----------------------------------------------

XrBaseInStructure* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const XrBaseInStructure* srcbase, CopyType copyType)
{
    return CopyXrStructChain(srcbase, copyType,
            [&ipcbuf](size_t size){return ipcbuf.allocate(size);},
            [&ipcbuf,&header](void* pointerToPointer){header->addOffsetToPointer(ipcbuf.base, pointerToPointer);});
}

// CopyOut XR structs -------------------------------------------------------

template <>
void IPCCopyOut(XrBaseOutStructure* dstbase, const XrBaseOutStructure* srcbase)
{
    bool skipped = true;

    do {
        skipped = false;

        if(!srcbase) {
            return;
        }

        switch(dstbase->type) {
            case XR_TYPE_SPACE_LOCATION: {
                auto src = reinterpret_cast<const XrSpaceLocation*>(srcbase);
                auto dst = reinterpret_cast<XrSpaceLocation*>(dstbase);
                dst->locationFlags = src->locationFlags;
                dst->pose = src->pose;
                break;
            }

            case XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR: {
                auto src = reinterpret_cast<const XrGraphicsRequirementsD3D11KHR*>(srcbase);
                auto dst = reinterpret_cast<XrGraphicsRequirementsD3D11KHR*>(dstbase);
                dst->adapterLuid = src->adapterLuid;
                dst->minFeatureLevel = src->minFeatureLevel;
                break;
            }

            case XR_TYPE_FRAME_STATE: {
                auto src = reinterpret_cast<const XrFrameState*>(srcbase);
                auto dst = reinterpret_cast<XrFrameState*>(dstbase);
                dst->predictedDisplayTime = src->predictedDisplayTime;
                dst->predictedDisplayPeriod = src->predictedDisplayPeriod;
                dst->shouldRender = src->shouldRender;
                break;
            }

            case XR_TYPE_INSTANCE_PROPERTIES: {
                auto src = reinterpret_cast<const XrInstanceProperties*>(srcbase);
                auto dst = reinterpret_cast<XrInstanceProperties*>(dstbase);
                dst->runtimeVersion = src->runtimeVersion;
                strncpy_s(dst->runtimeName, src->runtimeName, XR_MAX_RUNTIME_NAME_SIZE);
                break;
            }

            case XR_TYPE_EXTENSION_PROPERTIES: {
                auto src = reinterpret_cast<const XrExtensionProperties*>(srcbase);
                auto dst = reinterpret_cast<XrExtensionProperties*>(dstbase);
                strncpy_s(dst->extensionName, src->extensionName, XR_MAX_EXTENSION_NAME_SIZE);
                dst->extensionVersion = src->extensionVersion;
                break;
            }

            case XR_TYPE_SYSTEM_PROPERTIES: {
                auto src = reinterpret_cast<const XrSystemProperties*>(srcbase);
                auto dst = reinterpret_cast<XrSystemProperties*>(dstbase);
                dst->systemId = src->systemId;
                dst->vendorId = src->vendorId;
                dst->graphicsProperties = src->graphicsProperties;
                dst->trackingProperties = src->trackingProperties;
                strncpy_s(dst->systemName, src->systemName, XR_MAX_SYSTEM_NAME_SIZE);
                break;
            }

            case XR_TYPE_VIEW_CONFIGURATION_PROPERTIES: {
                auto src = reinterpret_cast<const XrViewConfigurationProperties*>(srcbase);
                auto dst = reinterpret_cast<XrViewConfigurationProperties*>(dstbase);
                dst->viewConfigurationType = src->viewConfigurationType;
                dst->fovMutable = src->fovMutable;
                break;
            }

            case XR_TYPE_VIEW_CONFIGURATION_VIEW: {
                auto src = reinterpret_cast<const XrViewConfigurationView*>(srcbase);
                auto dst = reinterpret_cast<XrViewConfigurationView*>(dstbase);
                dst->recommendedImageRectWidth = src->recommendedImageRectWidth;
                dst->maxImageRectWidth = src->maxImageRectWidth;
                dst->recommendedImageRectHeight = src->recommendedImageRectHeight;
                dst->maxImageRectHeight = src->maxImageRectHeight;
                dst->recommendedSwapchainSampleCount = src->recommendedSwapchainSampleCount;
                dst->maxSwapchainSampleCount = src->maxSwapchainSampleCount;
                break;
            }

            default: {
                // I don't know what this is, drop it and keep going
                outputDebugF("IPCCopyOut called to copy out to %p of unknown type %d - skipped.\n", dstbase, dstbase->type);

                dstbase = dstbase->next;
                skipped = true;

                // Don't increment srcbase.  Unknown structs were
                // dropped during serialization, so keep going until we
                // see a type we know and then we'll have caught up with
                // what was serialized.
                //
                break;
            }
        }
    } while(skipped);

    IPCCopyOut(dstbase->next, srcbase->next);
}

// xrEnumerateSwapchainFormats ----------------------------------------------

template <>
IPCXrEnumerateSwapchainFormats* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrEnumerateSwapchainFormats* src)
{
    auto dst = new(ipcbuf) IPCXrEnumerateSwapchainFormats;

    dst->session = src->session;
    dst->formatCapacityInput = src->formatCapacityInput;
    dst->formatCountOutput = IPCSerializeNoCopy(ipcbuf, header, src->formatCountOutput);
    header->addOffsetToPointer(ipcbuf.base, &dst->formatCountOutput);
    dst->formats = IPCSerializeNoCopy(ipcbuf, header, src->formats, src->formatCapacityInput);
    header->addOffsetToPointer(ipcbuf.base, &dst->formats);

    return dst;
}

template <>
void IPCCopyOut(IPCXrEnumerateSwapchainFormats* dst, const IPCXrEnumerateSwapchainFormats* src)
{
    IPCCopyOut(dst->formatCountOutput, src->formatCountOutput);
    if (src->formats) {
        IPCCopyOut(dst->formats, src->formats, *src->formatCountOutput);
    }
}

XrResult xrEnumerateSwapchainFormats(
    XrSession                                   session,
    uint32_t                                    formatCapacityInput,
    uint32_t*                                   formatCountOutput,
    int64_t*                                    formats)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_ENUMERATE_SWAPCHAIN_FORMATS};

    IPCXrEnumerateSwapchainFormats args {session, formatCapacityInput, formatCountOutput, formats};

    IPCXrEnumerateSwapchainFormats* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrEnumerateViewConfigurations --------------------------------------------

template <>
IPCXrEnumerateViewConfigurations* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrEnumerateViewConfigurations* src)
{
    auto dst = new(ipcbuf) IPCXrEnumerateViewConfigurations;

    dst->instance = src->instance;
    dst->systemId = src->systemId;
    dst->viewConfigurationTypeCapacityInput = src->viewConfigurationTypeCapacityInput;
    dst->viewConfigurationTypeCountOutput = IPCSerializeNoCopy(ipcbuf, header, src->viewConfigurationTypeCountOutput);
    header->addOffsetToPointer(ipcbuf.base, &dst->viewConfigurationTypeCountOutput);
    dst->viewConfigurationTypes = IPCSerializeNoCopy(ipcbuf, header, src->viewConfigurationTypes, src->viewConfigurationTypeCapacityInput);
    header->addOffsetToPointer(ipcbuf.base, &dst->viewConfigurationTypes);

    return dst;
}

template <>
void IPCCopyOut(IPCXrEnumerateViewConfigurations* dst, const IPCXrEnumerateViewConfigurations* src)
{
    IPCCopyOut(dst->viewConfigurationTypeCountOutput, src->viewConfigurationTypeCountOutput);
    if (src->viewConfigurationTypes) {
        IPCCopyOut(dst->viewConfigurationTypes, src->viewConfigurationTypes, *src->viewConfigurationTypeCountOutput);
    }
}

XrResult xrEnumerateViewConfigurations(
    XrInstance                                  instance,
    XrSystemId                                  systemId,
    uint32_t                                    viewConfigurationTypeCapacityInput,
    uint32_t*                                   viewConfigurationTypeCountOutput,
    XrViewConfigurationType*                    viewConfigurationTypes)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_ENUMERATE_VIEW_CONFIGURATIONS};

    IPCXrEnumerateViewConfigurations args {instance, systemId, viewConfigurationTypeCapacityInput, viewConfigurationTypeCountOutput, viewConfigurationTypes};

    IPCXrEnumerateViewConfigurations* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrGetInstanceProperties --------------------------------------------------

template <>
IPCXrGetInstanceProperties* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrGetInstanceProperties* src)
{
    auto dst = new(ipcbuf) IPCXrGetInstanceProperties;

    dst->instance = src->instance;

    dst->properties = reinterpret_cast<XrInstanceProperties*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->properties), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->properties);

    return dst;
}

template <>
void IPCCopyOut(IPCXrGetInstanceProperties* dst, const IPCXrGetInstanceProperties* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->properties),
            reinterpret_cast<const XrBaseOutStructure*>(src->properties)
            );
}

XrResult xrGetInstanceProperties (
    XrInstance                                   instance,
    XrInstanceProperties*                        properties)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_GET_INSTANCE_PROPERTIES};

    IPCXrGetInstanceProperties args {instance, properties};

    IPCXrGetInstanceProperties* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrGetPollEvent -----------------------------------------------------------

template <>
IPCXrPollEvent* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrPollEvent* src)
{
    auto dst = new(ipcbuf) IPCXrPollEvent;

    dst->instance = src->instance;

    dst->event = reinterpret_cast<XrEventDataBuffer*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->event), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->event);

    return dst;
}

template <>
void IPCCopyOut(IPCXrPollEvent* dst, const IPCXrPollEvent* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->event),
            reinterpret_cast<const XrBaseOutStructure*>(src->event)
            );
}

XrResult xrPollEvent (
    XrInstance                                   instance,
    XrEventDataBuffer*                        event)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_POLL_EVENT};

    IPCXrPollEvent args {instance, event};

    IPCXrPollEvent* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized);
    if(header->result == XR_SUCCESS) {
        CopyEventChainIntoBuffer(const_cast<const XrEventDataBaseHeader*>(reinterpret_cast<XrEventDataBaseHeader*>(argsSerialized->event)), event);
    }

    return header->result;
}

// xrGetSystemProperties ----------------------------------------------------

template <>
IPCXrGetSystemProperties* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrGetSystemProperties* src)
{
    auto dst = new(ipcbuf) IPCXrGetSystemProperties;

    dst->instance = src->instance;
	dst->system = src->system;

    dst->properties = reinterpret_cast<XrSystemProperties*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->properties), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->properties);

    return dst;
}

template <>
void IPCCopyOut(IPCXrGetSystemProperties* dst, const IPCXrGetSystemProperties* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->properties),
            reinterpret_cast<const XrBaseOutStructure*>(src->properties)
            );
}

XrResult xrGetSystemProperties (
    XrInstance                                   instance,
    XrSystemId                                   system,
    XrSystemProperties*                        properties)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_GET_SYSTEM_PROPERTIES};

    IPCXrGetSystemProperties args {instance, system, properties};

    IPCXrGetSystemProperties* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrLocateSpace ------------------------------------------------------------

template <>
IPCXrLocateSpace* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrLocateSpace* src)
{
    auto dst = new(ipcbuf) IPCXrLocateSpace;

    dst->space = src->space;
    dst->baseSpace = src->baseSpace;
    dst->time = src->time;

    dst->spaceLocation = reinterpret_cast<XrSpaceLocation*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->spaceLocation), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->spaceLocation);

    return dst;
}

template <>
void IPCCopyOut(IPCXrLocateSpace* dst, const IPCXrLocateSpace* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->spaceLocation),
            reinterpret_cast<const XrBaseOutStructure*>(src->spaceLocation)
            );
}

XrResult xrLocateSpace (
  XrSpace space,
  XrSpace baseSpace,
  XrTime time,
  XrSpaceLocation* location)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_LOCATE_SPACE};

    IPCXrLocateSpace args {space, baseSpace, time, location};

    IPCXrLocateSpace* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrGetD3D11GraphicsRequirementsKHR ----------------------------------------

template <>
IPCXrGetD3D11GraphicsRequirementsKHR* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrGetD3D11GraphicsRequirementsKHR* src)
{
    auto dst = new(ipcbuf) IPCXrGetD3D11GraphicsRequirementsKHR;

    dst->instance = src->instance;
    dst->systemId = src->systemId;

    dst->graphicsRequirements = reinterpret_cast<XrGraphicsRequirementsD3D11KHR*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->graphicsRequirements), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->graphicsRequirements);

    return dst;
}

template <>
void IPCCopyOut(IPCXrGetD3D11GraphicsRequirementsKHR* dst, const IPCXrGetD3D11GraphicsRequirementsKHR* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->graphicsRequirements),
            reinterpret_cast<const XrBaseOutStructure*>(src->graphicsRequirements)
            );
}

XrResult xrGetD3D11GraphicsRequirementsKHR  (
    XrInstance                                   instance,
    XrSystemId                                   system,
	XrGraphicsRequirementsD3D11KHR*                        graphicsRequirements)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_GET_D3D11_GRAPHICS_REQUIREMENTS_KHR};

    IPCXrGetD3D11GraphicsRequirementsKHR args {instance, system, graphicsRequirements};

    IPCXrGetD3D11GraphicsRequirementsKHR* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrCreateSwapchain --------------------------------------------------------

template <>
IPCXrCreateSwapchain* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrCreateSwapchain* src)
{
    auto dst = new(ipcbuf) IPCXrCreateSwapchain;

    dst->session = src->session;

    dst->createInfo = reinterpret_cast<const XrSwapchainCreateInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->createInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->createInfo);

    dst->swapchain = IPCSerializeNoCopy(ipcbuf, header, src->swapchain);
    header->addOffsetToPointer(ipcbuf.base, &dst->swapchain);

    dst->swapchainCount = IPCSerializeNoCopy(ipcbuf, header, src->swapchainCount);
    header->addOffsetToPointer(ipcbuf.base, &dst->swapchainCount);

    return dst;
}

template <>
void IPCCopyOut(IPCXrCreateSwapchain* dst, const IPCXrCreateSwapchain* src)
{
    IPCCopyOut(dst->swapchain, src->swapchain);
    IPCCopyOut(dst->swapchainCount, src->swapchainCount);
}

XrResult xrCreateSwapchain(
    XrSession                                   session,
    const XrSwapchainCreateInfo*                createInfo,
    XrSwapchain*                                swapchain)
{
    if(createInfo->sampleCount != 1) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    if(createInfo->mipCount != 1) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; 
    }
    if(createInfo->arraySize != 1) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; 
    }
    if((createInfo->usageFlags & ~(XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) != 0) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; 
    }
    if(createInfo->createFlags != 0) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; 
    }

    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_CREATE_SWAPCHAIN};

    int32_t swapchainCount;    
    IPCXrCreateSwapchain args {session, createInfo, swapchain, &swapchainCount};

    IPCXrCreateSwapchain* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    if(header->result == XR_SUCCESS) {

        auto& localSession = gLocalSessionMap[session];

        gLocalSwapchainMap[*swapchain] = LocalSwapchainPtr(new LocalSwapchain(*swapchain, swapchainCount, localSession->d3d11, createInfo));
    }

    return header->result;
}

// xrWaitFrame --------------------------------------------------------------

template <>
IPCXrWaitFrame* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrWaitFrame* src)
{
    auto dst = new(ipcbuf) IPCXrWaitFrame;

    dst->session = src->session;

    dst->frameWaitInfo = reinterpret_cast<const XrFrameWaitInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->frameWaitInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->frameWaitInfo);

    dst->frameState = reinterpret_cast<XrFrameState*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->frameState), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->frameState);

    return dst;
}

template <>
void IPCCopyOut(IPCXrWaitFrame* dst, const IPCXrWaitFrame* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->frameState),
            reinterpret_cast<const XrBaseOutStructure*>(src->frameState)
            );
}

XrResult xrWaitFrame(
    XrSession                                   session,
    const XrFrameWaitInfo*                      frameWaitInfo,
    XrFrameState*                               frameState)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_WAIT_FRAME};

    IPCXrWaitFrame args {session, frameWaitInfo, frameState};

    IPCXrWaitFrame* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrBeginFrame -------------------------------------------------------------

template <>
IPCXrBeginFrame* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrBeginFrame* src)
{
    auto dst = new(ipcbuf) IPCXrBeginFrame;

    dst->session = src->session;

    dst->frameBeginInfo = reinterpret_cast<const XrFrameBeginInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->frameBeginInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->frameBeginInfo);

    return dst;
}

XrResult xrBeginFrame(
    XrSession                                   session,
    const XrFrameBeginInfo*                     frameBeginInfo)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_BEGIN_FRAME};

    IPCXrBeginFrame args {session, frameBeginInfo};

    IPCXrBeginFrame* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    return header->result;
}

// xrEndFrame ---------------------------------------------------------------

template <>
IPCXrEndFrame* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrEndFrame* src)
{
    auto dst = new(ipcbuf) IPCXrEndFrame;

    dst->session = src->session;

    dst->frameEndInfo = reinterpret_cast<const XrFrameEndInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->frameEndInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->frameEndInfo);

    return dst;
}

XrResult xrEndFrame(
    XrSession                                  session,
    const XrFrameEndInfo*                      frameEndInfo)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_END_FRAME};

    IPCXrEndFrame args {session, frameEndInfo};

    IPCXrEndFrame* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    return header->result;
}

// xrAcquireSwapchainImage --------------------------------------------------

template <>
IPCXrAcquireSwapchainImage* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrAcquireSwapchainImage* src)
{
    auto dst = new(ipcbuf) IPCXrAcquireSwapchainImage;

    dst->swapchain = src->swapchain;

    dst->acquireInfo = reinterpret_cast<const XrSwapchainImageAcquireInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->acquireInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->acquireInfo);

    dst->index = IPCSerializeNoCopy(ipcbuf, header, src->index);
    header->addOffsetToPointer(ipcbuf.base, &dst->index);

    return dst;
}

template <>
void IPCCopyOut(IPCXrAcquireSwapchainImage* dst, const IPCXrAcquireSwapchainImage* src)
{
    IPCCopyOut(dst->index, src->index);
}

XrResult xrAcquireSwapchainImage(
    XrSwapchain                                 swapchain,
    const XrSwapchainImageAcquireInfo*          acquireInfo,
    uint32_t*                                   index)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_ACQUIRE_SWAPCHAIN_IMAGE};

    IPCXrAcquireSwapchainImage args {swapchain, acquireInfo, index};

    IPCXrAcquireSwapchainImage* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    gLocalSwapchainMap[swapchain]->acquired.push_back(*index);

    return header->result;
}

// xrWaitSwapchainImage -----------------------------------------------------

template <>
IPCXrWaitSwapchainImage* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrWaitSwapchainImage* src)
{
    auto dst = new(ipcbuf) IPCXrWaitSwapchainImage;

    dst->swapchain = src->swapchain;

    dst->waitInfo = reinterpret_cast<const XrSwapchainImageWaitInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->waitInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->waitInfo);

    dst->sourceImage = src->sourceImage;

    return dst;
}

XrResult xrWaitSwapchainImage(
    XrSwapchain                                 swapchain,
    const XrSwapchainImageWaitInfo*             waitInfo)
{
    auto& localSwapchain = gLocalSwapchainMap[swapchain];
    if(localSwapchain->waited) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_WAIT_SWAPCHAIN_IMAGE};

    uint32_t wasWaited = localSwapchain->acquired[0];
    HANDLE sharedResourceHandle = localSwapchain->swapchainHandles[wasWaited];
    IPCXrWaitSwapchainImage args {swapchain, waitInfo, sharedResourceHandle};

    IPCXrWaitSwapchainImage* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    localSwapchain->waited = true;
    IDXGIKeyedMutex* keyedMutex;
    CHECK(localSwapchain->swapchainTextures[wasWaited]->QueryInterface( __uuidof(IDXGIKeyedMutex), (LPVOID*)&keyedMutex));
    CHECK(keyedMutex->AcquireSync(KEYED_MUTEX_IPC_REMOTE, INFINITE));
    keyedMutex->Release();

    return header->result;
}

// xrReleaseSwapchainImage --------------------------------------------------

template <>
IPCXrReleaseSwapchainImage* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrReleaseSwapchainImage* src)
{
    auto dst = new(ipcbuf) IPCXrReleaseSwapchainImage;

    dst->swapchain = src->swapchain;

    dst->releaseInfo = reinterpret_cast<const XrSwapchainImageReleaseInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->releaseInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->releaseInfo);

    dst->sourceImage = src->sourceImage;

    return dst;
}

XrResult xrReleaseSwapchainImage(
    XrSwapchain                                 swapchain,
    const XrSwapchainImageReleaseInfo*             waitInfo)
{
    if(!gLocalSwapchainMap[swapchain]->waited)
        return XR_ERROR_CALL_ORDER_INVALID;

    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_RELEASE_SWAPCHAIN_IMAGE};

    uint32_t beingReleased = gLocalSwapchainMap[swapchain]->acquired[0];

    auto& localSwapchain = gLocalSwapchainMap[swapchain];

    localSwapchain->acquired.erase(localSwapchain->acquired.begin());

    IDXGIKeyedMutex* keyedMutex;
    CHECK(localSwapchain->swapchainTextures[beingReleased]->QueryInterface( __uuidof(IDXGIKeyedMutex), (LPVOID*)&keyedMutex));
    CHECK(keyedMutex->ReleaseSync(KEYED_MUTEX_IPC_HOST));
    keyedMutex->Release();

    HANDLE sharedResourceHandle = localSwapchain->swapchainHandles[beingReleased];
    IPCXrReleaseSwapchainImage args {swapchain, waitInfo, sharedResourceHandle};

    IPCXrReleaseSwapchainImage* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    gLocalSwapchainMap[swapchain]->waited = false;

    return header->result;
}

// xrDestroySession ---------------------------------------------------------

template <>
IPCXrDestroySession* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrDestroySession* src)
{
    auto dst = new(ipcbuf) IPCXrDestroySession;

    dst->session = src->session;

    return dst;
}

XrResult xrDestroySession(
    XrSession                                 session)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_DESTROY_SESSION};

    IPCXrDestroySession args {session};

    IPCXrDestroySession* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    if(header->result == XR_SUCCESS) {
        gLocalSessionMap.erase(session);
    }

    return header->result;
}

// xrCreateSession ----------------------------------------------------------

template <>
IPCXrCreateSession* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrCreateSession* src)
{
    auto dst = new(ipcbuf) IPCXrCreateSession;

    dst->instance = src->instance;

    dst->createInfo = reinterpret_cast<const XrSessionCreateInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->createInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->createInfo);

    dst->session = IPCSerializeNoCopy(ipcbuf, header, src->session);
    header->addOffsetToPointer(ipcbuf.base, &dst->session);

    return dst;
}

template <>
void IPCCopyOut(IPCXrCreateSession* dst, const IPCXrCreateSession* src)
{
    IPCCopyOut(dst->session, src->session);
}

XrResult xrCreateSession(
    XrInstance instance,
    const XrSessionCreateInfo* createInfo,
    XrSession* session)
{
    const XrBaseInStructure* p = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    const XrGraphicsBindingD3D11KHR* d3dbinding = nullptr;
    while(p) {
        if(p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            d3dbinding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p);
        }
        p = reinterpret_cast<const XrBaseInStructure*>(p->next);
    }

    if(!d3dbinding) {
        return XR_ERROR_GRAPHICS_DEVICE_INVALID; // ?
    }

    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_CREATE_SESSION};

    IPCXrCreateSession args {instance, createInfo, session};

    IPCXrCreateSession* argsSerialized = IPCSerialize(ipcbuf, header, &args);
    header->makePointersRelative(ipcbuf.base);

    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    if(header->result == XR_SUCCESS) {
        gLocalSessionMap[*session] = LocalSessionPtr(new LocalSession(*session, d3dbinding->device));
    }

    return header->result;
}

// xrGetSystem --------------------------------------------------------------

template <>
IPCXrGetSystem* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrGetSystem* src)
{
    auto dst = new(ipcbuf) IPCXrGetSystem;

    dst->instance = src->instance;

    dst->getInfo = reinterpret_cast<const XrSystemGetInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->getInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->getInfo);

    dst->systemId = IPCSerializeNoCopy(ipcbuf, header, src->systemId);
    header->addOffsetToPointer(ipcbuf.base, &dst->systemId);

    return dst;
}

template <>
void IPCCopyOut(IPCXrGetSystem* dst, const IPCXrGetSystem* src)
{
    IPCCopyOut(dst->systemId, src->systemId);
}

XrResult xrGetSystem(
    XrInstance instance,
    const XrSystemGetInfo* getInfo,
    XrSystemId* systemId)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_GET_SYSTEM};

    IPCXrGetSystem args {instance, getInfo, systemId};

    IPCXrGetSystem* argsSerialized = IPCSerialize(ipcbuf, header, &args);
    header->makePointersRelative(ipcbuf.base);

    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrCreateReferenceSpace ---------------------------------------------------

template <>
IPCXrCreateReferenceSpace* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrCreateReferenceSpace* src)
{
    auto dst = new(ipcbuf) IPCXrCreateReferenceSpace;

    dst->session = src->session;

    dst->createInfo = reinterpret_cast<const XrReferenceSpaceCreateInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->createInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->createInfo);

    dst->space = IPCSerializeNoCopy(ipcbuf, header, src->space);
    header->addOffsetToPointer(ipcbuf.base, &dst->space);

    return dst;
}

template <>
void IPCCopyOut(IPCXrCreateReferenceSpace* dst, const IPCXrCreateReferenceSpace* src)
{
    IPCCopyOut(dst->space, src->space);
}

XrResult xrCreateReferenceSpace(
    XrSession                                   session,
    const XrReferenceSpaceCreateInfo*           createInfo,
    XrSpace*                                    space)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_CREATE_REFERENCE_SPACE};

    IPCXrCreateReferenceSpace args {session, createInfo, space};

    IPCXrCreateReferenceSpace* argsSerialized = IPCSerialize(ipcbuf, header, &args);
    header->makePointersRelative(ipcbuf.base);

    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrCreateInstance -----------------------------------------------------------

template <>
IPCXrCreateInstance* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrCreateInstance* src)
{
    IPCXrCreateInstance *dst = new(ipcbuf) IPCXrCreateInstance;

    dst->createInfo = reinterpret_cast<const XrInstanceCreateInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->createInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->createInfo);

    dst->instance = IPCSerializeNoCopy(ipcbuf, header, src->instance);
    header->addOffsetToPointer(ipcbuf.base, &dst->instance);

    dst->remoteProcessId = src->remoteProcessId;

    dst->hostProcessId = IPCSerializeNoCopy(ipcbuf, header, src->hostProcessId);
    header->addOffsetToPointer(ipcbuf.base, &dst->hostProcessId);

    return dst;
}

template <>
void IPCCopyOut(IPCXrCreateInstance* dst, const IPCXrCreateInstance* src)
{
    IPCCopyOut(dst->instance, src->instance);
    IPCCopyOut(dst->hostProcessId, src->hostProcessId);
}

XrResult xrCreateInstance(
    const XrInstanceCreateInfo*                 createInfo,
    XrInstance *instance)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    auto header = new(ipcbuf) IPCXrHeader{IPC_XR_CREATE_INSTANCE};


    IPCXrCreateInstance args {createInfo, instance, GetCurrentProcessId(), &gHostProcessId};
    IPCXrCreateInstance *argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);

    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrDestroySwapchain -------------------------------------------------------

template <>
IPCXrDestroySwapchain* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrDestroySwapchain* src)
{
    auto dst = new(ipcbuf) IPCXrDestroySwapchain;

    dst->swapchain = src->swapchain;

    return dst;
}

XrResult xrDestroySwapchain(
    XrSwapchain                                 swapchain)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_DESTROY_SWAPCHAIN};

    IPCXrDestroySwapchain args {swapchain};

    IPCXrDestroySwapchain* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    gLocalSwapchainMap.erase(gLocalSwapchainMap.find(swapchain));

    return header->result;
}

// xrDestroySpace -----------------------------------------------------------

template <>
IPCXrDestroySpace* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrDestroySpace* src)
{
    auto dst = new(ipcbuf) IPCXrDestroySpace;

    dst->space = src->space;

    return dst;
}

XrResult xrDestroySpace(
    XrSpace                                     space)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_DESTROY_SPACE};

    IPCXrDestroySpace args {space};

    IPCXrDestroySpace* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    return header->result;
}

// xrRequestExitSession -------------------------------------------------------------

template <>
IPCXrRequestExitSession* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrRequestExitSession* src)
{
    auto dst = new(ipcbuf) IPCXrRequestExitSession;

    dst->session = src->session;

    return dst;
}

XrResult xrRequestExitSession(
    XrSession                                   session)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_REQUEST_EXIT_SESSION};

    IPCXrRequestExitSession args {session};

    IPCXrRequestExitSession* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    return header->result;
}

// xrEndSession -------------------------------------------------------------

template <>
IPCXrEndSession* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrEndSession* src)
{
    auto dst = new(ipcbuf) IPCXrEndSession;

    dst->session = src->session;

    return dst;
}

XrResult xrEndSession(
    XrSession                                   session)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_END_SESSION};

    IPCXrEndSession args {session};

    IPCXrEndSession* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();

    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    return header->result;
}

// xrBeginSession -----------------------------------------------------------

template <>
IPCXrBeginSession* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrBeginSession* src)
{
    auto dst = new(ipcbuf) IPCXrBeginSession;

    dst->session = src->session;

    dst->beginInfo = reinterpret_cast<const XrSessionBeginInfo*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->beginInfo), COPY_EVERYTHING));
    header->addOffsetToPointer(ipcbuf.base, &dst->beginInfo);

    return dst;
}

XrResult xrBeginSession(
    XrSession                                   session,
    const XrSessionBeginInfo*                   beginInfo)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_BEGIN_SESSION};

    IPCXrBeginSession args {session, beginInfo};

    IPCXrBeginSession* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    // IPCCopyOut(&args, argsSerialized); // Nothing to copy back out

    return header->result;
}

// xrGetViewConfigurationProperties -----------------------------------------

template <>
IPCXrGetViewConfigurationProperties* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrGetViewConfigurationProperties* src)
{
    auto dst = new(ipcbuf) IPCXrGetViewConfigurationProperties;

    dst->instance = src->instance;
    dst->systemId = src->systemId;
    dst->viewConfigurationType = src->viewConfigurationType;

    dst->configurationProperties = reinterpret_cast<XrViewConfigurationProperties*>(IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->configurationProperties), COPY_ONLY_TYPE_NEXT));
    header->addOffsetToPointer(ipcbuf.base, &dst->configurationProperties);

    return dst;
}

template <>
void IPCCopyOut(IPCXrGetViewConfigurationProperties* dst, const IPCXrGetViewConfigurationProperties* src)
{
    IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(dst->configurationProperties),
            reinterpret_cast<const XrBaseOutStructure*>(src->configurationProperties)
            );
}

XrResult xrGetViewConfigurationProperties(
    XrInstance                                  instance,
    XrSystemId                                  systemId,
    XrViewConfigurationType                     viewConfigurationType,
    XrViewConfigurationProperties*              configurationProperties)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_GET_VIEW_CONFIGURATION_PROPERTIES};

    IPCXrGetViewConfigurationProperties args {instance, systemId, viewConfigurationType, configurationProperties};

    IPCXrGetViewConfigurationProperties* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrEnumerateInstanceExtensionProperties -----------------------------------

template <>
IPCXrEnumerateInstanceExtensionProperties* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrEnumerateInstanceExtensionProperties* src)
{
    auto dst = new(ipcbuf) IPCXrEnumerateInstanceExtensionProperties;

    if(src->layerName) {
        char *dstLayerName = new(ipcbuf) char[strlen(src->layerName) + 1];
        dst->layerName = dstLayerName;
        strncpy_s(dstLayerName, strlen(src->layerName) + 1, src->layerName, strlen(src->layerName) + 1);
    } else {
        dst->layerName = nullptr;
    }
    header->addOffsetToPointer(ipcbuf.base, &dst->layerName);

    dst->propertyCapacityInput = src->propertyCapacityInput;

    dst->propertyCountOutput = IPCSerializeNoCopy(ipcbuf, header, src->propertyCountOutput);
    header->addOffsetToPointer(ipcbuf.base, &dst->propertyCountOutput);

    if(dst->propertyCapacityInput > 0) {
        dst->properties = new(ipcbuf) XrExtensionProperties[dst->propertyCapacityInput];
        header->addOffsetToPointer(ipcbuf.base, &dst->properties);
        for(uint32_t i = 0; i < dst->propertyCapacityInput; i++) {
            dst->properties[i].type = src->properties[i].type;
            dst->properties[i].next = IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->properties[i].next), COPY_ONLY_TYPE_NEXT);
            header->addOffsetToPointer(ipcbuf.base, &dst->properties[i].next);
        }
    }

    return dst;
}

template <>
void IPCCopyOut(IPCXrEnumerateInstanceExtensionProperties* dst, const IPCXrEnumerateInstanceExtensionProperties* src)
{
    IPCCopyOut(dst->propertyCountOutput, src->propertyCountOutput);
    uint32_t toCopy = std::min(src->propertyCapacityInput, (uint32_t)*src->propertyCountOutput);
    for(uint32_t i = 0; i < toCopy; i++) {
        IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(&dst->properties[i]),
            reinterpret_cast<const XrBaseOutStructure*>(&src->properties[i])
            );
    }
}

XrResult xrEnumerateInstanceExtensionProperties(
    const char *                                  layerName,
    uint32_t                                    propertyCapacityInput,
    uint32_t*                                   propertyCountOutput,
    XrExtensionProperties*                    properties)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES};

    IPCXrEnumerateInstanceExtensionProperties args {layerName, propertyCapacityInput, propertyCountOutput, properties};

    IPCXrEnumerateInstanceExtensionProperties* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrEnumerateViewConfigurationViews ----------------------------------------

template <>
IPCXrEnumerateViewConfigurationViews* IPCSerialize(IPCBuffer& ipcbuf, IPCXrHeader* header, const IPCXrEnumerateViewConfigurationViews* src)
{
    auto dst = new(ipcbuf) IPCXrEnumerateViewConfigurationViews;

    dst->instance = src->instance;
    dst->systemId = src->systemId;
    dst->viewConfigurationType = src->viewConfigurationType;
    dst->viewCapacityInput = src->viewCapacityInput;

    dst->viewCountOutput = IPCSerializeNoCopy(ipcbuf, header, src->viewCountOutput);
    header->addOffsetToPointer(ipcbuf.base, &dst->viewCountOutput);

    if(dst->viewCapacityInput > 0) {
        dst->views = new(ipcbuf) XrViewConfigurationView[dst->viewCapacityInput];
        header->addOffsetToPointer(ipcbuf.base, &dst->views);
        for(uint32_t i = 0; i < dst->viewCapacityInput; i++) {
            dst->views[i].type = src->views[i].type;
            dst->views[i].next = IPCSerialize(ipcbuf, header, reinterpret_cast<const XrBaseInStructure*>(src->views[i].next), COPY_ONLY_TYPE_NEXT);
            header->addOffsetToPointer(ipcbuf.base, &dst->views[i].next);
        }
    }

    return dst;
}

template <>
void IPCCopyOut(IPCXrEnumerateViewConfigurationViews* dst, const IPCXrEnumerateViewConfigurationViews* src)
{
    IPCCopyOut(dst->viewCountOutput, src->viewCountOutput);
    uint32_t toCopy = std::min(src->viewCapacityInput, (uint32_t)*src->viewCountOutput);
    for(uint32_t i = 0; i < toCopy; i++) {
        IPCCopyOut(
            reinterpret_cast<XrBaseOutStructure*>(&dst->views[i]),
            reinterpret_cast<const XrBaseOutStructure*>(&src->views[i])
            );
    }
}

XrResult xrEnumerateViewConfigurationViews(
    XrInstance                                  instance,
    XrSystemId                                  systemId,
    XrViewConfigurationType                     viewConfigurationType,
    uint32_t                                    viewCapacityInput,
    uint32_t*                                   viewCountOutput,
    XrViewConfigurationView*                    views)
{
    IPCBuffer ipcbuf = IPCGetBuffer();
    IPCXrHeader* header = new(ipcbuf) IPCXrHeader{IPC_XR_ENUMERATE_VIEW_CONFIGURATION_VIEWS};

    IPCXrEnumerateViewConfigurationViews args {instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views};

    IPCXrEnumerateViewConfigurationViews* argsSerialized = IPCSerialize(ipcbuf, header, &args);

    header->makePointersRelative(ipcbuf.base);
    IPCFinishRemoteRequest();
    IPCWaitResult result = IPCWaitForHostResponse();
    if(result == IPC_REMOTE_PROCESS_TERMINATED) {
        // Remote process died for some reason.
        outputDebugF("The Host process was terminated.\n");
        return XR_ERROR_RUNTIME_FAILURE;
    } else if(result == IPC_WAIT_ERROR) {
        // Unknown exception on wait...  Note for debugging purposes
        outputDebugF("Waiting on remote process failed without indicating the remote process died, at %s:%d\n", __FILE__, __LINE__);
        return XR_ERROR_RUNTIME_FAILURE;
    } // else IPC_HOST_RESPONSE_READY is what we expect.

    header->makePointersAbsolute(ipcbuf.base);

    IPCCopyOut(&args, argsSerialized);

    return header->result;
}

// xrEnumerateSwapchainImages -----------------------------------------------
// (Not serialized, handled locally)

XrResult xrEnumerateSwapchainImages(
        XrSwapchain swapchain,
        uint32_t imageCapacityInput,
        uint32_t* imageCountOutput,
        XrSwapchainImageBaseHeader* images)
{
    auto& localSwapchain = gLocalSwapchainMap[swapchain];

    if(imageCapacityInput == 0) {
        *imageCountOutput = (uint32_t)localSwapchain->swapchainTextures.size();
        return XR_SUCCESS;
    }

    // (If storage is provided) Give back the "local" swapchainimages (rendertarget) for rendering
    auto sci = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    uint32_t toWrite = std::min(imageCapacityInput, (uint32_t)localSwapchain->swapchainTextures.size());
    for(uint32_t i = 0; i < toWrite; i++) {
        sci[i].texture = localSwapchain->swapchainTextures[i];
    }

    *imageCountOutput = toWrite;

    return XR_SUCCESS;
}

// xrGetInstanceProcAddr

XrResult xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    if (0 == strcmp(name, "xrGetD3D11GraphicsRequirementsKHR")) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(xrGetD3D11GraphicsRequirementsKHR);
    } else {
        *function = nullptr;
    }
    // Really should do all the base ones, too?

    // If we setup the function, just return
    if (*function) {
        return XR_SUCCESS;
    }

    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

