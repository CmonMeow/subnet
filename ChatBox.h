#pragma once

#include <shellapi.h>
#include "ChatSelection.h"
#include "ChatInputLayout.h"

class cChatBox
{
    ChatSelection _selection;
    bool _selecting=false, _selectionMoved=false;
    vec2i _selectionMouse;
    HWND _selectionWindow=nullptr;
    unsigned long long _historyTail=0, _hoverLine=0;
    int _layoutWidth=0;
    ULONGLONG _hoverSince=0;
    bool _active;
    string _draft;
    size_t _cursor;
    bool _cursorPreviousRow=false;
    size_t _inputViewStart;
    mutable string _layoutDraft;
    mutable int _draftWidth=0;
    mutable std::vector<ChatInputRow> _draftRows;
    size_t _scrollOffset;
    int _nameRows=1;
    size_t _inputRowLimit=4;
    enum
    {
        BOX_X = 12,
        BOX_Y = 14,
        BOX_WIDTH = 680,
        LINE_HEIGHT = 18,
        INPUT_HEIGHT = 24,
        INPUT_TEXT_X = BOX_X + 10,
        INPUT_TEXT_Y = BOX_Y + 12,
        HISTORY_WHEEL_LINES = 3
    };

    static string fitLink(ChatLink link,int width) {
        const string action=link.action.empty()?"":"["+link.action+"]";
        const int reserved=action.empty()?0:ChatTextWidth(action)+6;
        if(reserved>width)return "";
        const size_t tailSize=(std::min)(size_t(28),link.title.size()/3);
        const string tail=link.title.size()>18?link.title.substr(link.title.size()-tailSize):"";
        if(ChatTextWidth(link.text())>width && !tail.empty()){
            string head=link.title.substr(0,link.title.size()-tailSize);
            const string suffix="..."+tail+(link.status.empty()?"":" - "+link.status);
            while(!head.empty() && ChatTextWidth(head+suffix)+reserved>width)head.pop_back();
            link.title=head+"..."+tail;
        }
        string description=link.title+(link.status.empty()?"":" - "+link.status);
        description=FitChatText(description,width-reserved);
        return description+(description.empty()||action.empty()?"":" ")+action;
    }
    string historyText(const cNetworkRuntime& network,const NetworkChatLine& line) const {
        if(line.kind==CLKFile || line.kind==CLKImage || line.kind==CLKScreen)
            return fitLink(network.chatLink(line),(int)boxWidth()-24);
        return line.text;
    }
    bool historyPoint(const cNetworkRuntime& network,ChatSelection::Point& point,bool clamp=false) const {
        const auto& lines=network.chatLines();const size_t rows=visibleRows();
        const size_t last=lines.size()>_scrollOffset?lines.size()-_scrollOffset:0;
        const size_t first=last>rows?last-rows:0;if(first==last)return false;
        const int top=BOX_Y+(int)rows*LINE_HEIGHT+inputHeight()+18-22;
        const int mouseY=App.size.y-input.mouse.y;
        int row=(int)ceil((top-2-mouseY)/(double)LINE_HEIGHT);
        if(!clamp&&(row<0||row>=(int)(last-first)||input.mouse.x<BOX_X||input.mouse.x>BOX_X+boxWidth()))return false;
        row=(std::max)(0,(std::min)(row,(int)(last-first)-1));
        const auto& line=lines[first+row];auto text=historyText(network,line);
        int x=input.mouse.x-(BOX_X+8);size_t column=0;
        while(column<text.size()) {int width=ChatCharacterWidth(text[column]);if(x<width/2)break;x-=width;++column;}
        point={line.historyId,column};return true;
    }
    void drawTimestamp(const cNetworkRuntime& network,bool focused) {
        ChatSelection::Point point;
        if(!focused||_selecting||!historyPoint(network,point)){_hoverLine=0;return;}
        if(point.first!=_hoverLine){_hoverLine=point.first;_hoverSince=GetTickCount64();return;}
        if(GetTickCount64()-_hoverSince<500)return;
        for(const auto& line:network.chatLines())if(line.historyId==point.first){
            const auto& t=line.receivedAt;char text[32];
            snprintf(text,sizeof(text),"%04u-%02u-%02u %02u:%02u:%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
            const int width=ChatTextWidth(text)+16;
            const float x=(float)(std::max)(4,(std::min)(input.mouse.x+12,App.size.x-width-4));
            const float y=(float)(std::max)(4,(std::min)(App.size.y-input.mouse.y+20,App.size.y-26));
            QueueChatRect(x,y,x+width,y+22,.08f,.08f,.08f,1.f,false);
            QueueChatText(text,x+8,y+4,.94f,.94f,.94f);break;
        }
    }
    void copySelection(const cNetworkRuntime& network) {
        const auto text=_selection.text(network.chatLines(),
            [&](const NetworkChatLine& line){return historyText(network,line);},
            [](const NetworkChatLine& previous,const NetworkChatLine& line){
                if(previous.message && previous.message==line.message) {
                    const size_t end=previous.messageOffset+previous.text.size();
                    return line.message->substr(end,line.messageOffset-end);
                }
                return string("\r\n");
            });
        if(text.empty())return;
        int count=MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0);
        if(count<=0)return;
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(count+1)*sizeof(wchar_t));if(!memory)return;
        auto buffer=static_cast<wchar_t*>(GlobalLock(memory));if(!buffer){GlobalFree(memory);return;}
        MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),buffer,count);buffer[count]=0;GlobalUnlock(memory);
        if(!OpenClipboard(GetActiveWindow())){GlobalFree(memory);return;}
        if(!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);
        CloseClipboard();
    }
    void drawSelection(const cNetworkRuntime& network,const NetworkChatLine& line,float y) {
        if(!_selection.selected())return;
        const auto text=historyText(network,line);size_t first,last;
        if(!_selection.range(line.historyId,text.size(),first,last)||first==last)return;
        const float left=BOX_X+8.f+ChatTextWidth(text.substr(0,first));
        const float right=(std::min)(BOX_X+boxWidth()-8.f,BOX_X+8.f+ChatTextWidth(text.substr(0,last)));
        if(left<right)QueueChatRect(left,y-2,right,y+LINE_HEIGHT-2,59.f/255.f,0.f,1.f,1.f,false);
    }

    static void DrainTextInput()
    {
        unsigned char ignored;
        while (input.PopTextInput(ignored))
        {
        }
    }

    static string Trim(const string& text)
    {
        size_t first = 0;
        while (first < text.size() && isspace((unsigned char)text[first]))
        {
            ++first;
        }
        size_t last = text.size();
        while (last > first && isspace((unsigned char)text[last - 1]))
        {
            --last;
        }
        return text.substr(first, last - first);
    }

    static bool ParseCommandArgument(const string& command, const char* prefix, string& argument)
    {
        size_t prefixLength = strlen(prefix);
        if (!MatchesCommand(command, prefix))
        {
            return false;
        }
        argument = Trim(command.size() > prefixLength ? command.substr(prefixLength) : string());
        return !argument.empty();
    }

    static bool ControlDown()
    {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    static string ClipboardText()
    {
        string result;
        if (!IsClipboardFormatAvailable(CF_TEXT) || !OpenClipboard(NULL))
        {
            return result;
        }

        HANDLE handle = GetClipboardData(CF_TEXT);
        if (handle)
        {
            const char* text = static_cast<const char*>(GlobalLock(handle));
            if (text)
            {
                const size_t size = (std::min)(GlobalSize(handle), (SIZE_T)CHAT_MAX_MESSAGE_CHARS);
                const char* end = static_cast<const char*>(memchr(text, 0, size));
                result.assign(text, end ? (size_t)(end - text) : size);
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return result;
    }

    void pasteClipboard(cNetworkRuntime& network)
    {
        if (IsClipboardFormatAvailable(CF_HDROP))
        {
            if (!OpenClipboard(NULL)) { network.showNotice("Could not open the clipboard.", true); return; }
            HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            vector<std::wstring> paths;
            const UINT count = drop ? DragQueryFileW(drop, 0xffffffff, nullptr, 0) : 0;
            for (UINT i = 0; i < count && i < 8; ++i)
            {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (!length || length >= 32768) continue;
                vector<wchar_t> path(length + 1);
                if (DragQueryFileW(drop, i, path.data(), length + 1)) paths.emplace_back(path.data());
            }
            // Clipboard storage belongs to Windows; release it before offering files.
            CloseClipboard();
            for (const auto& path : paths) network.offerFile(path);
            if (count > 8) network.showNotice("Paste at most eight files at once.", true);
            else if (paths.empty()) network.showNotice("No readable file paths on the clipboard.", true);
            return;
        }
        if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5))
        {
            network.offerScreenshot();
            return;
        }
        string text = SanitiseChatText(ClipboardText());
        if (text.empty() || _draft.size() >= CHAT_MAX_MESSAGE_CHARS)
        {
            return;
        }

        size_t available = CHAT_MAX_MESSAGE_CHARS - _draft.size();
        if (text.size() > available)
        {
            text.resize(available);
        }
        _draft.insert(_cursor, text);
        _cursor += text.size();_cursorPreviousRow=false;
    }

    size_t visibleRows() const
    {
        return (size_t)(std::max)(1, (App.size.y - 68 - (_nameRows-1)*18 - BOX_Y - inputHeight() - 18) / LINE_HEIGHT);
    }

    float boxWidth() const
    {
        return (float)(std::max)(120, App.size.x - BOX_X * 2);
    }

    void updateLayout(cNetworkRuntime& network) {
        const int width=(int)boxWidth();
        if(width==_layoutWidth)return;
        _layoutWidth=width;_inputViewStart=0;_cursorPreviousRow=false;
        const auto& old=network.chatLines();
        const size_t bottom=old.size()>_scrollOffset?old.size()-_scrollOffset:0;
        const auto anchor=bottom?old[bottom-1]:NetworkChatLine{};
        if(!network.setChatWrap((size_t)(std::max)(1,int(boxWidth()-24))))return;
        _selection.clear();_selecting=false;_hoverLine=0;
        const auto& lines=network.chatLines();
        if(_scrollOffset && bottom) {
            for(size_t i=0;i<lines.size();++i)
                if((anchor.message && lines[i].message==anchor.message &&
                    lines[i].messageOffset<=anchor.messageOffset &&
                    (lines[i].messageOffset+lines[i].text.size()>anchor.messageOffset || lines[i].messageOffset==anchor.messageOffset)) ||
                    (!anchor.message && !lines[i].message && lines[i].kind==anchor.kind &&
                     lines[i].fileSender==anchor.fileSender && lines[i].fileId==anchor.fileId)) {
                    _scrollOffset=lines.size()-i-1;break;
                }
        }
        _historyTail=lines.empty()?0:lines.back().historyId;
        clampScroll(network);
    }

    void clampCursor()
    {
        if (_cursor > _draft.size())
        {
            _cursor = _draft.size();_cursorPreviousRow=false;
        }
    }

    void clampScroll(const cNetworkRuntime& network)
    {
        const vector<NetworkChatLine>& lines = network.chatLines();
        const size_t rows = visibleRows();
        const size_t maxOffset = lines.size() > rows ? lines.size() - rows : 0;
        if (_scrollOffset > maxOffset)
        {
            _scrollOffset = maxOffset;
        }
    }

    const std::vector<ChatInputRow>& draftRows() const {
        const int width=(std::max)(1,int(boxWidth()-48));
        if(_draftRows.empty() || _draftWidth!=width || _layoutDraft!=_draft){
            _draftRows=ChatInputRows(_draft,width);_layoutDraft=_draft;_draftWidth=width;
        }
        return _draftRows;
    }
    int inputHeight() const {return INPUT_HEIGHT+((int)(std::min)(_inputRowLimit,draftRows().size())-1)*LINE_HEIGHT;}
    size_t inputFirstRow() {
        clampCursor();const auto& rows=draftRows();const size_t cursorRow=ChatInputRowAt(rows,_cursor,_cursorPreviousRow);
        if(cursorRow<_inputViewStart)_inputViewStart=cursorRow;
        if(cursorRow>=_inputViewStart+_inputRowLimit)_inputViewStart=cursorRow-_inputRowLimit+1;
        _inputViewStart=(std::min)(_inputViewStart,rows.size()>_inputRowLimit?rows.size()-_inputRowLimit:0);
        return _inputViewStart;
    }
    void setCursorFromMouse() {
        const auto& rows=draftRows();const size_t first=inputFirstRow();
        const int visible=(int)(std::min)(_inputRowLimit,rows.size());
        const int top=INPUT_TEXT_Y+(visible-1)*LINE_HEIGHT;
        int row=(int)ceil((top-2-(App.size.y-input.mouse.y))/(double)LINE_HEIGHT);
        row=(std::max)(0,(std::min)(row,visible-1));
        _cursor=ChatInputColumn(_draft,rows[first+row],input.mouse.x-INPUT_TEXT_X-ChatTextWidth("> "));
        _cursorPreviousRow=(_cursor==rows[first+row].second);
    }
    void moveInputRow(int direction) {
        const auto& rows=draftRows();const size_t row=ChatInputRowAt(rows,_cursor,_cursorPreviousRow);
        if((direction<0 && !row)||(direction>0 && row+1==rows.size()))return;
        const int x=ChatTextWidth(_draft.substr(rows[row].first,_cursor-rows[row].first));
        const auto target=rows[direction<0?row-1:row+1];
        _cursor=ChatInputColumn(_draft,target,x);
        _cursorPreviousRow=(_cursor==target.second);
    }

    static bool IsUrlChar(char c)
    {
        return c > 32 && c != '"' && c != '\'' && c != '<' && c != '>' && c != '[' && c != ']';
    }

    static bool FindNextUrl(const string& line, size_t searchFrom, size_t& urlStart, size_t& urlEnd)
    {
        for(size_t start=searchFrom;start<line.size();++start){
            if(start && (isalnum((unsigned char)line[start-1]) || line[start-1]=='_'))continue;
            size_t prefix=0;
            for(const char* scheme:{"http://","https://","www."})
                if(_strnicmp(line.c_str()+start,scheme,strlen(scheme))==0){prefix=strlen(scheme);break;}
            if(!prefix || start+prefix>=line.size())continue;
            const char host=line[start+prefix];
            if(!isalnum((unsigned char)host))continue;
            size_t end=start;int parentheses=0;
            while(end<line.size() && IsUrlChar(line[end])){
                if(line[end]=='(')++parentheses;
                if(line[end]==')'){if(!parentheses)break;--parentheses;}
                ++end;
            }
            while(end>start+prefix && (line[end-1]=='.'||line[end-1]==','||line[end-1]==';'))--end;
            urlStart=start;urlEnd=end;return true;
        }
        return false;
    }

    static bool UrlAtCharacter(const string& text, size_t character, string& url)
    {
        size_t start=0,end=0,offset=0;
        while(FindNextUrl(text,offset,start,end)) {
            if(character>=start && character<end) {
                url=text.substr(start,end-start);
                if(_strnicmp(url.c_str(),"www.",4)==0)url="http://"+url;
                return true;
            }
            offset=end;
        }
        url.clear();return false;
    }

    static void DrawOfferLink(const ChatLink& link,const string& label,float x,float y,bool screen=false)
    {
        const string action=link.action.empty()?string():"["+link.action+"]";
        const size_t actionStart=!action.empty() && label.size()>=action.size()
            ? label.size()-action.size() : label.size();
        const bool inactive=link.action.empty() && link.status!="sharing";
        QueueChatText(label.substr(0,actionStart).c_str(),x,y,
            inactive?.58f:.96f,inactive?.62f:.96f,inactive?.62f:.96f);
        if(actionStart<label.size())
            QueueChatText(action.c_str(),x+ChatTextWidth(label.substr(0,actionStart)),y,.35f,screen?1.f:.85f,screen?.55f:1.f);
    }

    static void DrawChatLineWithLinks(const NetworkChatLine& row, float x, float y, float r, float g, float b)
    {
        const string& text=row.message ? *row.message : row.text;
        const size_t first=row.message ? row.messageOffset : 0, last=first+row.text.size();
        size_t offset=0,start=0,end=0,drawn=first;
        while(FindNextUrl(text,offset,start,end)) {
            if(start>=last)break;
            offset=end;
            if(end<=first)continue;
            start=(std::max)(start,first);end=(std::min)(end,last);
            if(start>drawn)QueueChatText(text.substr(drawn,start-drawn).c_str(),x+ChatTextWidth(text.substr(first,drawn-first)),y,r,g,b);
            QueueChatText(text.substr(start,end-start).c_str(),x+ChatTextWidth(text.substr(first,start-first)),y,1.f,.90f,.25f);
            drawn=end;
        }
        if(drawn<last)QueueChatText(text.substr(drawn,last-drawn).c_str(),x+ChatTextWidth(text.substr(first,drawn-first)),y,r,g,b);
    }

    bool openLinkUnderMouse(cNetworkRuntime& network, bool activate = true)
    {
        const vector<NetworkChatLine>& lines = network.chatLines();
        const size_t rows = visibleRows();
        const size_t newestExclusive = lines.size() > _scrollOffset ? lines.size() - _scrollOffset : 0;
        const size_t first = newestExclusive > rows ? newestExclusive - rows : 0;
        const float height = (float)(rows * (size_t)LINE_HEIGHT) + inputHeight() + 18.f;
        const float textTop = BOX_Y + height - 22.f;
        const float mouseX = (float)input.mouse.x;
        const float mouseY = (float)(-input.mouse.y + App.size.y);
        for (size_t i = first; i < newestExclusive; ++i)
        {
            const float lineY = textTop - (float)(i - first) * LINE_HEIGHT;
            if (mouseY >= lineY - 2.f && mouseY < lineY + LINE_HEIGHT - 2.f && mouseX >= BOX_X + 8.f && mouseX < BOX_X+boxWidth()-8.f)
            {
                if(lines[i].kind==CLKScreen || lines[i].kind==CLKFile || lines[i].kind==CLKImage){
                    const auto link=network.chatLink(lines[i]);
                    const string label=fitLink(link,(int)boxWidth()-24);
                    const float right=BOX_X+8.f+ChatTextWidth(label);
                    if(!link.action.empty() && !label.empty() && mouseX>=right-ChatTextWidth("["+link.action+"]") && mouseX<right){
                        if(activate){
                            if(lines[i].kind==CLKScreen)network.clickScreen(lines[i].fileSender,lines[i].fileId);
                            else network.clickFile(lines[i].fileSender,lines[i].fileId);
                        }
                        return true;
                    }
                    continue;
                }
                size_t character = 0;
                float remainingX = mouseX - (BOX_X + 8.f);
                while (character < lines[i].text.size())
                {
                    int advance = ChatCharacterWidth(lines[i].text[character]);
                    if (remainingX < advance) break;
                    remainingX -= advance;
                    ++character;
                }
                if (character < lines[i].text.size() && (lines[i].kind == CLKNormal || lines[i].kind == CLKPrivate))
                {
                    const string& message = lines[i].message ? *lines[i].message : lines[i].text;
                    const size_t position = character + (lines[i].message ? lines[i].messageOffset : 0);
                    const size_t start = lines[i].kind == CLKPrivate ?
                        (message.compare(0,10,"(private) ") == 0 ? 10 : message.compare(0,1,">") == 0 ? 1 : 0) : 0;
                    for (const auto& person : network.participants())
                    {
                        if (person.first == network.localPlayerId() || person.second.empty()) continue;
                        if (position < start || position >= start + person.second.size() ||
                            message.compare(start, person.second.size()+2, person.second+": ") != 0) continue;
                        if (activate)
                        {
                            string draft = _draft;
                            for (const char* alias : {"/pm", "/w", "/tell", "/direct"})
                            {
                                if (!MatchesCommand(draft, alias)) continue;
                                const size_t target = draft.find_first_not_of(" \t\r\n\v\f", strlen(alias));
                                const size_t body = target == string::npos ? string::npos : draft.find_first_of(" \t\r\n\v\f", target);
                                draft = body == string::npos ? string() : draft.substr(body + 1);
                                break;
                            }
                            const string command = "/pm " + person.second;
                            if (command.size() + 1 + draft.size() > CHAT_MAX_MESSAGE_CHARS)
                            {
                                network.showNotice("Draft too long for a private message.", true);
                                _active = true;
                            }
                            else
                            {
                                prepareCommand(command.c_str());
                                _draft += draft;
                                _cursor = _draft.size();_cursorPreviousRow=false;
                            }
                        }
                        return true;
                    }
                }
                string url;
                if (character < lines[i].text.size() && UrlAtCharacter(lines[i].message ? *lines[i].message : lines[i].text,
                    character + (lines[i].message ? lines[i].messageOffset : 0), url))
                {
                    if(activate) ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    return true;
                }
            }
        }
        return false;
    }

    static bool ParsePrivateCommand(const string& command, const char* prefix, string& target, string& message)
    {
        string argument;
        if (!ParseCommandArgument(command, prefix, argument))
        {
            return false;
        }
        size_t split = argument.find_first_of(" \t\r\n\v\f");
        if (split == string::npos)
        {
            return false;
        }
        target = Trim(argument.substr(0, split));
        message = Trim(argument.substr(split + 1));
        return !target.empty() && !message.empty();
    }

    static ChatCommand ResolveCommand(const string& command)
    {
        for (const auto& entry : CHAT_COMMANDS)
        {
            if (MatchesCommand(command, entry.name))
                return entry.id;
        }

        return ChatCommand::Unknown;
    }

    void runCommand(const string& command, cNetworkRuntime& network)
    {
        const ChatCommand cmd = ResolveCommand(command);

        switch (cmd)
        {
        case ChatCommand::Name:
            network.changeName(command.substr(5));
            break;

        case ChatCommand::Hostname:
            network.setHostName(command.substr(9));
            break;

        case ChatCommand::Dedicated:
            network.setDedicated(command.substr(10));
            break;

        case ChatCommand::Connect:
        {
            string address = Trim(command.size() > 8 ? command.substr(8) : string());


            network.connectTo(address);

            break;
        }

        case ChatCommand::Clear:
            network.clearChat();
            break;

        case ChatCommand::Leave:
            network.disconnect();
            break;

        case ChatCommand::Kick:
        case ChatCommand::Ban:
        case ChatCommand::Users:
            network.sendChat(command);
            break;

        case ChatCommand::Private:
        {
            const string prefix = command.substr(0, command.find_first_of(" \t\r\n\v\f"));

            string target;
            string message;

            if (!ParsePrivateCommand(command, prefix.c_str(), target, message))
            {
                network.showNotice("usage: /pm name|netId message", true);
            }
            else
            {
                network.sendPrivateChatByReference(target, message);
            }

            break;
        }

        default:
            network.showNotice(string("unknown command: ") + command, true);
            break;
        }
    }

    void submit(cNetworkRuntime& network)
    {
        string text = Trim(_draft);
        _draft.clear();
        _cursor = 0;_cursorPreviousRow=false;
        _inputViewStart = 0;
        _active = false;
        if (text.empty())
        {
            return;
        }

        if (text[0] == '/')
        {
            runCommand(text, network);
        }
        else
        {
            network.sendChat(text);
        }
    }

public:
    void nameRows(int rows) { _nameRows=rows; }
    bool linkHovered(cNetworkRuntime& network) { return openLinkUnderMouse(network,false); }
    const string& draft() const {return _draft;}
    void restoreDraft(const string& text){_draft=SanitiseChatText(text);_cursor=_draft.size();_cursorPreviousRow=false;}
    void prepareCommand(const char* command)
    {
        _draft = string(command) + " ";
        _cursor = _draft.size();_cursorPreviousRow=false;
        _inputViewStart = 0;
        _active = true;
        _selecting = false;
        _selection.clear();
    }

    cChatBox() : _active(false), _cursor(0), _inputViewStart(0), _scrollOffset(0)
    {
    }

    size_t historyOffset() const { return _scrollOffset; }

    void deactivate() { _active = false; _selecting=false; _selection.clear(); }

    bool active() const
    {
        return _active;
    }

    bool mouseOver() const
    {
        const float height = (float)(visibleRows() * LINE_HEIGHT) + inputHeight() + 18.f;
        const float width = boxWidth();
        const float mouseX = (float)input.mouse.x;
        const float mouseY = (float)(-input.mouse.y + App.size.y);
        return mouseX >= BOX_X && mouseX <= BOX_X + width && mouseY >= BOX_Y && mouseY <= BOX_Y + height;
    }

    void update(cNetworkRuntime& network)
    {
        _inputRowLimit=network.isHost()||network.clientReady()?4:1;
        updateLayout(network);
        const auto& current=network.chatLines();
        if((_scrollOffset || _selection.selected() || (_selecting && _selectionMoved))&&_historyTail&&!current.empty()&&current.back().historyId!=_historyTail)
            for(size_t i=0;i<current.size();++i)if(current[i].historyId==_historyTail){_scrollOffset+=current.size()-i-1;break;}
        _historyTail=current.empty()?0:current.back().historyId;
        clampScroll(network);
        const bool capturesMouse = mouseOver();
        __int32 mouseWheel = capturesMouse ? input.ConsumeMouseWheel() : 0;
        if (mouseWheel != 0)
        {
            const size_t rows = visibleRows();
            const vector<NetworkChatLine>& lines = network.chatLines();
            const size_t maxOffset = lines.size() > rows ? lines.size() - rows : 0;
            size_t wheelSteps = (size_t)(abs(mouseWheel) / WHEEL_DELTA);
            if (wheelSteps == 0)
            {
                wheelSteps = 1;
            }
            const size_t scrollLines = HISTORY_WHEEL_LINES * wheelSteps;
            if (mouseWheel > 0)
            {
                _scrollOffset += scrollLines;
                if (_scrollOffset > maxOffset)
                {
                    _scrollOffset = maxOffset;
                }
            }
            else
            {
                _scrollOffset = _scrollOffset > scrollLines ? _scrollOffset - scrollLines : 0;
            }
        }

        const auto& history=network.chatLines();
        if(_selection.anchor.first) {
            bool anchor=false,end=false;
            for(const auto& line:history){anchor|=line.historyId==_selection.anchor.first;end|=line.historyId==_selection.end.first;}
            if(!anchor||!end){_selection.clear();_selecting=false;}
        }
        if(ControlDown()&&input.pressed('C')&&_selection.selected()){
            copySelection(network);input.KeyUp('C');DrainTextInput();return;
        }
        if(_selecting){
            if(GetForegroundWindow()!=_selectionWindow){_selecting=false;input.KeyUp(VK_LBUTTON);return;}
            if(abs(input.mouse.x-_selectionMouse.x)>3||abs(input.mouse.y-_selectionMouse.y)>3||mouseWheel)_selectionMoved=true;
            if(_selectionMoved)historyPoint(network,_selection.end,true);
            const bool held=(GetAsyncKeyState(VK_LBUTTON)&0x8000)||GetCapture()==_selectionWindow;
            if(!held){
                _selecting=false;
                ChatSelection::Point released;
                if(!_selectionMoved&&historyPoint(network,released)&&released==_selection.anchor)openLinkUnderMouse(network);
                if(!_selectionMoved)_selection.clear();
            }
            input.KeyUp(VK_LBUTTON);DrainTextInput();return;
        }
        if(input.leftClick()) {
            ChatSelection::Point point;
            _selection.clear();
            if(capturesMouse&&historyPoint(network,point)){
                _selection.anchor=_selection.end=point;_selecting=true;_selectionMoved=false;
                _selectionMouse=input.mouse;_selectionWindow=GetActiveWindow();_active=false;
                input.KeyUp(VK_LBUTTON);DrainTextInput();return;
            }
        }
        if(input.pressed(VK_ESCAPE))_selection.clear();
        if (ControlDown() && input.pressed('V'))
        {
            if (!_active) { _active = true; _cursor = _draft.size();_cursorPreviousRow=false; }
            pasteClipboard(network);
            DrainTextInput();
            input.KeyUp('V');
            return;
        }
        if (!_active)
        {
            if (input.pressed(VK_RETURN) || input.leftClick() && capturesMouse)
            {
                _active = true;
                _cursor = _draft.size();_cursorPreviousRow=false;
                DrainTextInput();
                input.KeyUp(VK_RETURN);
                input.KeyUp(VK_LBUTTON);
            }
            else if (input.pressed(VK_OEM_2))
            {
                _active = true;
                _draft = "/";
                _cursor = _draft.size();_cursorPreviousRow=false;
                DrainTextInput();
                input.KeyUp(VK_OEM_2);
            }
            return;
        }

        if (input.leftClick() && capturesMouse)
        {
            setCursorFromMouse();
            input.KeyUp(VK_LBUTTON);
        }

        unsigned char c;
        while (input.PopTextInput(c))
        {
            if (_draft.size() < CHAT_MAX_MESSAGE_CHARS)
            {
                _draft.insert(_cursor, 1, (char)c);
                ++_cursor;_cursorPreviousRow=false;
            }
        }

        if (input.pressed(VK_BACK))
        {
            if (_cursor > 0 && !_draft.empty())
            {
                _draft.erase(_cursor - 1, 1);
                --_cursor;_cursorPreviousRow=false;
            }
            input.KeyUp(VK_BACK);
        }
        if (input.pressed(VK_DELETE))
        {
            if (_cursor < _draft.size())
            {
                _draft.erase(_cursor, 1);_cursorPreviousRow=false;
            }
            input.KeyUp(VK_DELETE);
        }
        if (input.pressed(VK_LEFT))
        {
            if (_cursor > 0)
            {
                --_cursor;_cursorPreviousRow=false;
            }
            input.KeyUp(VK_LEFT);
        }
        if (input.pressed(VK_RIGHT))
        {
            if (_cursor < _draft.size())
            {
                ++_cursor;_cursorPreviousRow=false;
            }
            input.KeyUp(VK_RIGHT);
        }
        if(input.pressed(VK_UP)){moveInputRow(-1);input.KeyUp(VK_UP);}
        if(input.pressed(VK_DOWN)){moveInputRow(1);input.KeyUp(VK_DOWN);}
        if (input.pressed(VK_HOME))
        {
            _cursor = draftRows()[ChatInputRowAt(draftRows(),_cursor,_cursorPreviousRow)].first;
            _cursorPreviousRow=false;
            input.KeyUp(VK_HOME);
        }
        if (input.pressed(VK_END))
        {
            _cursor = draftRows()[ChatInputRowAt(draftRows(),_cursor,_cursorPreviousRow)].second;
            _cursorPreviousRow=true;
            input.KeyUp(VK_END);
        }
        if (input.pressed(VK_ESCAPE) || input.leftClick() && !capturesMouse)
        {
            _draft.clear();
            _cursor = 0;_cursorPreviousRow=false;
            _inputViewStart = 0;
            _active = false;
            input.KeyUp(VK_ESCAPE);
        }
        if (input.pressed(VK_RETURN))
        {
            if(GetKeyState(VK_SHIFT)&0x8000){
                if(_draft.size()<CHAT_MAX_MESSAGE_CHARS){_draft.insert(_cursor,1,'\n');++_cursor;_cursorPreviousRow=false;}
            }else submit(network);
            input.KeyUp(VK_RETURN);
        }
    }

    void draw(cNetworkRuntime& network, bool windowFocused = true, bool showHistory = true)
    {
        _inputRowLimit=showHistory?4:1;
        updateLayout(network);
        clampScroll(network);
        const float x = BOX_X;
        const float y = BOX_Y;
        const float width = boxWidth();
        const float lineHeight = LINE_HEIGHT;
        const float inputAreaHeight = (float)inputHeight();
        const vector<NetworkChatLine>& lines = network.chatLines();
        const size_t rows = visibleRows();
        const float historyHeight = (float)(rows * (size_t)lineHeight);
        const float height = historyHeight + inputAreaHeight + 18.f;

        const float panelTop = showHistory ? y + height : 44.f;
        QueueChatRect(x, y, x + width, panelTop, .02f, .025f, .025f, .50f, false);
        QueueChatRect(x, y, x + width, panelTop, .32f, .38f, .36f, .95f, true);

        const size_t newestExclusive = lines.size() > _scrollOffset ? lines.size() - _scrollOffset : 0;
        const size_t first = newestExclusive > rows ? newestExclusive - rows : 0;
        float textY = y + height - 22.f;
        for (size_t i = first; showHistory && i < newestExclusive; ++i)
        {
            drawSelection(network,lines[i],textY-(float)(i-first)*lineHeight);
            float r = .96f;
            float g = .96f;
            float b = .96f;
            if (lines[i].kind == CLKSystem)
            {
                r = .58f;
                g = .65f;
                b = .63f;
            }
            else if (lines[i].kind == CLKError)
            {
                r = 1.f; g = .38f; b = .32f;
            }
            else if (lines[i].kind == CLKPrivate)
            {
                r = .82f;
                g = .70f;
                b = 1.0f;
            }
            if (lines[i].kind == CLKScreen || lines[i].kind == CLKFile || lines[i].kind == CLKImage)
            {
                const auto link=network.chatLink(lines[i]);
                const string label=fitLink(link,(int)boxWidth()-24);
                DrawOfferLink(link,label,x+8.f,textY-(float)(i-first)*lineHeight,lines[i].kind==CLKScreen);
                continue;
            }
            DrawChatLineWithLinks(lines[i], x + 8.f, textY - (float)(i - first) * lineHeight, r, g, b);
        }

        const auto& inputRows=draftRows();
        const size_t firstInput=inputFirstRow();
        const size_t shown=(std::min)(_inputRowLimit,inputRows.size());
        const float inputTop=y+12.f+(shown-1)*LINE_HEIGHT;
        if (_active && windowFocused)
            QueueChatRect(x + 6.f, y + 6.f, x + width - 6.f, y + inputAreaHeight + 4.f, .35f, .85f, 1.f, 1.f, true);
        for(size_t i=0;i<shown;++i){
            const auto row=inputRows[firstInput+i];
            const string label=(firstInput+i==0?"> ":"  ")+_draft.substr(row.first,row.second-row.first);
            const float textX=x+10.f+(firstInput+i==0?0.f:ChatTextWidth("> ")-ChatTextWidth("  "));
            QueueChatText(label.c_str(),textX,inputTop-i*LINE_HEIGHT,.90f,.94f,.92f);
        }
        if(_draft.empty())
            QueueChatText(FitChatText("Message or /command",(int)width-48).c_str(),x+10.f+ChatTextWidth("> "),y+12.f,.42f,.50f,.47f);
        if(_active && windowFocused && (GetTickCount64()/500)%2==0){
            const size_t row=ChatInputRowAt(inputRows,_cursor,_cursorPreviousRow);
            const float caretX=x+10.f+ChatTextWidth("> ")+ChatTextWidth(_draft.substr(inputRows[row].first,_cursor-inputRows[row].first));
            const float caretY=inputTop-(row-firstInput)*LINE_HEIGHT;
            QueueChatRect(caretX,caretY,caretX+1.f,caretY+14.f,.65f,.92f,1.f,1.f,false);
        }
        if (showHistory) drawTimestamp(network,windowFocused);


    }
};

