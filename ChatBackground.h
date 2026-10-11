#pragma once
#include <windows.h>
#include <gl/GL.h>
#include <algorithm>
#include <vector>
#include "resource.h"

class ChatBackground
{
    static constexpr int Width = 1672, Height = 941;
    const unsigned char* _pixels = nullptr;
    std::vector<unsigned char> _scaled;
    int _width = 0, _height = 0;

    void resize(int width, int height)
    {
        const size_t stride = (size_t(width) + 7) / 8;
        _scaled.assign(stride * height, 0);
        const double scale = (std::max)(double(width) / Width, double(height) / Height);
        const double left = (Width - width / scale) * .5;
        const double bottom = (Height - height / scale) * .5;
        std::vector<int> columns(width);
        for (int x = 0; x < width; ++x)
            columns[x] = (std::min)(Width - 1, int(left + (x + .5) / scale));
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = (std::min)(Height - 1, int(bottom + (y + .5) / scale));
            for (int x = 0; x < width; ++x)
            {
                const int sourceX = columns[x];
                if (_pixels[sourceY * (Width / 8) + sourceX / 8] & (0x80 >> (sourceX % 8)))
                    _scaled[size_t(y) * stride + x / 8] |= 0x80 >> (x % 8);
            }
        }
        _width = width;
        _height = height;
    }

public:
    ChatBackground()
    {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_CHAT_BACKGROUND), MAKEINTRESOURCEW(10));
        if (resource && SizeofResource(module, resource) == Width / 8 * Height)
            _pixels = static_cast<const unsigned char*>(LockResource(LoadResource(module, resource)));
        if (!_pixels) Error("Could not load chat background");
    }

    void draw(int width, int height)
    {
        if (!_pixels || width <= 0 || height <= 0) return;
        if (width != _width || height != _height) resize(width, height);
        glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
        glColor3f(1.f, .18f, 0.f);
        // glBitmap cannot scale; reuse the one-bit image prepared on resize.
        glRasterPos2i(0, 0);
        glBitmap(width, height, 0, 0, 0, 0, _scaled.data());
        glPopClientAttrib();
        glPopAttrib();
    }
};
