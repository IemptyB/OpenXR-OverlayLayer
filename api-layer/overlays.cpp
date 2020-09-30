// Copyright (c) 2017-2020 The Khronos Group Inc.
// Copyright (c) 2017-2019 Valve Corporation
// Copyright (c) 2017-2020 LunarG, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: Mark Young <marky@lunarg.com>
// Author: Dave Houlton <daveh@lunarg.com>
// Author: Brad Grantham <brad@lunarg.com>
//

#include "loader_interfaces.h"
#include "platform_utils.hpp"

#include "overlays.h"

#include "xr_generated_overlays.hpp"
#include "xr_generated_dispatch_table.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GNUC__) && __GNUC__ >= 4
#define LAYER_EXPORT __attribute__((visibility("default")))
#elif defined(_WIN32)
#define LAYER_EXPORT __declspec(dllexport)
#else
#define LAYER_EXPORT
#endif

const char *kOverlayLayerName = "xr_extx_overlay";

const std::set<HandleTypePair> OverlaysLayerNoObjectInfo = {};


void OverlaysLayerLogMessage(XrInstance instance,
                         XrDebugUtilsMessageSeverityFlagsEXT message_severity, const char* command_name,
                         const std::set<HandleTypePair>& objects_info, const char* message)
{
    // If we have instance information, see if we need to log this information out to a debug messenger
    // callback.
    if(instance != XR_NULL_HANDLE) {

        OverlaysLayerXrInstanceHandleInfo& instanceInfo = gOverlaysLayerXrInstanceToHandleInfo.at(instance);

        // To be a little more performant, check all messenger's
        // messageSeverities and messageTypes to make sure we will call at
        // least one

        /* XXX TBD !instanceInfo.debug_data.Empty() */

        if (!instanceInfo.debugUtilsMessengers.empty()) {

            // Setup our callback data once
            XrDebugUtilsMessengerCallbackDataEXT callback_data = {};
            callback_data.type = XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
            callback_data.messageId = "Overlays API Layer";
            callback_data.functionName = command_name;
            callback_data.message = message;

#if 0
            // TBD
            NamesAndLabels names_and_labels;
            std::vector<XrSdkLogObjectInfo> objects;
            objects.reserve(objects_info.size());
            std::transform(objects_info.begin(), objects_info.end(), std::back_inserter(objects),
                           [](GenValidUsageXrObjectInfo const &info) {
                               return XrSdkLogObjectInfo{info.handle, info.type};
                           });
            names_and_labels = instance_info->debug_data.PopulateNamesAndLabels(std::move(objects));
            names_and_labels.PopulateCallbackData(callback_data);
#endif

            // Loop through all active messengers and give each a chance to output information
            for (const auto &messenger : instanceInfo.debugUtilsMessengers) {

                std::unique_lock<std::mutex> mlock(gOverlaysLayerXrInstanceToHandleInfoMutex);
                XrDebugUtilsMessengerCreateInfoEXT *messenger_create_info = gOverlaysLayerXrDebugUtilsMessengerEXTToHandleInfo.at(messenger).createInfo;
                mlock.unlock();

                // If a callback exists, and the message is of a type this callback cares about, call it.
                if (nullptr != messenger_create_info->userCallback &&
                    0 != (messenger_create_info->messageSeverities & message_severity) &&
                    0 != (messenger_create_info->messageTypes & XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)) {

                    XrBool32 ret_val = messenger_create_info->userCallback(message_severity,
                                                                           XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                                                                           &callback_data, messenger_create_info->userData);
                }
            }
        } else {
            if(command_name) {
                OutputDebugStringA(fmt("Overlays API Layer: %s, %s\n", command_name, message).c_str());
            } else {
                OutputDebugStringA(fmt("Overlays API Layer: %s\n", message).c_str());
            }
        }
    } else {
        if(command_name) {
            OutputDebugStringA(fmt("Overlays API Layer: %s, %s\n", command_name, message).c_str());
        } else {
            OutputDebugStringA(fmt("Overlays API Layer: %s\n", message).c_str());
        }
    }
}

void OverlaysLayerLogMessage(XrDebugUtilsMessageSeverityFlagsEXT message_severity, const char* command_name,
                         const std::set<HandleTypePair>& objects_info, const char* message)
{
    OverlaysLayerLogMessage(XR_NULL_HANDLE, message_severity, command_name, objects_info, message);
}



