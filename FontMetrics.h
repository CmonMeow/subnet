#pragma once
#include "FontGlyphs.h"
#include <array>
#include <string>

struct ChatGlyphMetric { int left=0, width=0, advance=0; };
inline const ChatGlyphMetric& ChatGlyph(unsigned char c) {
    static const auto metrics=[] {
        std::array<ChatGlyphMetric,128> result{};
        result[' '].advance=6;result['\t'].advance=18;
        for(unsigned c=33;c<128;++c){
            int left=16,right=-1;
            for(int y=0;y<16;++y)for(int x=0;x<16;++x)
                if(serif[c-32][y*2+x/8]&(128>>(x%8))){if(x<left)left=x;if(x>right)right=x;}
            if(right>=left)result[c]={left,right-left+1,right-left+3};
        }
        // Equal digit advances keep clocks and numeric columns steady.
        int digits=0;for(unsigned c='0';c<='9';++c)if(result[c].advance>digits)digits=result[c].advance;
        for(unsigned c='0';c<='9';++c)result[c].advance=digits;
        return result;
    }();
    return metrics[c<128?c:0];
}
inline int ChatCharacterWidth(unsigned char c) {return ChatGlyph(c).advance;}
inline int ChatTextWidth(const std::string& text) {
    int width=0;for(unsigned char c:text)width+=ChatCharacterWidth(c);return width;
}
inline size_t ChatTextFit(const std::string& text,size_t start,size_t end,size_t pixels) {
    size_t cursor=start,width=0;
    while(cursor<end){const size_t next=ChatCharacterWidth(text[cursor]);if(next>pixels-width)break;width+=next;++cursor;}
    return cursor-start;
}
