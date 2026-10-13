// SPDX-License-Identifier: GPL-3.0-or-later
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "converter.hpp"

namespace fs = std::filesystem;
using Object = QJsonObject;
using Array = QJsonArray;

static QString requiredString(const Object& args, const QString& key) {
    if (!args.value(key).isString() || args.value(key).toString().isEmpty())
        throw std::invalid_argument((key + " must be a nonempty string").toStdString());
    return args.value(key).toString();
}

static Object toolResult(const QString& text, bool error = false) {
    return {{"content", Array{Object{{"type", "text"}, {"text", text}}}}, {"isError", error}};
}

static Object tool() {
    Object properties;
    for (const auto& key : {"schematic", "netlist", "output_directory"})
        properties[key] = Object{{"type", "string"}};
    properties["format"] = Object{{"type", "string"}, {"enum", Array{
        ".kicad_pcb", ".kicad_mod", ".lht", ".m", ".svg"}}, {"default", ".kicad_pcb"}};
    return {{"name", "convert_layout"},
        {"description", "Convert a local Qucs .sch schematic and existing .net netlist into a layout. Writes files into a new output directory. Does not run simulations."},
        {"inputSchema", Object{{"type", "object"}, {"properties", properties},
            {"required", Array{"schematic", "netlist", "output_directory"}}, {"additionalProperties", false}}}};
}

static Object convert(const Object& args) {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "schematic" && it.key() != "netlist" && it.key() != "output_directory" && it.key() != "format")
            throw std::invalid_argument("Unknown argument: " + it.key().toStdString());
    Data data;
    data.n_sch = fs::canonical(requiredString(args, "schematic").toStdString());
    data.n_net = fs::canonical(requiredString(args, "netlist").toStdString());
    data.out_dir = fs::absolute(requiredString(args, "output_directory").toStdString());
    if (!fs::is_regular_file(data.n_sch) || data.n_sch.extension() != ".sch" || !fs::is_regular_file(data.n_net))
        throw std::invalid_argument("Expected a .sch schematic and a regular netlist file");
    std::ifstream schematic(data.n_sch);
    std::string header;
    std::getline(schematic, header);
    if (!header.starts_with("<Qucs Schematic"))
        throw std::invalid_argument("Only Qucs schematics are supported by this MCP tool");
    const auto format = args.contains("format") ? requiredString(args, "format") : QString(".kicad_pcb");
    if (format != ".kicad_pcb" && format != ".kicad_mod" && format != ".lht" && format != ".m" && format != ".svg")
        throw std::invalid_argument("Unsupported output format");
    data.out_format = format.toStdString();
    // The legacy parser uses shell commands for temporary-file cleanup. Supplying
    // a netlist and keeping temporary files avoids those paths entirely.
    data.keep_tmp_files = true;
    if (fs::exists(data.out_dir))
        throw std::invalid_argument("Output directory must not already exist; use a new directory for every conversion");
    Converter converter(data);
    if (converter.read()) return toolResult("Schematic conversion failed; see server stderr for diagnostics", true);
    fs::create_directories(data.out_dir);
    std::vector<std::string> files;
    if (converter.write(files)) return toolResult("Layout export failed; output may be incomplete. See server stderr", true);
    Array paths;
    for (const auto& file : files) paths.append(QString::fromStdString(fs::absolute(file).string()));
    return toolResult(QString::fromUtf8(QJsonDocument(Object{{"files", paths}, {"format", format}}).toJson(QJsonDocument::Compact)));
}

int main(int argc, char* argv[]) {
    if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << " (MCP JSON-RPC over stdin/stdout)\n";
        return 1;
    }
    // Legacy conversion diagnostics go to stderr; stdout is exclusively MCP.
    std::ostream protocol(std::cout.rdbuf());
    std::cout.rdbuf(std::cerr.rdbuf());
    bool initialized = false, ready = false;
    auto send = [&](const Object& message) {
        protocol << QJsonDocument(message).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
    };
    auto error = [&](const QJsonValue& id, int code, const QString& message) {
        send(Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", Object{{"code", code}, {"message", message}}}});
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parse);
        if (parse.error != QJsonParseError::NoError) { error(QJsonValue::Null, -32700, "Parse error"); continue; }
        if (!doc.isObject()) { error(QJsonValue::Null, -32600, "Invalid request"); continue; }
        const auto request = doc.object();
        const auto id = request.value("id");
        const bool notification = !request.contains("id");
        if (request.value("jsonrpc") != "2.0" || !request.value("method").isString() ||
            (!notification && !id.isString() && !id.isDouble()) ||
            (request.contains("params") && !request.value("params").isObject())) {
            error(QJsonValue::Null, -32600, "Invalid request"); continue;
        }
        const auto method = request.value("method").toString();
        if (notification) {
            if (method == "notifications/initialized" && initialized) ready = true;
            continue;
        }
        const auto params = request.value("params").toObject();
        Object result;
        if (method == "initialize") {
            if (initialized) { error(id, -32600, "Already initialized"); continue; }
            if (!params.value("protocolVersion").isString() || !params.value("capabilities").isObject() || !params.value("clientInfo").isObject()) {
                error(id, -32602, "Missing initialization parameters"); continue;
            }
            auto version = params.value("protocolVersion").toString();
            if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18") version = "2025-06-18";
            initialized = true;
            result = {{"protocolVersion", version}, {"capabilities", Object{{"tools", Object{}}}},
                {"serverInfo", Object{{"name", "qucsrflayout-mcp"}, {"version", QRFL_VERSION}}}};
        } else if (method == "ping") {
            result = Object{};
        } else if (!ready) {
            error(id, -32600, "Complete MCP initialization first"); continue;
        } else if (method == "tools/list") {
            result = {{"tools", Array{tool()}}};
        } else if (method == "tools/call") {
            if (params.value("name") != "convert_layout") { error(id, -32602, "Unknown tool"); continue; }
            if (!params.value("arguments").isObject()) { error(id, -32602, "arguments must be an object"); continue; }
            try { result = convert(params.value("arguments").toObject()); }
            catch (const std::exception& exception) { result = toolResult(QString::fromUtf8(exception.what()), true); }
        } else {
            error(id, -32601, "Method not found"); continue;
        }
        send(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }
    return 0;
}
