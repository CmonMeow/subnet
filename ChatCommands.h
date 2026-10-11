#pragma once

enum class ChatCommand { Name, Hosts, Host, Hostname, Dedicated, Connect, Clear, Disconnect, Users, Private, Kick, Ban, Unknown };
struct ChatCommandEntry { const char* name; ChatCommand id; const char* help; bool hostOnly; };
static constexpr ChatCommandEntry CHAT_COMMANDS[] = {
    {"/name", ChatCommand::Name, "/name username - change your name", false},
    {"/hosts", ChatCommand::Hosts, "/hosts - browse available hosts", false},
    {"/host", ChatCommand::Host, "/host - host on UDP port 777", false},
    {"/hostname", ChatCommand::Hostname, "/hostname name - set your host listing name", false},
    {"/dedicated", ChatCommand::Dedicated, "/dedicated [on|off] - host at startup", false},
    {"/connect", ChatCommand::Connect, "/connect [address] - connect", false},
    {"/clear", ChatCommand::Clear, "/clear - clear chat", false},
    {"/disconnect", ChatCommand::Disconnect, "/disconnect - leave or stop hosting", false},
    {"/users", ChatCommand::Users, "/users - list connected users", false},
    {"/pm", ChatCommand::Private, "/pm name|netId message - private message", false},
    {"/w", ChatCommand::Private, "/w name|netId message - private message", false},
    {"/tell", ChatCommand::Private, "/tell name|netId message - private message", false},
    {"/direct", ChatCommand::Private, "/direct name|netId message - private message", false},
    {"/kick", ChatCommand::Kick, "/kick name|netId - kick player", true},
    {"/ban", ChatCommand::Ban, "/ban name|netId - ban player", true},
};

