//////////////////////////////////////////////////////////////////////////
//
// preview.cpp: Manages video preview.
// 
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#include "Camera.h"

#include <shlwapi.h>
#include <mferror.h>

#include "SafeRelease.h"
#include "Debug.h"

namespace {
    class MFObjectGuard {
    public:
        MFObjectGuard(IUnknown *object) :mObject(object){}
        ~MFObjectGuard() {
            if (mObject) {
                mObject->Release();
            }
        }
    private:
        IUnknown* mObject = nullptr;
    };
}


Camera::Camera(HWND hVideo, HWND hEvent, uint32_t width, uint32_t height, uint32_t fps) :
    mVideoWindow(hVideo), mAppWindow(hEvent), mWidth(width), mHeight(height), mFps(fps)
{
}

Camera::~Camera()
{
    closeDevice();

    mDrawDevice.DestroyDevice();
}

bool Camera::init()
{
    if (!mDrawDevice.createDevice(mVideoWindow)) {
        return false;
    }

    IMFActivate* firstDevice = findFirstDevice();
    if (!firstDevice) {
        return false;
    }

    bool ok = setDevice(firstDevice);
    firstDevice->Release();

    return ok;
}

void Camera::closeDevice()
{
    {
        std::lock_guard lock(mMutex);

        if (mReader) {
            mReader->Release();
            mReader = nullptr;
        }
    }


    CoTaskMemFree(mSymbolicLink);
    mSymbolicLink = nullptr;
    mSymbolicLinkId = 0;
}

ULONG Camera::AddRef()
{
    return 1;
}

ULONG Camera::Release()
{
    return 0;
}

HRESULT Camera::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = 
    {
        QITABENT(Camera, IMFSourceReaderCallback),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

// Called when the IMFMediaSource::ReadSample method completes.
HRESULT Camera::OnReadSample(
    HRESULT hrStatus,
    DWORD /* dwStreamIndex */,
    DWORD /* dwStreamFlags */,
    LONGLONG /* llTimestamp */,
    IMFSample *pSample      // Can be NULL
    )
{
    if (FAILED(hrStatus)) {
        return hrStatus;
    }

    std::lock_guard lock(mMutex);
    if (pSample) {
        if (!drawSample(pSample)) {
            return HRESULT(1);
        }
    }

    return mReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        nullptr,   // actual
        nullptr,   // flags
        nullptr,   // timestamp
        nullptr    // sample
    );
}

IMFMediaSource* Camera::createSource(IMFActivate* activate) const
{
    IMFMediaSource* source = nullptr;
    if (HRESULT hr = activate->ActivateObject(__uuidof(IMFMediaSource), (void**)&source);  SUCCEEDED(hr)) {
        return source;
    }

    if (source) {
        source->Shutdown();
        source->Release();
    }

    return nullptr;
}

IMFAttributes* Camera::createAttributes()
{
    IMFAttributes* attributes = nullptr;

    if (HRESULT hr = MFCreateAttributes(&attributes, 2);  FAILED(hr)) {
        if (attributes) {
            attributes->Release();
        }
        return nullptr;
    }

    if (!attributes) {
        return nullptr;
    }

    if (HRESULT hr = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);  FAILED(hr)) {
        attributes->Release();
        return nullptr;
    }

    if (HRESULT hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this); FAILED(hr)) {
        attributes->Release();
        return nullptr;
    }

    return attributes;
}

bool Camera::setupOutputFormat(IMFSourceReader* reader)
{
    // Try to find a suitable output type.
    for (uint32_t i = 0; ; i++) {
        IMFMediaType* nativeType = nullptr;
        HRESULT hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType);

        MFObjectGuard nativeTypeGuard(nativeType);

        if (FAILED(hr)) {
            break;
        }

        if (!checkRequiredResolution(nativeType)) {
            continue;
        }

        if (!adjustMediaTypeToDevice(nativeType)) {
            continue;
        }

        if (mDrawDevice.setVideoType(nativeType)) {
            return true;
        }
    }

    return false;
}

IMFSourceReader* Camera::createReader(IMFMediaSource* source)
{
    IMFAttributes* attributes = createAttributes();
    if (!attributes) {
        return nullptr;
    }

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromMediaSource(source, attributes, &reader);
    attributes->Release();

    return SUCCEEDED(hr) ? reader : nullptr;
}

bool Camera::checkRequiredResolution(IMFMediaType* nativeType) const
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (HRESULT hr = MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height); FAILED(hr)) {
        return false;
    }

    uint32_t numerator = 0;
    uint32_t denominator = 0;
    if (HRESULT hr = MFGetAttributeSize(nativeType, MF_MT_FRAME_RATE, &numerator, &denominator); FAILED(hr)) {
        return false;
    }

    Info("Native resolution %ix%i@%1.3f\n", width, height, float(numerator) / float(denominator));

    return mWidth == width && mHeight == height && mFps == numerator / denominator;
}