XrResult OverlaysLayerXrCreateInstance(const XrInstanceCreateInfo * /*info*/, XrInstance * /*instance*/)
{
    return XR_SUCCESS;
}


XrResult OverlaysLayerXrCreateApiLayerInstance(const XrInstanceCreateInfo *instanceCreateInfo,
        const struct XrApiLayerCreateInfo *apiLayerInfo, XrInstance *instance)
{
    PFN_xrGetInstanceProcAddr next_get_instance_proc_addr = nullptr;
    PFN_xrCreateApiLayerInstance next_create_api_layer_instance = nullptr;
    XrApiLayerCreateInfo new_api_layer_info = {};

    // Validate the API layer info and next API layer info structures before we try to use them
    if (!apiLayerInfo ||
        XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO != apiLayerInfo->structType ||
        XR_API_LAYER_CREATE_INFO_STRUCT_VERSION > apiLayerInfo->structVersion ||
        sizeof(XrApiLayerCreateInfo) > apiLayerInfo->structSize ||
        !apiLayerInfo->nextInfo ||
        XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO != apiLayerInfo->nextInfo->structType ||
        XR_API_LAYER_NEXT_INFO_STRUCT_VERSION > apiLayerInfo->nextInfo->structVersion ||
        sizeof(XrApiLayerNextInfo) > apiLayerInfo->nextInfo->structSize ||
        0 != strcmp(kOverlayLayerName, apiLayerInfo->nextInfo->layerName) ||
        nullptr == apiLayerInfo->nextInfo->nextGetInstanceProcAddr ||
        nullptr == apiLayerInfo->nextInfo->nextCreateApiLayerInstance) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    // Copy the contents of the layer info struct, but then move the next info up by
    // one slot so that the next layer gets information.
    memcpy(&new_api_layer_info, apiLayerInfo, sizeof(XrApiLayerCreateInfo));
    new_api_layer_info.nextInfo = apiLayerInfo->nextInfo->next;

    // Get the function pointers we need
    next_get_instance_proc_addr = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;
    next_create_api_layer_instance = apiLayerInfo->nextInfo->nextCreateApiLayerInstance;

    // Create the instance
    XrInstance returned_instance = *instance;
    XrResult result = next_create_api_layer_instance(instanceCreateInfo, &new_api_layer_info, &returned_instance);
    *instance = returned_instance;

    // Create the dispatch table to the next levels
    auto *next_dispatch = new XrGeneratedDispatchTable();
    GeneratedXrPopulateDispatchTable(next_dispatch, returned_instance, next_get_instance_proc_addr);

    std::unique_lock<std::mutex> mlock(gOverlaysLayerXrInstanceToHandleInfoMutex);
	gOverlaysLayerXrInstanceToHandleInfo.emplace(*instance, next_dispatch);
    gOverlaysLayerXrInstanceToHandleInfo.at(*instance).createInfo = reinterpret_cast<XrInstanceCreateInfo*>(CopyXrStructChainWithMalloc(*instance, instanceCreateInfo));

    return result;
}

XrResult OverlaysLayerXrDestroyInstance(XrInstance instance) {
	std::unique_lock<std::mutex> mlock(gOverlaysLayerXrInstanceToHandleInfoMutex);
	OverlaysLayerXrInstanceHandleInfo& instanceInfo = gOverlaysLayerXrInstanceToHandleInfo.at(instance);
	XrGeneratedDispatchTable *next_dispatch = instanceInfo.downchain;
	instanceInfo.Destroy();
	mlock.unlock();

    next_dispatch->DestroyInstance(instance);

    return XR_SUCCESS;
}

NegotiationChannels gNegotiationChannels;

bool gHaveMainSessionActive = false;
XrInstance gMainSessionInstance;
HANDLE gMainMutexHandle; // Held by Main for duration of operation as Main Session
HANDLE gMainOverlayMutexHandle; // Held when Main and MainAsOverlay functions need to run exclusively

