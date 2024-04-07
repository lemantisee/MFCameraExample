//////////////////////////////////////////////////////////////////////////
//
// device.cpp: Manages the Direct3D device
// 
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#include "DrawDevice.h"

#include <memory>
#include <array>
#include <iterator>
#include <algorithm>
#include <unordered_map>

#include "SafeRelease.h"
#include "BufferLock.h"
#include "FormatConvertor.h"
#include "Debug.h"

const DWORD NUM_BACK_BUFFERS = 2;


inline LONG Width(const RECT& r)
{
    return r.right - r.left;
}

inline LONG Height(const RECT& r)
{
    return r.bottom - r.top;
}

namespace {

    class MFObjectGuard {
    public:
        MFObjectGuard(IUnknown* object) :mObject(object) {}
        ~MFObjectGuard() {
            if (mObject) {
                mObject->Release();
            }
        }
    private:
        IUnknown* mObject = nullptr;
    };

    // Static table of output formats and conversion functions.
    struct ConversionFunction
    {
        GUID subtype = {};
        std::unique_ptr<FormatConvertor> converter;
    };

    std::array<ConversionFunction, 4> formatConversions =
    {
        ConversionFunction{MFVideoFormat_RGB32, std::make_unique<FormatConvertorRGB32>()},
        ConversionFunction{MFVideoFormat_RGB24, std::make_unique<FormatConvertorRGB24>()},
        ConversionFunction{MFVideoFormat_YUY2,  std::make_unique<FormatConvertorYUY2>()},
        ConversionFunction{MFVideoFormat_NV12,  std::make_unique<FormatConvertorNV12>()}
    };

    //-------------------------------------------------------------------
// LetterBoxDstRect
//
// Takes a src rectangle and constructs the largest possible 
// destination rectangle within the specifed destination rectangle 
// such thatthe video maintains its current shape.
//
// This function assumes that pels are the same shape within both the 
// source and destination rectangles.
//
//-------------------------------------------------------------------

    RECT LetterBoxRect(const RECT& rcSrc, const RECT& rcDst)
    {
        // figure out src/dest scale ratios
        int iSrcWidth = Width(rcSrc);
        int iSrcHeight = Height(rcSrc);

        int iDstWidth = Width(rcDst);
        int iDstHeight = Height(rcDst);

        int iDstLBWidth;
        int iDstLBHeight;

        if (MulDiv(iSrcWidth, iDstHeight, iSrcHeight) <= iDstWidth) {

            // Column letter boxing ("pillar box")

            iDstLBWidth = MulDiv(iDstHeight, iSrcWidth, iSrcHeight);
            iDstLBHeight = iDstHeight;
        }
        else {

            // Row letter boxing.

            iDstLBWidth = iDstWidth;
            iDstLBHeight = MulDiv(iDstWidth, iSrcHeight, iSrcWidth);
        }


        // Create a centered rectangle within the current destination rect

        RECT rc;

        LONG left = rcDst.left + ((iDstWidth - iDstLBWidth) / 2);
        LONG top = rcDst.top + ((iDstHeight - iDstLBHeight) / 2);

        SetRect(&rc, left, top, left + iDstLBWidth, top + iDstLBHeight);

        return rc;
    }


    //-----------------------------------------------------------------------------
    // CorrectAspectRatio
    //
    // Converts a rectangle from the source's pixel aspect ratio (PAR) to 1:1 PAR.
    // Returns the corrected rectangle.
    //
    // For example, a 720 x 486 rect with a PAR of 9:10, when converted to 1x1 PAR,  
    // is stretched to 720 x 540. 
    //-----------------------------------------------------------------------------

    RECT CorrectAspectRatio(const RECT& src, const MFRatio& srcPAR)
    {
        // Start with a rectangle the same size as src, but offset to the origin (0,0).
        RECT rc = { 0, 0, src.right - src.left, src.bottom - src.top };

        if ((srcPAR.Numerator != 1) || (srcPAR.Denominator != 1))
        {
            // Correct for the source's PAR.

            if (srcPAR.Numerator > srcPAR.Denominator)
            {
                // The source has "wide" pixels, so stretch the width.
                rc.right = MulDiv(rc.right, srcPAR.Numerator, srcPAR.Denominator);
            }
            else if (srcPAR.Numerator < srcPAR.Denominator)
            {
                // The source has "tall" pixels, so stretch the height.
                rc.bottom = MulDiv(rc.bottom, srcPAR.Denominator, srcPAR.Numerator);
            }
            // else: PAR is 1:1, which is a no-op.
        }
        return rc;
    }


    //-----------------------------------------------------------------------------
    // GetDefaultStride
    //
    // Gets the default stride for a video frame, assuming no extra padding bytes.
    //
    //-----------------------------------------------------------------------------

