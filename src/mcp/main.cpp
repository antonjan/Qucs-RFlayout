// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "simulation.hpp"
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
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("qucsrflayout-mcp");
    QCoreApplication::setApplicationVersion(QRFL_VERSION);
    QCommandLineParser options;
    options.setApplicationDescription("Qucs-RFlayout MCP server over stdin/stdout");
    options.addHelpOption();
    options.addVersionOption();
    options.addOption({"octave", "GNU Octave executable (default: octave-cli)", "executable", "octave-cli"});
    options.addOption({"openems-prefix", "OpenEMS installation prefix (default: OPENEMS_PREFIX or detected installation)", "directory"});
    options.process(app);
    if (!options.positionalArguments().isEmpty()) {
        std::cerr << "Unexpected positional arguments\n";
        return 1;
    }
    auto prefix = options.isSet("openems-prefix") ? options.value("openems-prefix") : qEnvironmentVariable("OPENEMS_PREFIX");
    if (prefix.isEmpty() && !options.isSet("openems-prefix")) {
        for (const auto& candidate : QStringList{QDir::homePath() + "/opt/openEMS", "/usr/local", "/usr"}) {
            if (QFileInfo::exists(candidate + "/share/openEMS/matlab/InitFDTD.m") &&
                QFileInfo::exists(candidate + "/share/CSXCAD/matlab/InitCSX.m")) {
                prefix = candidate;
                break;
            }
        }
    }
    SimulationJobs jobs(options.value("octave"), prefix.isEmpty() ? QString() : QString::fromStdString(fs::absolute(prefix.toStdString()).string()));
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
    auto dispatch = [&](const std::string& line) {
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parse);
        if (parse.error != QJsonParseError::NoError) { error(QJsonValue::Null, -32700, "Parse error"); return; }
        if (!doc.isObject()) { error(QJsonValue::Null, -32600, "Invalid request"); return; }
        const auto request = doc.object();
        const auto id = request.value("id");
        const bool notification = !request.contains("id");
        if (request.value("jsonrpc") != "2.0" || !request.value("method").isString() ||
            (!notification && !id.isString() && !id.isDouble()) ||
            (request.contains("params") && !request.value("params").isObject())) {
            error(QJsonValue::Null, -32600, "Invalid request"); return;
        }
        const auto method = request.value("method").toString();
        if (notification) {
            if (method == "notifications/initialized" && initialized) ready = true;
            return;
        }
        const auto params = request.value("params").toObject();
        Object result;
        if (method == "initialize") {
            if (initialized) { error(id, -32600, "Already initialized"); return; }
            if (!params.value("protocolVersion").isString() || !params.value("capabilities").isObject() || !params.value("clientInfo").isObject()) {
                error(id, -32602, "Missing initialization parameters"); return;
            }
            auto version = params.value("protocolVersion").toString();
            if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18") version = "2025-06-18";
            initialized = true;
            result = {{"protocolVersion", version}, {"capabilities", Object{{"tools", Object{}}}},
                {"serverInfo", Object{{"name", "qucsrflayout-mcp"}, {"version", QRFL_VERSION}}}};
        } else if (method == "ping") {
            result = Object{};
        } else if (!ready) {
            error(id, -32600, "Complete MCP initialization first"); return;
        } else if (method == "tools/list") {
            auto list = jobs.tools();
            list.prepend(tool());
            result = {{"tools", list}};
        } else if (method == "tools/call") {
            const auto name = params.value("name").toString();
            if (name != "convert_layout" && name != "start_simulation" && name != "simulation_status" &&
                name != "simulation_results" && name != "cancel_simulation") {
                error(id, -32602, "Unknown tool"); return;
            }
            if (!params.value("arguments").isObject()) { error(id, -32602, "arguments must be an object"); return; }
            try {
                const auto arguments = params.value("arguments").toObject();
                result = name == "convert_layout" ? convert(arguments) : jobs.call(name, arguments);
            }
            catch (const std::exception& exception) { result = toolResult(QString::fromUtf8(exception.what()), true); }
        } else {
            error(id, -32601, "Method not found"); return;
        }
        send(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    };
    // Only input reading blocks. All dispatch and QProcess callbacks run on the
    // Qt event loop, which stays active even while the client sends no messages.
    std::thread input([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            QMetaObject::invokeMethod(&app, [&, line] { dispatch(line); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    const int exitCode = app.exec();
    input.join();
    jobs.shutdown();
    return exitCode;
}
