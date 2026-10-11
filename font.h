#pragma once
#include <gl/GL.h>

#include "FontGlyphs.h"

#define Glyphs serif

int ChatCharacterWidth(unsigned char c)
{
    return c == ' ' ? 6 : c == '\t' ? 18 : (c >= 33 && c <= 127 ? 12 : 0);
}

int ChatTextWidth(const string& text)
{
    int width = 0;
    for (unsigned char c : text) width += ChatCharacterWidth(c);
    return width;
}

string FitChatText(string text, int pixels)
{
    int width = ChatTextWidth(text);
    if (width <= pixels) return text;
    if (pixels < 36) return string();
    while (!text.empty() && width + 36 > pixels) {
        width -= ChatCharacterWidth(text.back());
        text.pop_back();
    }
    return text + "...";
}

vec2i drawstring(const char* text = "Error", vec3f pos = { 0, 0, 0 }, vec3f color = { .8f, .8f, .8f })
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (pos.x < 0) pos.x = 0.f;
    if (pos.y < 2.f) pos.y = 2.f;
    vec2i size = { 0, 16 };

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool shadow = pass == 0;
        if (shadow) glColor3f(.065f, .07f, .07f);
        else glColor3f(color.x, color.y, color.z);

        int line = 0;
        int width = 0;
        // Both passes use the same transformed origin. Offset the shadow in
        // framebuffer pixels so scaling and raster rounding cannot separate it.
        glRasterPos3f(pos.x, pos.y, pos.z);
        if (shadow) glBitmap(0, 0, 0.f, 0.f, 1.f, -1.f, NULL);
        for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
        {
            if (*c == '\n')
            {
                if (width > size.x) size.x = width;
                width = 0;
                ++line;
                glRasterPos3f(pos.x, pos.y - line * 16.f, pos.z);
                if (shadow) glBitmap(0, 0, 0.f, 0.f, 1.f, -1.f, NULL);
                continue;
            }
            float advance = 0.f;
            const unsigned char* glyph = Glyphs[0];
            if (*c >= 33 && *c <= 127) { advance = 12.f; glyph = Glyphs[*c - 32]; }
            else if (*c == ' ') advance = 6.f;
            else if (*c == '\t') advance = 18.f;
            if (advance > 0.f)
            {
                glBitmap(16, 16, 0.f, 0.f, advance, 0.f, glyph);
                width += (int)advance;
            }
        }
        if (width > size.x) size.x = width;
        size.y = (line + 1) * 16;
    }
    return size;
}
#undef Glyphs