// Both main and overlay processes call this function, which creates/opens
// the negotiation mutex, shmem, and semaphores.
bool OpenNegotiationChannels(XrInstance instance, NegotiationChannels &ch)
{
    ch.instance = instance;
    ch.mutexHandle = CreateMutexA(NULL, TRUE, NegotiationChannels::mutexName);
    if (ch.mutexHandle == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not initialize the negotiation mutex: CreateMutex error was %d (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false;
    }

    ch.shmemHandle = CreateFileMappingA( 
        INVALID_HANDLE_VALUE,   // use sys paging file instead of an existing file
        NULL,                   // default security attributes
        PAGE_READWRITE,         // read/write access
        0,                      // size: high 32-bits
        NegotiationChannels::shmemSize,         // size: low 32-bits
        NegotiationChannels::shmemName);        // name of map object

    if (ch.shmemHandle == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession",
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not initialize the negotiation shmem: CreateFileMappingA error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false; 
    }

    // Get a pointer to the file-mapped shared memory, read/write
    ch.params = reinterpret_cast<NegotiationParams*>(MapViewOfFile(ch.shmemHandle, FILE_MAP_WRITE, 0, 0, 0));
    if (!ch.params) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession",
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not get the negotiation shmem: MapViewOfFile error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false; 
    }

    ch.overlayWaitSema = CreateSemaphoreA(nullptr, 0, 1, NegotiationChannels::overlayWaitSemaName);
    if(ch.overlayWaitSema == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession",
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not create negotiation overlay wait sema: CreateSemaphore error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false;
    }

    ch.mainWaitSema = CreateSemaphoreA(nullptr, 0, 1, NegotiationChannels::mainWaitSemaName);
    if(ch.mainWaitSema == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession",
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not create negotiation main wait sema: CreateSemaphore error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false;
    }

    return true;
}