    HRESULT GetDefaultStride(IMFMediaType* pType, LONG* plStride)
    {
        LONG lStride = 0;

        // Try to get the default stride from the media type.
        HRESULT hr = pType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&lStride);
        if (FAILED(hr))
        {
            // Attribute not set. Try to calculate the default stride.
            GUID subtype = GUID_NULL;

            UINT32 width = 0;
            UINT32 height = 0;

            // Get the subtype and the image size.
            hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (SUCCEEDED(hr))
            {
                hr = MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
            }
            if (SUCCEEDED(hr))
            {
                hr = MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &lStride);
            }

            // Set the attribute for later reference.
            if (SUCCEEDED(hr))
            {
                (void)pType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(lStride));
            }
        }

        if (SUCCEEDED(hr))
        {
            *plStride = lStride;
        }
        return hr;
    }

}


//-------------------------------------------------------------------
// Constructor
//-------------------------------------------------------------------

DrawDevice::DrawDevice()
{
    std::memset(&mD3Params, 0, sizeof(mD3Params));
}


//-------------------------------------------------------------------
// Destructor
//-------------------------------------------------------------------

DrawDevice::~DrawDevice()
{
    DestroyDevice();
}


//-------------------------------------------------------------------
//  IsFormatSupported
//
//  Query if a format is supported.
//-------------------------------------------------------------------

bool DrawDevice::isFormatSupported(REFGUID subtype) const
{
    auto it = std::find_if(formatConversions.begin(), formatConversions.end(),
        [subtype](const ConversionFunction &f) {
            return f.subtype == subtype;
        });

    return it != formatConversions.end();
}




//-------------------------------------------------------------------
// CreateDevice
//
// Create the Direct3D device.
//-------------------------------------------------------------------

bool DrawDevice::createDevice(HWND hwnd)
{
    if (mDevice) {
        return true;
    }

    if (!mD3D) {
        mD3D = Direct3DCreate9(D3D_SDK_VERSION);
        if (!mD3D) {
            return false;
        }
    }

    D3DDISPLAYMODE mode = { 0 };
    if (HRESULT hr = mD3D->GetAdapterDisplayMode( D3DADAPTER_DEFAULT, &mode); FAILED(hr)) {
        return false;
    }

    HRESULT typeRes = mD3D->CheckDeviceType(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        mode.Format,
        D3DFMT_X8R8G8B8,
        TRUE    // windowed
        );

    if (FAILED(typeRes)) {
        return false;
    }

    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;  
    pp.Windowed = TRUE;
    pp.hDeviceWindow = hwnd;

    HRESULT hr = mD3D->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &pp,
        &mDevice
        );

    if (FAILED(hr)) {
        return false;
    }

    mWindow = hwnd;
    mD3Params = pp;

    return true;
}

const FormatConvertor *DrawDevice::findConversionFunction(REFGUID subtype) const
{
    auto it = std::find_if(formatConversions.begin(), formatConversions.end(),
        [subtype](const ConversionFunction& f) {
            return f.subtype == subtype;
        });

    return it == formatConversions.end() ? nullptr : it->converter.get();
}


//-------------------------------------------------------------------
// SetVideoType
//
// Set the video format.  
//-------------------------------------------------------------------

bool DrawDevice::setVideoType(IMFMediaType *pType)
{
    // Find the video subtype.
    GUID subtype = { 0 };
    if (HRESULT hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype); FAILED(hr)) {
        return false;
    }

    // Choose a conversion function.
    // (This also validates the format type.)
    mRGB32Converter = findConversionFunction(subtype);
    if (!mRGB32Converter) {
        return false;
    }

    Info("Vide format: %s\n", mRGB32Converter->type().c_str());

    mFormat = (D3DFORMAT)subtype.Data1;

    //
    // Get some video attributes.
    //

    // Get the frame size.
    if (HRESULT hr = MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &mWidth, &mHeight); FAILED(hr)) {
        return false;
    }

    // Get the interlace mode. Default: assume progressive.
    mInterlaceMode = (MFVideoInterlaceMode)MFGetAttributeUINT32(
        pType,
        MF_MT_INTERLACE_MODE, 
        MFVideoInterlace_Progressive
        );

    // Get the image stride.
    if (HRESULT hr = GetDefaultStride(pType, &mDefaultStride); FAILED(hr)) {
        return false;
    }

    Info("Resolution %ix%i stride %i\n", mWidth, mHeight, mDefaultStride);

    // Get the pixel aspect ratio. Default: Assume square pixels (1:1)
    HRESULT ratioRes = MFGetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 
        (UINT32*)&mAspect.Numerator, (UINT32*)&mAspect.Denominator);

    if (FAILED(ratioRes)) {
        mAspect.Numerator = mAspect.Denominator = 1;
    }

    if (!createSwapChains()) {
        return false;
    }

    // Update the destination rectangle for the correct
    // aspect ratio.

    UpdateDestinationRect();

    return true;
}

//-------------------------------------------------------------------
//  UpdateDestinationRect
//
//  Update the destination rectangle for the current window size.
//  The destination rectangle is letterboxed to preserve the 
//  aspect ratio of the video image.
//-------------------------------------------------------------------

