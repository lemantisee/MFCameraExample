//////////////////////////////////////////////////////////////////////////
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


//-------------------------------------------------------------------
//  VideoBufferLock class
//
//  Locks a video buffer that might or might not support IMF2DBuffer.
//
//-------------------------------------------------------------------

class VideoBufferLock
{   
public:
    VideoBufferLock(IMFMediaBuffer *pBuffer)
    {
        mBuffer = pBuffer;
        mBuffer->AddRef();

        // Query for the 2-D buffer interface. OK if this fails.
        mBuffer->QueryInterface(IID_PPV_ARGS(&m2DBuffer));
    }

    ~VideoBufferLock()
    {
        unlock();
        SafeRelease(&mBuffer);
        SafeRelease(&m2DBuffer);
    }

    //-------------------------------------------------------------------
    // LockBuffer
    //
    // Locks the buffer. Returns a pointer to scan line 0 and returns the stride.
    //
    // The caller must provide the default stride as an input parameter, in case
    // the buffer does not expose IMF2DBuffer. You can calculate the default stride
    // from the media type.
    // uint32_t stride - Minimum stride (with no padding).
    //-------------------------------------------------------------------

    uint8_t *LockBuffer(uint32_t stride, uint32_t height)
    {
        mLocked = false;
        mActualStride = 0;
        // Use the 2-D version if available.
        if (m2DBuffer) {
            uint8_t* scanLine = nullptr;
            if (HRESULT hr = m2DBuffer->Lock2D(&scanLine, &mActualStride); FAILED(hr)) {
                return nullptr;
            }
            mLocked = true;
            return scanLine;
        }

        // Use non-2D version.
        uint8_t* data = nullptr;
        if (HRESULT hr = mBuffer->Lock(&data, nullptr, nullptr); FAILED(hr)) {
            return nullptr;
        }

        mLocked = true;

        mActualStride = stride;
        if (stride < 0) {
            // Bottom-up orientation. Return a pointer to the start of the
            // last row *in memory* which is the top row of the image.
            return data + abs(long(stride)) * (height - 1);
        }

        // Top-down orientation. Return a pointer to the start of the
        // buffer.
        return data;
    }

    long getStride() const
    {
        return mActualStride;
    }

    void unlock()
    {
        if (!mLocked) {
            return;
        }

        mLocked = false;

        if (m2DBuffer) {
            (void)m2DBuffer->Unlock2D();
            return;
        }

        (void)mBuffer->Unlock();
    }

private:
    long mActualStride = 0;
    IMFMediaBuffer *mBuffer = nullptr;
    IMF2DBuffer *m2DBuffer = nullptr;
    bool mLocked = false;
};
