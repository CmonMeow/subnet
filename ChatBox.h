#pragma once

#include <shellapi.h>
#include "ChatSelection.h"

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
    size_t _inputViewStart;
    size_t _scrollOffset;
    enum
    {
        BOX_X = 12,
        BOX_Y = 14,
        BOX_WIDTH = 680,
        LINE_HEIGHT = 18,
        INPUT_HEIGHT = 24,
        INPUT_TEXT_X = BOX_X + 10,
        INPUT_TEXT_Y = BOX_Y + 12,
        INPUT_CHAR_WIDTH = 12,
        HISTORY_WHEEL_LINES = 3
    };

    string historyText(const cNetworkRuntime& network,const NetworkChatLine& line) const {
        if(line.kind==CLKFile || line.kind==CLKImage)return FitChatText(network.fileLabel(line.fileSender,line.fileId),(int)boxWidth()-24);
        if(line.kind==CLKScreen)return FitChatText(network.screenLabel(line.fileSender,line.fileId),(int)boxWidth()-24);
        return line.text;
    }
    bool historyPoint(const cNetworkRuntime& network,ChatSelection::Point& point,bool clamp=false) const {
        const auto& lines=network.chatLines();const size_t rows=visibleRows();
        const size_t last=lines.size()>_scrollOffset?lines.size()-_scrollOffset:0;
        const size_t first=last>rows?last-rows:0;if(first==last)return false;
        const int top=BOX_Y+(int)rows*LINE_HEIGHT+INPUT_HEIGHT+18-22;
        const int mouseY=App.size.y-input.mouse.y;
        int row=(int)floor((top+LINE_HEIGHT-2-mouseY)/(double)LINE_HEIGHT);
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
        if (_strnicmp(command.c_str(), prefix, prefixLength) != 0)
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
                result = text;
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
        _cursor += text.size();
    }

    size_t visibleRows() const
    {
        return (size_t)(std::max)(1, (App.size.y - 68 - BOX_Y - INPUT_HEIGHT - 18) / LINE_HEIGHT);
    }

    float boxWidth() const
    {
        return (float)(std::max)(120, App.size.x - BOX_X * 2);
    }

    void updateLayout(cNetworkRuntime& network) {
        const int width=(int)boxWidth();
        if(width==_layoutWidth)return;
        _layoutWidth=width;_inputViewStart=0;
        const auto& old=network.chatLines();
        const size_t bottom=old.size()>_scrollOffset?old.size()-_scrollOffset:0;
        const auto anchor=bottom?old[bottom-1]:NetworkChatLine{};
        if(!network.setChatWrap((size_t)(std::max)(1,int(boxWidth()-24)/12)))return;
        _selection.clear();_selecting=false;_hoverLine=0;
        const auto& lines=network.chatLines();
        if(_scrollOffset && bottom) {
            for(size_t i=0;i<lines.size();++i)
                if((anchor.message && lines[i].message==anchor.message &&
                    lines[i].messageOffset<=anchor.messageOffset &&
                    lines[i].messageOffset+lines[i].text.size()>anchor.messageOffset) ||
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
            _cursor = _draft.size();
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

    size_t inputViewStart()
    {
        clampCursor();
        if (_cursor < _inputViewStart)
        {
            _inputViewStart = _cursor;
        }
        const size_t usableChars = (size_t)(std::max)(1,int(boxWidth()-48)/12);
        if (_cursor > _inputViewStart + usableChars)
        {
            _inputViewStart = _cursor - usableChars;
        }
        if (_inputViewStart > _draft.size())
        {
            _inputViewStart = _draft.size();
        }
        return _inputViewStart;
    }

    void setCursorFromMouse()
    {
        const __int32 mouseX = input.mouse.x;
        size_t viewStart = inputViewStart();
        int relativeX = mouseX - INPUT_TEXT_X - ChatTextWidth("> ");
        _cursor = viewStart;
        while (_cursor < _draft.size())
        {
            int advance = _draft[_cursor] == ' ' ? 6 : 12;
            if (relativeX < advance / 2) break;
            relativeX -= advance;
            ++_cursor;
        }
    }

    static bool IsUrlChar(char c)
    {
        return c > 32 && c != '"' && c != '\'' && c != '<' && c != '>' && c != '[' && c != ']';
    }

    static bool FindNextUrl(const string& line, size_t searchFrom, size_t& urlStart, size_t& urlEnd)
    {
        const char* prefixes[] = { "http://", "https://", "www." };
        size_t best = string::npos;
        for (size_t p = 0; p < 3; ++p)
        {
            size_t found = line.find(prefixes[p], searchFrom);
            if (found != string::npos && (best == string::npos || found < best))
            {
                best = found;
            }
        }
        if (best == string::npos)
        {
            return false;
        }

        size_t end = best;
        while (end < line.size() && IsUrlChar(line[end]))
        {
            ++end;
        }
        while (end > best && (line[end - 1] == '.' || line[end - 1] == ',' || line[end - 1] == ')' || line[end - 1] == ';'))
        {
            --end;
        }
        if (end <= best)
        {
            return false;
        }
        urlStart = best;
        urlEnd = end;
        return true;
    }

    static bool UrlAtCharacter(const string& text, size_t character, string& url)
    {
        size_t start=0,end=0,offset=0;
        while(FindNextUrl(text,offset,start,end)) {
            if(character>=start && character<end) {
                url=text.substr(start,end-start);
                if(url.compare(0,4,"www.")==0)url="http://"+url;
                return true;
            }
            offset=end;
        }
        url.clear();return false;
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
            QueueChatText(text.substr(start,end-start).c_str(),x+ChatTextWidth(text.substr(first,start-first)),y,1.f,.92f,.22f);
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
        const float height = (float)(rows * (size_t)LINE_HEIGHT) + INPUT_HEIGHT + 18.f;
        const float textTop = BOX_Y + height - 22.f;
        const float mouseX = (float)input.mouse.x;
        const float mouseY = (float)(-input.mouse.y + App.size.y);
        for (size_t i = first; i < newestExclusive; ++i)
        {
            const float lineY = textTop - (float)(i - first) * LINE_HEIGHT;
            if (mouseY >= lineY - 2.f && mouseY <= lineY + LINE_HEIGHT - 2.f && mouseX >= BOX_X + 8.f)
            {
                if (lines[i].kind == CLKScreen)
                {
                    const string label=FitChatText(network.screenLabel(lines[i].fileSender,lines[i].fileId),(int)boxWidth()-24);
                    if(mouseX < BOX_X+8.f+ChatTextWidth(label)) { if(activate) network.clickScreen(lines[i].fileSender,lines[i].fileId); return true; }
                    continue;
                }
                if (lines[i].kind == CLKFile || lines[i].kind == CLKImage)
                {
                    const string label = FitChatText(network.fileLabel(lines[i].fileSender,lines[i].fileId),(int)boxWidth()-24);
                    if (mouseX < BOX_X + 8.f + ChatTextWidth(label))
                    {
                        if(activate) network.clickFile(lines[i].fileSender,lines[i].fileId);
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
        size_t split = argument.find(' ');
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

        case ChatCommand::Hosts:
            if (network.browseHosts) network.browseHosts();
            break;

        case ChatCommand::Host:
            if (!Trim(command.substr(5)).empty())
                network.showNotice("usage: /host", true);
            else if (network.hasConnection())
                network.showNotice("Use /disconnect before hosting.", true);
            else
                network.hostOnPort();
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

        case ChatCommand::Disconnect:
            network.disconnect();
            break;

        case ChatCommand::Kick:
        case ChatCommand::Ban:
        case ChatCommand::Users:
            network.sendChat(command);
            break;

        case ChatCommand::Private:
        {
            const string prefix = command.substr(0, command.find_first_of(" \t"));

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
        _cursor = 0;
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
    bool linkHovered(cNetworkRuntime& network) { return openLinkUnderMouse(network,false); }
    void prepareCommand(const char* command)
    {
        _draft = string(command) + " ";
        _cursor = _draft.size();
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
        const float height = (float)(visibleRows() * LINE_HEIGHT) + INPUT_HEIGHT + 18.f;
        const float width = boxWidth();
        const float mouseX = (float)input.mouse.x;
        const float mouseY = (float)(-input.mouse.y + App.size.y);
        return mouseX >= BOX_X && mouseX <= BOX_X + width && mouseY >= BOX_Y && mouseY <= BOX_Y + height;
    }

    void update(cNetworkRuntime& network)
    {
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
            if (!_active) { _active = true; _cursor = _draft.size(); }
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
                _cursor = _draft.size();
                DrainTextInput();
                input.KeyUp(VK_RETURN);
                input.KeyUp(VK_LBUTTON);
            }
            else if (input.pressed(VK_OEM_2))
            {
                _active = true;
                _draft = "/";
                _cursor = _draft.size();
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
                ++_cursor;
            }
        }

        if (input.pressed(VK_BACK))
        {
            if (_cursor > 0 && !_draft.empty())
            {
                _draft.erase(_cursor - 1, 1);
                --_cursor;
            }
            input.KeyUp(VK_BACK);
        }
        if (input.pressed(VK_DELETE))
        {
            if (_cursor < _draft.size())
            {
                _draft.erase(_cursor, 1);
            }
            input.KeyUp(VK_DELETE);
        }
        if (input.pressed(VK_LEFT))
        {
            if (_cursor > 0)
            {
                --_cursor;
            }
            input.KeyUp(VK_LEFT);
        }
        if (input.pressed(VK_RIGHT))
        {
            if (_cursor < _draft.size())
            {
                ++_cursor;
            }
            input.KeyUp(VK_RIGHT);
        }
        if (input.pressed(VK_HOME))
        {
            _cursor = 0;
            input.KeyUp(VK_HOME);
        }
        if (input.pressed(VK_END))
        {
            _cursor = _draft.size();
            input.KeyUp(VK_END);
        }
        if (input.pressed(VK_ESCAPE) || input.leftClick() && !capturesMouse)
        {
            _draft.clear();
            _cursor = 0;
            _inputViewStart = 0;
            _active = false;
            input.KeyUp(VK_ESCAPE);
        }
        if (input.pressed(VK_RETURN))
        {
            submit(network);
            input.KeyUp(VK_RETURN);
        }
    }

    void draw(cNetworkRuntime& network, bool windowFocused = true, bool showHistory = true)
    {
        updateLayout(network);
        clampScroll(network);
        const float x = BOX_X;
        const float y = BOX_Y;
        const float width = boxWidth();
        const float lineHeight = LINE_HEIGHT;
        const float inputHeight = INPUT_HEIGHT;
        const vector<NetworkChatLine>& lines = network.chatLines();
        const size_t rows = visibleRows();
        const float historyHeight = (float)(rows * (size_t)lineHeight);
        const float height = historyHeight + inputHeight + 18.f;

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
            if (lines[i].kind == CLKScreen)
            {
                const string label=FitChatText(network.screenLabel(lines[i].fileSender,lines[i].fileId),(int)width-24);
                QueueChatText(label.c_str(),x+8.f,textY-(float)(i-first)*lineHeight,.25f,1.f,.42f);
                continue;
            }
            if (lines[i].kind == CLKFile || lines[i].kind == CLKImage)
            {
                const string label = FitChatText(network.fileLabel(lines[i].fileSender,lines[i].fileId),(int)width-24);
                QueueChatText(label.c_str(),x+8.f,textY-(float)(i-first)*lineHeight,1.f,lines[i].kind==CLKImage ? .92f : .30f,lines[i].kind==CLKImage ? .22f : .26f);
                continue;
            }
            DrawChatLineWithLinks(lines[i], x + 8.f, textY - (float)(i - first) * lineHeight, r, g, b);
        }

        size_t viewStart = inputViewStart();
        string draft = _draft.substr(viewStart);
        size_t cursorInView = _cursor - viewStart;
        const size_t maxDraftChars = (size_t)(std::max)(1,int(width-48)/12);
        if (draft.size() > maxDraftChars)
        {
            draft.resize(maxDraftChars);
        }
        if (cursorInView > draft.size())
        {
            cursorInView = draft.size();
        }
        string inputLine = "> " + draft;
        if (_active && windowFocused)
            QueueChatRect(x + 6.f, y + 6.f, x + width - 6.f, y + inputHeight + 4.f, .18f, .65f, .57f, 1.f, true);
        QueueChatText(inputLine.c_str(), x + 10.f, y + 12.f, .90f, .94f, .92f);
        if (_draft.empty())
            QueueChatText(FitChatText("Message or /command", (int)width - 48).c_str(), x + 28.f, y + 12.f, .42f, .50f, .47f);
        if (_active && windowFocused && (GetTickCount64() / 500) % 2 == 0)
        {
            float caretX = x + 10.f + ChatTextWidth("> " + draft.substr(0, cursorInView));
            QueueChatRect(caretX, y + 12.f, caretX + 1.f, y + 26.f, .70f, 1.f, .9f, 1.f, false);
        }
        if (showHistory) drawTimestamp(network,windowFocused);


    }
};

