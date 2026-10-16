#pragma once
#include <string>

// Only action is clickable; title and status are descriptive text.
struct ChatLink {
    std::string title, status, action;
    std::string text() const {
        return title + (status.empty()?"":" - "+status) + (action.empty()?"":" ["+action+"]");
    }
};
