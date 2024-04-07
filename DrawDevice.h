//////////////////////////////////////////////////////////////////////////
//
// device.h: Manages the Direct3D device
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

#include <vector>

#include <d3d9.h>
#include <mfapi.h>

class FormatConvertor;

class DrawDevice
{
public:
    DrawDevice();
    ~DrawDevice();

    bool createDevice(HWND hwnd);
    bool resetDevice();
    void DestroyDevice();
    bool setVideoType(IMFMediaType* pType);
    bool DrawFrame(IMFMediaBuffer* pBuffer);

    bool isFormatSupported(REFGUID subtype) const;
    std::vector<GUID> getSupportedFormats() const;

private:
    bool TestCooperativeLevel();
    const FormatConvertor *findConversionFunction(REFGUID subtype) const;
    bool createSwapChains();
    void UpdateDestinationRect();

    HWND mWindow = nullptr;
    IDirect3D9 *mD3D = nullptr;
    IDirect3DDevice9 *mDevice = nullptr;
    IDirect3DSwapChain9 *mSwapChain = nullptr;
    D3DPRESENT_PARAMETERS mD3Params;
    D3DFORMAT mFormat = D3DFMT_UNKNOWN;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    LONG mDefaultStride = 0;
    MFRatio mAspect = {1, 1};
    MFVideoInterlaceMode mInterlaceMode = MFVideoInterlace_Unknown;
    RECT mDestRect = {};
    const FormatConvertor* mRGB32Converter = nullptr;
};
