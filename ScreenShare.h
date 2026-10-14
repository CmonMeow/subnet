#pragma once
#include <memory>
#include <functional>
#include "ScreenSignaling.h"

class ScreenShare
{
    struct State;
    std::shared_ptr<State> state;
public:
    using Send = std::function<void(const ScreenSignaling::Event&)>;
    using Notice = std::function<void(const string&,bool)>;
    ScreenShare(HWND parent,Send send,Notice notice);
    ~ScreenShare();
    void toggle();
    void watch(int owner,const string& share);
    void receive(const ScreenSignaling::Event& event);
    void update();
    void close();
    bool sharing() const;
    bool busy() const;
    bool watching() const;
    void stopWatching();
    bool focused() const;
};
