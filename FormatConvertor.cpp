#include "FormatConvertor.h"

#include <mfapi.h>
#include <algorithm>

#define D3DCOLOR_ARGB(a,r,g,b) \
    ((uint32_t)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))

#define D3DCOLOR_RGBA(r,g,b,a) D3DCOLOR_ARGB(a,r,g,b)
#define D3DCOLOR_XRGB(r,g,b)   D3DCOLOR_ARGB(0xff,r,g,b)

#define D3DCOLOR_XYUV(y,u,v)   D3DCOLOR_ARGB(0xff,y,u,v)
#define D3DCOLOR_AYUV(a,y,u,v) D3DCOLOR_ARGB(a,y,u,v)

namespace {
    struct RgbColor {
        uint8_t blue = 0;
        uint8_t green = 0;
        uint8_t red = 0;
    };

    uint8_t Clip(int clr)
    {
        return (uint8_t)(clr < 0 ? 0 : (clr > 255 ? 255 : clr));
    }

    RGBQUAD ConvertYCrCbToRGB(int y, int cr, int cb)
    {
        RGBQUAD rgbq;

        int c = y - 16;
        int d = cb - 128;
        int e = cr - 128;

        rgbq.rgbRed = Clip((298 * c + 409 * e + 128) >> 8);
        rgbq.rgbGreen = Clip((298 * c - 100 * d - 208 * e + 128) >> 8);
        rgbq.rgbBlue = Clip((298 * c + 516 * d + 128) >> 8);

        return rgbq;
    }
}

bool FormatConvertorRGB24::convert(uint8_t* destination, uint32_t destStride, const uint8_t* source, uint32_t srcStride, uint32_t width, uint32_t height) const
{
    for (uint32_t y = 0; y < height; y++)
    {
        RgbColor* pSrcPel = (RgbColor*)source;
        uint32_t* pDestPel = (uint32_t*)destination;

        for (uint32_t x = 0; x < width; x++) {
            pDestPel[x] = D3DCOLOR_XRGB(
                pSrcPel[x].red,
                pSrcPel[x].green,
                pSrcPel[x].blue
            );
        }

        source += srcStride;
        destination += destStride;
    }

    return true;
}

bool FormatConvertorRGB32::convert(uint8_t* destination, uint32_t destStride, const uint8_t* source, uint32_t srcStride, uint32_t width, uint32_t height) const
{
    return MFCopyImage(destination, destStride, source, srcStride, width * 4, height) == 0;
}

bool FormatConvertorYUY2::convert(uint8_t* destination, uint32_t destStride, const uint8_t* source, uint32_t srcStride, uint32_t width, uint32_t height) const
{
    for (uint32_t y = 0; y < height; y++) {
        RGBQUAD* pDestPel = (RGBQUAD*)destination;
        const uint16_t* pSrcPel = (const uint16_t*)source;

        for (uint32_t x = 0; x < width; x += 2)
        {
            // Byte order is U0 Y0 V0 Y1

            int y0 = (int)LOBYTE(pSrcPel[x]);
            int u0 = (int)HIBYTE(pSrcPel[x]);
            int y1 = (int)LOBYTE(pSrcPel[x + 1]);
            int v0 = (int)HIBYTE(pSrcPel[x + 1]);

            pDestPel[x] = ConvertYCrCbToRGB(y0, v0, u0);
            pDestPel[x + 1] = ConvertYCrCbToRGB(y1, v0, u0);
        }

        source += srcStride;
        destination += destStride;
    }

    return true;
}

bool FormatConvertorNV12::convert(uint8_t* destination, uint32_t destStride, const uint8_t* source, uint32_t srcStride, uint32_t width, uint32_t height) const
{
    const uint8_t* lpBitsY = source;
    const uint8_t* lpBitsCb = lpBitsY + (height * srcStride);
    const uint8_t* lpBitsCr = lpBitsCb + 1;

    for (uint32_t y = 0; y < height; y += 2)
    {
        const uint8_t* lpLineY1 = lpBitsY;
        const uint8_t* lpLineY2 = lpBitsY + srcStride;
        const uint8_t* lpLineCr = lpBitsCr;
        const uint8_t* lpLineCb = lpBitsCb;

        LPBYTE lpDibLine1 = destination;
        LPBYTE lpDibLine2 = destination + destStride;

        for (uint32_t x = 0; x < width; x += 2)
        {
            int  y0 = int(lpLineY1[0]);
            int  y1 = int(lpLineY1[1]);
            int  y2 = int(lpLineY2[0]);
            int  y3 = int(lpLineY2[1]);
            int  cb = int(lpLineCb[0]);
            int  cr = int(lpLineCr[0]);

            RGBQUAD r = ConvertYCrCbToRGB(y0, cr, cb);
            lpDibLine1[0] = r.rgbBlue;
            lpDibLine1[1] = r.rgbGreen;
            lpDibLine1[2] = r.rgbRed;
            lpDibLine1[3] = 0; // Alpha

            r = ConvertYCrCbToRGB(y1, cr, cb);
            lpDibLine1[4] = r.rgbBlue;
            lpDibLine1[5] = r.rgbGreen;
            lpDibLine1[6] = r.rgbRed;
            lpDibLine1[7] = 0; // Alpha

            r = ConvertYCrCbToRGB(y2, cr, cb);
            lpDibLine2[0] = r.rgbBlue;
            lpDibLine2[1] = r.rgbGreen;
            lpDibLine2[2] = r.rgbRed;
            lpDibLine2[3] = 0; // Alpha

            r = ConvertYCrCbToRGB(y3, cr, cb);
            lpDibLine2[4] = r.rgbBlue;
            lpDibLine2[5] = r.rgbGreen;
            lpDibLine2[6] = r.rgbRed;
            lpDibLine2[7] = 0; // Alpha

            lpLineY1 += 2;
            lpLineY2 += 2;
            lpLineCr += 2;
            lpLineCb += 2;

            lpDibLine1 += 8;
            lpDibLine2 += 8;
        }

        destination += (2 * destStride);
        lpBitsY += (2 * srcStride);
        lpBitsCr += srcStride;
        lpBitsCb += srcStride;
    }

    return true;
}
