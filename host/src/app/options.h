// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace wmh::app {

struct Options {
    std::string pipe_endpoint;
};

struct OptionsResult {
    bool        ok = false;
    bool        show_help = false;
    Options     options;
    std::string error;
};

// Parse arguments excluding argv[0]. cli --pipe wins over env_pipe; an empty
// or absent environment value falls back to the default endpoint.
OptionsResult ParseOptions(const std::vector<std::string>& args,
                           const char* env_pipe);

std::string UsageText();

} // namespace wmh::app
