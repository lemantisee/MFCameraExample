#pragma once

#include <cstdint>

#include <string>

class FormatConvertor
{
public:
    FormatConvertor() = default;
    virtual ~FormatConvertor() = default;
    virtual bool convert(uint8_t* destination, uint32_t destStride, const uint8_t* source,
        uint32_t srcStride, uint32_t width, uint32_t height) const = 0;

    virtual std::string type() const = 0;
};

class FormatConvertorRGB24 : public FormatConvertor
{
public:
    bool convert(uint8_t* destination, uint32_t destStride, const uint8_t* source,
        uint32_t srcStride, uint32_t width, uint32_t height) const override;

    std::string type() const override { return "RGB24"; }
};

class FormatConvertorRGB32 : public FormatConvertor
{
public:
    bool convert(uint8_t* destination, uint32_t destStride, const uint8_t* source,
        uint32_t srcStride, uint32_t width, uint32_t height) const override;

    std::string type() const override { return "RGB32"; }
};

class FormatConvertorYUY2 : public FormatConvertor
{
public:
    bool convert(uint8_t* destination, uint32_t destStride, const uint8_t* source,
        uint32_t srcStride, uint32_t width, uint32_t height) const override;

    std::string type() const override { return "YUY2"; }
};

class FormatConvertorNV12 : public FormatConvertor
{
public:
    bool convert(uint8_t* destination, uint32_t destStride, const uint8_t* source,
        uint32_t srcStride, uint32_t width, uint32_t height) const override;

    std::string type() const override { return "NV12"; }
};
