#pragma once
#include "FontMetrics.h"
#include <vector>
#include <utility>

using ChatInputRow=std::pair<size_t,size_t>;
inline std::vector<ChatInputRow> ChatInputRows(const std::string& text,size_t pixels) {
    std::vector<ChatInputRow> rows;
    size_t start=0;
    for(;;){
        size_t end=text.find('\n',start);if(end==std::string::npos)end=text.size();
        do {
            size_t count=ChatTextFit(text,start,end,pixels);
            if(!count && start<end)count=1;
            rows.push_back({start,start+count});start+=count;
        }while(start<end);
        if(end==text.size())break;
        start=end+1;
    }
    return rows;
}
inline size_t ChatInputRowAt(const std::vector<ChatInputRow>& rows,size_t cursor) {
    size_t row=0;while(row+1<rows.size() && rows[row+1].first<=cursor)++row;return row;
}
inline size_t ChatInputColumn(const std::string& text,ChatInputRow row,int x) {
    size_t cursor=row.first;
    while(cursor<row.second){const int width=ChatCharacterWidth(text[cursor]);if(x<width/2)break;x-=width;++cursor;}
    return cursor;
}
