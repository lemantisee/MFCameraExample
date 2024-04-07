//////////////////////////////////////////////////////////////////////////
//
// preview.h: Manages video preview.
// 
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <mutex>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <Dbt.h>

#include "DrawDevice.h"

//const UINT WM_APP_PREVIEW_ERROR = WM_APP + 1;    // wparam = HRESULT

class Camera : public IMFSourceReaderCallback
{
public:
    /*
    * HWND hVideo - Handle to the video window
    * HWND hEvent - Handle to the window to receive notifications
    */
    Camera(HWND hVideo, HWND hEvent, uint32_t width, uint32_t height, uint32_t fps);
    ~Camera();


    bool init();
    bool setDevice(IMFActivate* pActivate);
    void closeDevice();
    void resizeVideo(WORD width, WORD height);
    bool isDeviceLost(DEV_BROADCAST_HDR* pHdr) const;

    // IUnknown methods
    HRESULT QueryInterface(REFIID iid, void** ppv) override;
    ULONG AddRef() override;
    ULONG Release() override;

    // IMFSourceReaderCallback methods
    HRESULT OnReadSample(
        HRESULT hrStatus,
        DWORD dwStreamIndex,
        DWORD dwStreamFlags,
        LONGLONG llTimestamp,
        IMFSample *pSample
    ) override;

    HRESULT OnEvent(DWORD, IMFMediaEvent *) override
    {
        return S_OK;
    }

    HRESULT OnFlush(DWORD) override
    {
        return S_OK;
    }

private:
    bool drawSample(IMFSample* sample);
    IMFMediaSource* createSource(IMFActivate* activate) const;
    IMFAttributes* createAttributes();
    bool setupOutputFormat(IMFSourceReader *reader);
    IMFSourceReader* createReader(IMFMediaSource *source);
    bool checkRequiredResolution(IMFMediaType* nativeType) const;
    bool adjustMediaTypeToDevice(IMFMediaType* pType) const;
    IMFActivate *findFirstDevice();

    DrawDevice mDrawDevice;
    HWND mVideoWindow = nullptr;
    HWND mAppWindow = nullptr;
    IMFSourceReader* mReader = nullptr;
    WCHAR* mSymbolicLink = nullptr;
    UINT32 mSymbolicLinkId = 0;
    uint32_t mWidth = 1280;
    uint32_t mHeight = 720;
    uint32_t mFps = 30;
    mutable std::mutex mMutex;
};