bool OpenRPCChannels(XrInstance instance, DWORD overlayProcessId, RPCChannels& ch)
{
    ch.instance = instance;
    ch.mutexHandle = CreateMutexA(NULL, TRUE, fmt(RPCChannels::mutexNameTemplate, overlayProcessId).c_str());
    if (ch.mutexHandle == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "no function", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not initialize the RPC mutex: CreateMutex error was %d (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false;
    }

    ch.shmemHandle = CreateFileMappingA( 
        INVALID_HANDLE_VALUE,   // use sys paging file instead of an existing file
        NULL,                   // default security attributes
        PAGE_READWRITE,         // read/write access
        0,                      // size: high 32-bits
        RPCChannels::shmemSize,         // size: low 32-bits
        fmt(RPCChannels::shmemNameTemplate, overlayProcessId).c_str());        // name of map object

    if (ch.shmemHandle == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "no function", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not initialize the RPC shmem: CreateFileMappingA error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false; 
    }

    // Get a pointer to the file-mapped shared memory, read/write
    ch.shmem = MapViewOfFile(ch.shmemHandle, FILE_MAP_WRITE, 0, 0, 0);
    if (ch.shmem == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not get the RPC shmem: MapViewOfFile error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false; 
    }

    ch.overlayRequestSema = CreateSemaphoreA(nullptr, 0, 1, fmt(RPCChannels::overlayRequestSemaNameTemplate, overlayProcessId).c_str());
    if(ch.overlayRequestSema == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not create RPC overlay request sema: CreateSemaphore error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false;
    }

    ch.mainResponseSema = CreateSemaphoreA(nullptr, 0, 1, fmt(RPCChannels::mainResponseSemaNameTemplate, overlayProcessId).c_str());
    if(ch.mainResponseSema == NULL) {
        DWORD lastError = GetLastError();
        LPVOID messageBuf;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not create RPC main response sema: CreateSemaphore error was %08X (%s)\n", lastError, messageBuf).c_str());
        LocalFree(messageBuf);
        return false;
    }

    return true;
}


DWORD WINAPI MainRPCThreadBody(void *param)
{
    RPCChannels *channels = reinterpret_cast<RPCChannels*>(param);

    DebugBreak();
#if 0
    bool connectionLost = false;
    do {
        IPCWaitResult result;
        result = IPCWaitForRemoteRequestOrTermination();

        if(result == IPC_REMOTE_PROCESS_TERMINATED) {

            if(gMainSession && gMainSession->overlaySession) { // Might have LOST SESSION
                ScopedMutex scopedMutex(gOverlayCallMutex, INFINITE, "overlay layer command mutex", __FILE__, __LINE__);
                gMainSession->overlaySession->swapchainMap.clear();
                gMainSession->ClearOverlayLayers();
            }
            connectionLost = true;

        } else if(result == IPC_WAIT_ERROR) {

            if(gMainSession && gMainSession->overlaySession) { // Might have LOST SESSION
                ScopedMutex scopedMutex(gOverlayCallMutex, INFINITE, "overlay layer command mutex", __FILE__, __LINE__);
                gMainSession->overlaySession->swapchainMap.clear();
                gMainSession->ClearOverlayLayers();
            }
            connectionLost = true;
            OutputDebugStringA("**OVERLAY** IPC Wait Error\n");

        } else {

            IPCBuffer ipcbuf = IPCGetBuffer();
            IPCXrHeader *hdr = ipcbuf.getAndAdvance<IPCXrHeader>();

            hdr->makePointersAbsolute(ipcbuf.base);

            connectionLost = ProcessRemoteRequestAndReturnConnectionLost(ipcbuf, hdr);

            hdr->makePointersRelative(ipcbuf.base);

            IPCFinishHostResponse();
        }

        if(connectionLost && gMainSession && gMainSession->overlaySession) {
            gMainSession->DestroyOverlaySession();
        }

    } while(!connectionLost && !gMainInstanceContext.exitIPCLoop);
#endif
	return 0;
}

std::unordered_map<DWORD, ConnectionToOverlay> gConnectionsToOverlayByProcessId;

DWORD WINAPI MainNegotiateThreadBody(void*)
{
    DWORD result;
    HANDLE handles[2];
    handles[0] = gNegotiationChannels.mainNegotiateThreadStop;
    handles[1] = gNegotiationChannels.mainWaitSema;

    while(1) {
        // Signal that one overlay app may attempt to connect
        ReleaseSemaphore(gNegotiationChannels.overlayWaitSema, 1, nullptr);

        do {
            result = WaitForMultipleObjects(2, handles, FALSE, NegotiationChannels::negotiationWaitMillis);
        } while(result == WAIT_TIMEOUT);

        if(result == WAIT_OBJECT_0 + 0) {

            // Main process has signaled us to stop, probably Session was destroyed.
            return 0;

        } else if(result != WAIT_OBJECT_0 + 1) {

            // WAIT_FAILED
            DWORD lastError = GetLastError();
            LPVOID messageBuf;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &messageBuf, 0, nullptr);
            OverlaysLayerLogMessage(gNegotiationChannels.instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "no function", 
                OverlaysLayerNoObjectInfo, fmt("FATAL: Could not wait on negotiation sema sema: WaitForMultipleObjects error was %08X (%s)\n", lastError, messageBuf).c_str());
            // XXX need way to signal main process that thread errored unexpectedly
            LocalFree(messageBuf);
            return 0;
        }

        if(gNegotiationChannels.params->status != NegotiationParams::SUCCESS) {

            OverlaysLayerLogMessage(gNegotiationChannels.instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, "no function",
                OverlaysLayerNoObjectInfo, fmt("WARNING: the Overlay API Layer in the overlay app has a different version (%u) than in the main app (%u), connection rejected.\n", gNegotiationChannels.params->overlayLayerBinaryVersion, gNegotiationChannels.params->mainLayerBinaryVersion).c_str());

        } else {
            DWORD overlayProcessId = gNegotiationChannels.params->overlayProcessId;
            RPCChannels channels;

            if(!OpenRPCChannels(gNegotiationChannels.instance, overlayProcessId, channels)) {
                OverlaysLayerLogMessage(gNegotiationChannels.instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, "no function",
                    OverlaysLayerNoObjectInfo, fmt("WARNING: couldn't open RPC channels to overlay app, connection rejected.\n").c_str());
            } else {

                /* XXX save off negotiation parameters because they are systemwide ?? */

                RPCChannels *threadChannels = new RPCChannels;
                *threadChannels = channels; // thread frees
                DWORD threadId;
                HANDLE receiverThread = CreateThread(nullptr, 0, MainRPCThreadBody, threadChannels, 0, &threadId);
                gConnectionsToOverlayByProcessId.emplace(std::piecewise_construct,
					std::forward_as_tuple(overlayProcessId),
					std::forward_as_tuple(channels, receiverThread, threadId));
            }
        }
    }
}

