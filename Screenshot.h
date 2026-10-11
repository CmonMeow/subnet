#pragma once
#include <windows.h>
#include <compressapi.h>
#include <atomic>
#include <new>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// OIIM: magic, little-endian width/height, then top-down BGRX pixels.
// OIZ1 uses the same dimensions with a Windows XPRESS-Huffman pixel payload.
// Neither format carries palettes, profiles, offsets or executable content.
struct Screenshot
{
    static constexpr size_t HeaderBytes = 12, MaxBytes = HeaderBytes + 64 * 1024 * 1024;
    static constexpr uint32_t MaxDimension = 8192;
    inline static std::atomic<size_t> memoryBytes{0};
    static constexpr size_t MemoryLimit = 128 * 1024 * 1024;
private:
    size_t _reserved = 0;
    static bool reserve(size_t size) {
        size_t used = memoryBytes.load();
        do { if (used > MemoryLimit || size > MemoryLimit - used) return false; }
        while (!memoryBytes.compare_exchange_weak(used, used + size));
        return true;
    }
    struct CodecMemory {
        size_t used = 0;
        struct alignas(MEMORY_ALLOCATION_ALIGNMENT) Block { size_t size; };
        static void* __cdecl allocate(void* context, size_t size) {
            auto& self = *static_cast<CodecMemory*>(context);
            if (size > 8 * 1024 * 1024 - self.used || !reserve(size)) return nullptr;
            auto* block = static_cast<Block*>(HeapAlloc(GetProcessHeap(),0,sizeof(Block)+size));
            if (!block) { memoryBytes.fetch_sub(size); return nullptr; }
            block->size=size;self.used+=size;return block+1;
        }
        static void __cdecl release(void* context, void* memory) {
            if (!memory) return;
            auto& self = *static_cast<CodecMemory*>(context);
            auto* block = static_cast<Block*>(memory)-1;
            const size_t size=block->size;self.used-=size;HeapFree(GetProcessHeap(),0,block);memoryBytes.fetch_sub(size);
        }
        COMPRESS_ALLOCATION_ROUTINES routines() { return {allocate,release,this}; }
    };
public:
    std::vector<unsigned char> bytes;
    Screenshot() = default;
    Screenshot(const Screenshot&) = delete;
    Screenshot& operator=(const Screenshot&) = delete;
    ~Screenshot() { std::vector<unsigned char>().swap(bytes); memoryBytes.fetch_sub(_reserved); }
    static std::shared_ptr<Screenshot> allocate(size_t size) {
        if (size < HeaderBytes || size > MaxBytes) return {};
        // A bounded allocation can still fail under system memory pressure.
        try {
            auto result = std::make_shared<Screenshot>();
            if (!reserve(size)) return {};
            result->_reserved=size;result->bytes.resize(size);return result;
        } catch (const std::bad_alloc&) { return {}; }
    }
    static uint32_t read32(const unsigned char* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    static void write32(unsigned char* p, uint32_t n) {
        for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8));
    }
    static bool dimensions(uint32_t width, uint32_t height) {
        return width && height && width <= MaxDimension && height <= MaxDimension &&
            uint64_t(width) * height <= 16 * 1024 * 1024;
    }
    bool valid() const {
        if (bytes.size() < HeaderBytes || memcmp(bytes.data(), "OIIM", 4)) return false;
        const auto w = read32(bytes.data() + 4), h = read32(bytes.data() + 8);
        return dimensions(w,h) && bytes.size() == HeaderBytes + uint64_t(w) * h * 4;
    }
    bool compressed() const { return bytes.size() > HeaderBytes && !memcmp(bytes.data(),"OIZ1",4); }
    static std::shared_ptr<Screenshot> compress(std::shared_ptr<Screenshot> image) {
        if (!image || !image->valid()) return {};
        CodecMemory memory;auto routines=memory.routines();COMPRESSOR_HANDLE codec=nullptr;
        if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,&routines,&codec)) return image;
        const auto* pixels=image->bytes.data()+HeaderBytes;
        const size_t pixelBytes=image->bytes.size()-HeaderBytes;
        // Try a buffer smaller than raw pixels. Incompressible input stays raw.
        auto result=allocate(image->bytes.size()-1);
        SIZE_T written=0;
        const bool ok=result && Compress(codec,pixels,pixelBytes,result->bytes.data()+HeaderBytes,pixelBytes-1,&written) && written && written<pixelBytes;
        CloseCompressor(codec);
        if (!ok) return image;
        memcpy(result->bytes.data(),"OIZ1",4);memcpy(result->bytes.data()+4,image->bytes.data()+4,8);
        result->bytes.resize(HeaderBytes+written);
        // Shrinking a vector does not release its allocation or budget.
        auto compact=allocate(result->bytes.size());
        if(compact){memcpy(compact->bytes.data(),result->bytes.data(),result->bytes.size());return compact;}
        return result;
    }
    static std::shared_ptr<Screenshot> expand(std::shared_ptr<Screenshot> image, bool compressed) {
        if (!image) return {};
        if (!compressed) return image->valid() ? image : nullptr;
        if (!image->compressed() || image->bytes.size()>MaxBytes) return {};
        const uint32_t width=read32(image->bytes.data()+4),height=read32(image->bytes.data()+8);
        if (!dimensions(width,height)) return {};
        const size_t pixelBytes=size_t(width)*height*4;
        // Never grow output to a size supplied by the compressed stream.
        if (image->bytes.size()-HeaderBytes >= pixelBytes) return {};
        auto result=allocate(HeaderBytes+pixelBytes);if(!result)return {};
        CodecMemory memory;auto routines=memory.routines();DECOMPRESSOR_HANDLE codec=nullptr;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,&routines,&codec)) return {};
        SIZE_T written=0;
        const bool ok=Decompress(codec,image->bytes.data()+HeaderBytes,image->bytes.size()-HeaderBytes,
            result->bytes.data()+HeaderBytes,pixelBytes,&written) && written==pixelBytes;
        CloseDecompressor(codec);
        if (!ok) return {};
        memcpy(result->bytes.data(),"OIIM",4);memcpy(result->bytes.data()+4,image->bytes.data()+4,8);
        return result;
    }
    static std::shared_ptr<Screenshot> fromDib(const unsigned char* data, size_t size) {
        if (!data || size < sizeof(BITMAPINFOHEADER)) return {};
        BITMAPINFOHEADER header; memcpy(&header,data,sizeof(header));
        if ((header.biSize != 40 && header.biSize != 108 && header.biSize != 124) || size < header.biSize ||
            header.biWidth <= 0 || !header.biHeight || header.biHeight == LONG_MIN || header.biPlanes != 1 ||
            (header.biBitCount != 24 && header.biBitCount != 32) || header.biClrUsed ||
            (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) return {};
        size_t offset = header.biSize;
        if (header.biCompression == BI_BITFIELDS) {
            if (header.biBitCount != 32 || size < 52 || read32(data+40) != 0xff0000 ||
                read32(data+44) != 0xff00 || read32(data+48) != 0xff) return {};
            if (header.biSize == 40) offset += 12;
        }
        const uint32_t width = header.biWidth, height = header.biHeight < 0 ? -header.biHeight : header.biHeight;
        if (!dimensions(width,height)) return {};
        const size_t stride = (size_t(width) * header.biBitCount + 31) / 32 * 4;
        if (offset > size || stride * height > size - offset) return {};
        auto result = allocate(HeaderBytes + size_t(width) * height * 4); if (!result) return {};
        auto* out = result->bytes.data(); memcpy(out,"OIIM",4); write32(out+4,width); write32(out+8,height);
        for (uint32_t y=0;y<height;++y) {
            const auto* row = data + offset + stride * (header.biHeight < 0 ? y : height-1-y);
            for (uint32_t x=0;x<width;++x) {
                auto* pixel = out + HeaderBytes + (size_t(y)*width+x)*4;
                memcpy(pixel,row+size_t(x)*(header.biBitCount/8),3); pixel[3]=0;
            }
        }
        return result;
    }
    static std::shared_ptr<Screenshot> clipboard() {
        if (!OpenClipboard(nullptr)) return {};
        const UINT format = IsClipboardFormatAvailable(CF_DIB) ? CF_DIB : CF_DIBV5;
        const auto handle = GetClipboardData(format);
        const auto size = handle ? GlobalSize(handle) : 0;
        auto* data = handle ? static_cast<const unsigned char*>(GlobalLock(handle)) : nullptr;
        auto result = fromDib(data,size);
        if (data) GlobalUnlock(handle);
        CloseClipboard(); return result;
    }
};