bool Camera::adjustMediaTypeToDevice(IMFMediaType* nativeType) const
{
    GUID subtype = { 0 };
    if (HRESULT hr = nativeType->GetGUID(MF_MT_SUBTYPE, &subtype); FAILED(hr)) {
        return false;
    }

    if (mDrawDevice.isFormatSupported(subtype)) {
        return mReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nativeType) == S_OK;
    }

    // Can we decode this media type to one of our supported
    // output formats?
    for (GUID format : mDrawDevice.getSupportedFormats()) {
        if (HRESULT hr = nativeType->SetGUID(MF_MT_SUBTYPE, format); FAILED(hr)) {
            break;
        }

        // Try to set this type on the source reader.
        if (HRESULT hr = mReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nativeType);
            SUCCEEDED(hr)) {
            return true;
        }
    }

    return false;
}

IMFActivate *Camera::findFirstDevice()
{
    // Initialize an attribute store to specify enumeration parameters.
    IMFAttributes* pAttributes = nullptr;
    if (HRESULT hr = MFCreateAttributes(&pAttributes, 1); FAILED(hr)) {
        SafeRelease(&pAttributes);
        return nullptr;
    }

    MFObjectGuard attrGuard(pAttributes);

    HRESULT typeRes = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    if (FAILED(typeRes)){
        return nullptr;
    }

    // Enumerate devices.
    IMFActivate** ppDevices = nullptr;
    uint32_t count = 0;
    if (HRESULT hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count); FAILED(hr)) {
        return nullptr;
    }

    IMFActivate* firstDevice = nullptr;
    if (count > 0) {
        firstDevice = ppDevices[0];
        firstDevice->AddRef();
    }

    for (uint32_t i = 0; i < count; i++) {
        SafeRelease(&ppDevices[i]);
    }
    CoTaskMemFree(ppDevices);

    return firstDevice;
}

bool Camera::drawSample(IMFSample* sample)
{
    // Get the video frame buffer from the sample.
    IMFMediaBuffer* pBuffer = nullptr;
    if (HRESULT hr = sample->GetBufferByIndex(0, &pBuffer); FAILED(hr)) {
        SafeRelease(&pBuffer);
        return false;
    }

    // Draw the frame.
    if (!mDrawDevice.DrawFrame(pBuffer)) {
        SafeRelease(&pBuffer);
        return false;
    }

    SafeRelease(&pBuffer);
    return true;
}

//-------------------------------------------------------------------
// SetDevice
//
// Set up preview for a specified video capture device. 
//-------------------------------------------------------------------

bool Camera::setDevice(IMFActivate *pActivate)
{
    Info("SetDevice\n");
    // Release the current device, if any.
    closeDevice();

    // Create the media source for the device.
    IMFMediaSource* source = createSource(pActivate);
    if (!source) {
        return false;
    }

    MFObjectGuard gurad(source);

    // Get the symbolic link.
    HRESULT symRes = pActivate->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        &mSymbolicLink,
        &mSymbolicLinkId
    );

    if (FAILED(symRes)) {
        source->Shutdown();
        return false;
    }

    std::lock_guard lock(mMutex);

    mReader = createReader(source);
    if (!mReader) {
        source->Shutdown();
        return false;
    }

    if (!setupOutputFormat(mReader)) {
        return false;
    }

    // Ask for the first sample.
    HRESULT hr = mReader->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        closeDevice();
        return false;
    }

    return true;
}

//-------------------------------------------------------------------
//  ResizeVideo
//  Resizes the video rectangle.
//
//  The application should call this method if the size of the video
//  window changes; e.g., when the application receives WM_SIZE.
//-------------------------------------------------------------------

void Camera::resizeVideo(WORD /*width*/, WORD /*height*/)
{
    {
        std::lock_guard lock(mMutex);

        if (mDrawDevice.resetDevice()) {
            return;
        }
    }

    MessageBox(NULL, L"ResetDevice failed!", NULL, MB_OK);
}


//-------------------------------------------------------------------
//  CheckDeviceLost
//  Checks whether the current device has been lost.
//
//  The application should call this method in response to a
//  WM_DEVICECHANGE message. (The application must register for 
//  device notification to receive this message.)
//-------------------------------------------------------------------

bool Camera::isDeviceLost(DEV_BROADCAST_HDR *pHdr) const
{
    if (!pHdr) {
        return false;
    }

    if (pHdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
        return false;
    }

    DEV_BROADCAST_DEVICEINTERFACE* pDi = (DEV_BROADCAST_DEVICEINTERFACE*)pHdr;

    std::lock_guard lock(mMutex);

    if (!mSymbolicLink) {
        return false;
    }

    return _wcsicmp(mSymbolicLink, pDi->dbcc_name) == 0;
}

