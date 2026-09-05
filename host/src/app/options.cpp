// SPDX-License-Identifier: MIT

#include "app/options.h"

#include "windbgmcp/pipe_endpoint.h"

#include <optional>
#include <string_view>
#include <utility>

namespace wmh::app {

OptionsResult ParseOptions(const std::vector<std::string>& args,
                           const char* env_pipe) {
    std::optional<std::string> cli_pipe;
    bool show_help = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
            continue;
        }
        if (arg == "--pipe") {
            if (cli_pipe) return {false, false, {}, "--pipe may be specified only once"};
            if (i + 1 >= args.size()) {
                return {false, false, {}, "--pipe requires a pipe name or endpoint"};
            }
            cli_pipe = args[++i];
            continue;
        }
        constexpr std::string_view kPipeEquals = "--pipe=";
        if (arg.starts_with(kPipeEquals)) {
            if (cli_pipe) return {false, false, {}, "--pipe may be specified only once"};
            cli_pipe = std::string(arg.substr(kPipeEquals.size()));
            continue;
        }
        return {false, false, {}, "unknown argument: " + std::string(arg)};
    }

    if (show_help) {
        return {true, true, {windbgmcp::kDefaultPipeEndpoint}, {}};
    }

    windbgmcp::PipeEndpointResult parsed;
    if (cli_pipe) {
        parsed = windbgmcp::NormalizePipeEndpoint(*cli_pipe, /*allow_default=*/false);
    } else if (env_pipe && *env_pipe) {
        parsed = windbgmcp::NormalizePipeEndpoint(env_pipe, /*allow_default=*/true);
    } else {
        parsed = windbgmcp::NormalizePipeEndpoint({}, /*allow_default=*/true);
    }
    if (!parsed.ok) {
        const std::string source = cli_pipe ? "--pipe" : windbgmcp::kPipeEnvironmentVariable;
        return {false, false, {}, source + ": " + parsed.error};
    }
    return {true, false, {std::move(parsed.endpoint)}, {}};
}

std::string UsageText() {
    return
        "usage: windbg-mcp.exe [--pipe <pipe-name|\\\\.\\pipe\\pipe-name>]\n"
        "\n"
        "Pipe selection precedence:\n"
        "  1. --pipe command-line option\n"
        "  2. WINDBGMCP_PIPE environment variable\n"
        "  3. \\\\.\\pipe\\windbgmcp (default)\n";
}

} // namespace wmh::app
