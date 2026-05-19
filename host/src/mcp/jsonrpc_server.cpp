// SPDX-License-Identifier: MIT
#include "mcp/jsonrpc_server.h"

#include "util/log.h"

namespace wmh::mcp {

using json = nlohmann::json;

namespace {
constexpr int kErrParse        = -32700;
constexpr int kErrInvalidReq   = -32600;
constexpr int kErrMethodNot    = -32601;
constexpr int kErrInvalidPar   = -32602;
constexpr int kErrInternal     = -32603;

constexpr const char* kProtocolVersion = "2024-11-05";
} // namespace

JsonRpcServer::JsonRpcServer(StdioServer& io, tools::Dispatcher& disp)
    : m_io(io), m_disp(disp) {}

void JsonRpcServer::OnLine(const std::string& line) {
    json req;
    try {
        req = json::parse(line);
    } catch (const std::exception&) {
        SendError(nullptr, kErrParse, "invalid JSON");
        return;
    }
    if (!req.is_object() || req.value("jsonrpc", "") != "2.0") {
        SendError(req.value("id", json(nullptr)), kErrInvalidReq, "not a JSON-RPC 2.0 message");
        return;
    }
    const std::string method = req.value("method", "");
    const json id     = req.contains("id") ? req["id"] : json(nullptr);
    const json params = req.value("params", json::object());

    // Notifications (no id) are fire-and-forget; we don't respond.
    const bool is_notification = !req.contains("id");

    try {
        if (method == "initialize") {
            json r = HandleInitialize(params);
            if (!is_notification) SendResult(id, r);
            return;
        }
        if (method == "tools/list") {
            json r = HandleToolsList();
            if (!is_notification) SendResult(id, r);
            return;
        }
        if (method == "tools/call") {
            json r = HandleToolsCall(params);
            if (!is_notification) SendResult(id, r);
            return;
        }
        // notifications/initialized, notifications/cancelled — ignore.
        if (method.rfind("notifications/", 0) == 0) {
            return;
        }
        if (!is_notification) {
            SendError(id, kErrMethodNot, "method not found: " + method);
        }
    } catch (const std::exception& e) {
        if (!is_notification) {
            SendError(id, kErrInternal, std::string("internal: ") + e.what());
        }
    }
}

json JsonRpcServer::HandleInitialize(const json& /*params*/) {
    return json{
        {"protocolVersion", kProtocolVersion},
        {"capabilities", {
            {"tools", json::object()},
        }},
        {"serverInfo", {
            {"name",    "windbg-mcp"},
            {"version", "1.0.0"},
        }},
    };
}

json JsonRpcServer::HandleToolsList() {
    return json{{"tools", m_disp.Descriptors()}};
}

json JsonRpcServer::HandleToolsCall(const json& params) {
    const std::string name = params.value("name", "");
    const json args = params.value("arguments", json::object());

    auto result = m_disp.Call(name, args);
    // MCP tools/call response shape: { content: [{type:"text", text:"..."}], isError? }
    json content_arr = json::array();
    content_arr.push_back(json{
        {"type", "text"},
        {"text", result.text},
    });
    json resp = {{"content", content_arr}};
    if (result.is_error) resp["isError"] = true;
    return resp;
}

void JsonRpcServer::SendResult(const json& id, const json& result) {
    json msg = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"result",  result},
    };
    m_io.Write(msg);
}

void JsonRpcServer::SendError(const json& id, int code, const std::string& msg,
                              const json& data) {
    json err = {{"code", code}, {"message", msg}};
    if (!data.is_null()) err["data"] = data;
    json out = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"error",   err},
    };
    m_io.Write(out);
}

} // namespace wmh::mcp