bool CreateMainSessionNegotiateThread(XrInstance instance)
{
    if(!OpenNegotiationChannels(instance, gNegotiationChannels)) {
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession",
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not create overlays negotiation channels\n").c_str());
        return false;
    }

    DWORD waitresult = WaitForSingleObject(gMainMutexHandle, NegotiationChannels::mutexWaitMillis);
    if (waitresult == WAIT_TIMEOUT) {
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession",
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not take main mutex sema; is there another main app running?\n").c_str());
        return false;
    }

    gNegotiationChannels.params->mainProcessId = GetCurrentProcessId();
    gNegotiationChannels.params->mainLayerBinaryVersion = gLayerBinaryVersion;
    gNegotiationChannels.mainNegotiateThreadStop = CreateEventA(nullptr, false, false, nullptr);
    gNegotiationChannels.mainThread = CreateThread(nullptr, 0, MainNegotiateThreadBody, nullptr, 0, &gNegotiationChannels.mainThreadId);

    return true;
}

std::unique_ptr<ConnectionToMain> gConnectionToMain;

XrResult OverlaysLayerCreateSessionMain(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
    bool result = CreateMainSessionNegotiateThread(instance);
    if(!result) {
        OverlaysLayerLogMessage(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "xrCreateSession", 
            OverlaysLayerNoObjectInfo, fmt("FATAL: Could not initialize the Main App listener thread.\n").c_str());
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    
    std::unique_lock<std::mutex> mlock(gOverlaysLayerXrInstanceToHandleInfoMutex);
    OverlaysLayerXrInstanceHandleInfo& instanceInfo = gOverlaysLayerXrInstanceToHandleInfo.at(instance);

	XrResult xrresult = instanceInfo.downchain->CreateSession(instance, createInfo, session);

    // XXX create unique local id, place as that instead of created handle
    gOverlaysLayerXrSessionToHandleInfo.emplace(std::piecewise_construct, std::forward_as_tuple(*session), std::forward_as_tuple(instance, instance, instanceInfo.downchain));

    mlock.unlock();
    gHaveMainSessionActive = true;

    return xrresult;
}

XrResult OverlaysLayerCreateSessionMainAsOverlay(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
	return XR_SUCCESS;
}

XrResult OverlaysLayerCreateSessionOverlay(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
	XrResult result;

    std::unique_lock<std::mutex> mlock(gOverlaysLayerXrInstanceToHandleInfoMutex);
    OverlaysLayerXrInstanceHandleInfo& instanceInfo = gOverlaysLayerXrInstanceToHandleInfo.at(instance);

        // connect-to-main
        // create session
        // store proxy
	result = XR_SUCCESS; // XXX

    gOverlaysLayerXrSessionToHandleInfo.emplace(std::piecewise_construct, std::forward_as_tuple(*session), std::forward_as_tuple(instance, instance, instanceInfo.downchain));
    // XXX create unique local id, return that

    mlock.unlock();

    return result;
}

XrResult OverlaysLayerCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
	XrResult result;

    const XrBaseInStructure* p = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    const XrSessionCreateInfoOverlayEXTX* cio = nullptr;
    const XrGraphicsBindingD3D11KHR* d3dbinding = nullptr;
    while(p) {
        if(p->type == XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX) {
            cio = reinterpret_cast<const XrSessionCreateInfoOverlayEXTX*>(p);
        }
        if(false) {
            // XXX save off requested API in Overlay, match against Main API
            // XXX save off requested API in Main, match against Overlay API
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
        }
        p = reinterpret_cast<const XrBaseInStructure*>(p->next);
    }

    if(!cio) {
        result = OverlaysLayerCreateSessionMain(instance, createInfo, session);
    } else {
        result = OverlaysLayerCreateSessionOverlay(instance, createInfo, session);
    }

    return result;
}

extern "C" {

// Function used to negotiate an interface betewen the loader and an API layer.  Each library exposing one or
// more API layers needs to expose at least this function.
XrResult LAYER_EXPORT XRAPI_CALL Overlays_xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *loaderInfo,
                                                                    const char* apiLayerName,
                                                                    XrNegotiateApiLayerRequest *apiLayerRequest)
{
    if (apiLayerName)
    {
        if (0 != strncmp(kOverlayLayerName, apiLayerName, strnlen_s(kOverlayLayerName, XR_MAX_API_LAYER_NAME_SIZE)))
        {
            return XR_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (!loaderInfo ||
        !apiLayerRequest ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxApiVersion < XR_CURRENT_API_VERSION ||
        loaderInfo->minApiVersion > XR_CURRENT_API_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(OverlaysLayerXrGetInstanceProcAddr);
    apiLayerRequest->createApiLayerInstance = reinterpret_cast<PFN_xrCreateApiLayerInstance>(OverlaysLayerXrCreateApiLayerInstance);

    return XR_SUCCESS;
}

}  // extern "C"
