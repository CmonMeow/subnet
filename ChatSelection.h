#pragma once
#include <algorithm>
#include <string>
#include <utility>

// Stable history IDs keep selection attached to messages when rows are evicted.
struct ChatSelection {
    using Point = std::pair<unsigned long long,size_t>;
    Point anchor{}, end{};
    void clear() { anchor = end = {}; }
    bool selected() const { return anchor.first && anchor != end; }
    bool range(unsigned long long id,size_t length,size_t& first,size_t& last) const {
        auto low=(std::min)(anchor,end), high=(std::max)(anchor,end);
        if(!selected()||id<low.first||id>high.first)return false;
        first=id==low.first?(std::min)(low.second,length):0;
        last=id==high.first?(std::min)(high.second,length):length;
        return true;
    }
    template<class Lines,class Label,class Separator> std::string text(const Lines& lines,Label label,Separator separator) const {
        std::string result;
        const typename Lines::value_type* previous=nullptr;
        for(const auto& line:lines){
            auto value=label(line);size_t first,last;
            if(!range(line.historyId,value.size(),first,last))continue;
            if(previous)result+=separator(*previous,line);
            result+=value.substr(first,last-first);previous=&line;
        }
        return result;
    }
    template<class Lines,class Label> std::string text(const Lines& lines,Label label) const {
        return text(lines,label,[](const auto&,const auto&){return "\r\n";});
    }
};