void DrawDevice::UpdateDestinationRect()
{
    RECT rcClient;
    RECT rcSrc = { 0, 0, long(mWidth), long(mHeight) };

    GetClientRect(mWindow, &rcClient);

    rcSrc = CorrectAspectRatio(rcSrc, mAspect);

    mDestRect = LetterBoxRect(rcSrc, rcClient);
}

//-------------------------------------------------------------------
// CreateSwapChains
//
// Create Direct3D swap chains.
//-------------------------------------------------------------------

bool DrawDevice::createSwapChains()
{
    D3DPRESENT_PARAMETERS pp = { 0 };

    SafeRelease(&mSwapChain);

    pp.BackBufferWidth  = mWidth;
    pp.BackBufferHeight = mHeight;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_FLIP;
    pp.hDeviceWindow = mWindow;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.Flags = 
        D3DPRESENTFLAG_VIDEO | D3DPRESENTFLAG_DEVICECLIP |
        D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.BackBufferCount = NUM_BACK_BUFFERS;

    return mDevice->CreateAdditionalSwapChain(&pp, &mSwapChain) == S_OK;
}


//-------------------------------------------------------------------
// DrawFrame
//
// Draw the video frame.
//-------------------------------------------------------------------

bool DrawDevice::DrawFrame(IMFMediaBuffer *pBuffer)
{
    if (!mRGB32Converter) {
        return false;
    }

    if (!mDevice || !mSwapChain) {
        return true;
    }

    if (!TestCooperativeLevel()) {
        return false;
    }

    // Get the swap-chain surface.
    IDirect3DSurface9* pSurf = nullptr;
    if (HRESULT hr = mSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pSurf);  FAILED(hr)) {
        SafeRelease(&pSurf);
        return false;
    }

    MFObjectGuard surfGuard(pSurf);

    // Lock the swap-chain surface.
    D3DLOCKED_RECT lr = {};
    if (HRESULT hr = pSurf->LockRect(&lr, nullptr, D3DLOCK_NOSYSLOCK);  FAILED(hr)) {
        return false;
    }

    VideoBufferLock buffer(pBuffer);
    const uint8_t* scanLine = buffer.LockBuffer(mDefaultStride, mHeight);
    if (!scanLine) {
        return false;
    }

    const long stride = buffer.getStride();

    // Convert the frame. This also copies it to the Direct3D surface.
    mRGB32Converter->convert((uint8_t*)lr.pBits, lr.Pitch, scanLine, stride, mWidth, mHeight);

    if (HRESULT hr = pSurf->UnlockRect(); FAILED(hr)) {
        return false;
    }

    // Color fill the back buffer.
    IDirect3DSurface9* pBB = NULL;
    if (HRESULT hr = mDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB);  FAILED(hr)) {
        SafeRelease(&pBB);
        return false;
    }

    MFObjectGuard backBufferGuard(pBB);

    if (HRESULT hr = mDevice->ColorFill(pBB, NULL, D3DCOLOR_XRGB(0, 0, 0x80));  FAILED(hr)) {
        return false;
    }

    // Blit the frame.
    if (HRESULT hr = mDevice->StretchRect(pSurf, NULL, pBB, &mDestRect, D3DTEXF_LINEAR);  FAILED(hr)) {
        return false;
    }

    // Present the frame.
    return mDevice->Present(NULL, NULL, NULL, NULL) == S_OK;
}

std::vector<GUID> DrawDevice::getSupportedFormats() const
{
    std::vector<GUID> list;

    std::transform(formatConversions.begin(), formatConversions.end(), std::back_inserter(list),
        [](const ConversionFunction &c) {
            return c.subtype;
        });

    return list;
}

//-------------------------------------------------------------------
// TestCooperativeLevel
//
// Test the cooperative-level status of the Direct3D device.
//-------------------------------------------------------------------

bool DrawDevice::TestCooperativeLevel()
{
    if (!mDevice) {
        return false;
    }

    switch (mDevice->TestCooperativeLevel())
    {
    case D3D_OK:
        return true;
    case D3DERR_DEVICELOST:
    case D3DERR_DEVICENOTRESET:
        return resetDevice();
    default:
        break;
    }

    return false;
}


//-------------------------------------------------------------------
// ResetDevice
//
// Resets the Direct3D device.
//-------------------------------------------------------------------

bool DrawDevice::resetDevice()
{
    if (mDevice) {
        D3DPRESENT_PARAMETERS d3dpp = mD3Params;

        if (HRESULT hr = mDevice->Reset(&d3dpp); FAILED(hr))
        {
            DestroyDevice();
        }
    }

    if (!mDevice) {
        if (!createDevice(mWindow)) {
            return false;
        }
    }

    if (!mSwapChain && mFormat != D3DFMT_UNKNOWN) {
        
        if (!createSwapChains()) {
            return false;
        }

        UpdateDestinationRect();
    }

    return true;
}


//-------------------------------------------------------------------
// DestroyDevice 
//
// Release all Direct3D resources.
//-------------------------------------------------------------------

void DrawDevice::DestroyDevice()
{
    SafeRelease(&mSwapChain);
    SafeRelease(&mDevice);
    SafeRelease(&mD3D);
}
