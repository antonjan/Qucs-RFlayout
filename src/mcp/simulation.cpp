// SPDX-License-Identifier: GPL-3.0-or-later
#include "simulation.hpp"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace {
using Object = QJsonObject;
using Array = QJsonArray;
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString stringArg(const Object& args, const QString& key) {
    if (!args.value(key).isString() || args.value(key).toString().isEmpty())
        throw std::invalid_argument((key + " must be a nonempty string").toStdString());
    const auto value = args.value(key).toString();
    for (const auto ch : value)
        if (ch.unicode() < 32) throw std::invalid_argument("Control characters are not allowed in paths or job IDs");
    return value;
}
int intArg(const Object& args, const QString& key, int fallback, int low, int high) {
    if (!args.contains(key)) return fallback;
    const auto value = args.value(key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || number < low || number > high || number != static_cast<int>(number))
        throw std::invalid_argument((key + " must be an integer between " + QString::number(low) + " and " + QString::number(high)).toStdString());
    return static_cast<int>(number);
}
void keys(const Object& args, const QStringList& allowed) {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!allowed.contains(it.key())) throw std::invalid_argument("Unknown argument: " + it.key().toStdString());
}
QString quoted(QString value) { return "'" + value.replace("'", "''") + "'"; }
void writeFile(const QString& path, const QByteArray& data) {
    // Named temporary files work on filesystems/sandboxes without O_TMPFILE.
    const auto temporary = std::filesystem::path(path.toStdString() + ".tmp");
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(data.constData(), data.size());
    file.close();
    if (!file) throw std::runtime_error("Cannot write " + path.toStdString());
    std::filesystem::rename(temporary, std::filesystem::path(path.toStdString()));
}
Object result(const Object& payload) {
    return {{"content", Array{Object{{"type", "text"},
        {"text", QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))}}}}, {"isError", false}};
}
Object schema(Object properties, Array required) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QString logTail(const QString& path, int lines) {
    if (!lines) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    // Read at most 64 KiB, never the entire simulation log into memory.
    const qint64 begin = qMax(qint64(0), file.size() - 65536);
    file.seek(begin);
    if (begin) file.readLine();
    auto parts = QString::fromUtf8(file.readAll()).split('\n');
    if (!parts.isEmpty() && parts.back().isEmpty()) parts.removeLast();
    return parts.mid(qMax(0, parts.size() - lines)).join('\n');
}
} // namespace

struct SimulationJobs::Job {
    QString id, state = "starting", reason, error, directory, script, log;
    QString startedAt = now(), finishedAt;
    QElapsedTimer elapsed;
    QProcess process;
    QTimer timeout, killTimer;
    qint64 processGroup = 0;
    int exitCode = 0, timeoutSeconds = 3600;
    qint64 durationMs = -1;
    bool hasExitCode = false, preprocessOnly = false;
    bool active() const { return state == "starting" || state == "running" || state == "cancelling"; }
};

SimulationJobs::SimulationJobs(QString octave, QString openemsPrefix)
    : octave_(std::move(octave)), prefix_(std::move(openemsPrefix)) {}
SimulationJobs::~SimulationJobs() { shutdown(); }

Array SimulationJobs::tools() const {
    const Object jobProperties{{"job_id", Object{{"type", "string"}}}};
    Object statusProperties = jobProperties;
    statusProperties["log_tail_lines"] = Object{{"type", "integer"}, {"minimum", 0}, {"maximum", 200}, {"default", 30}};
    return {
        Object{{"name", "start_simulation"},
            {"description", "Launch a trusted local .m script with GNU Octave/OpenEMS in the background. Returns a job ID immediately. Requires a new output directory; only one simulation may run at a time."},
            {"inputSchema", schema(Object{
                {"script", Object{{"type", "string"}}},
                {"output_directory", Object{{"type", "string"}}},
                {"timeout_seconds", Object{{"type", "integer"}, {"minimum", 1}, {"maximum", 86400}, {"default", 3600}}},
                {"preprocess_only", Object{{"type", "boolean"}, {"default", false}}}}, Array{"script", "output_directory"})}},
        Object{{"name", "simulation_status"}, {"description", "Get a simulation job's state, elapsed time, exit code and recent log output."},
            {"inputSchema", schema(statusProperties, Array{"job_id"})}},
        Object{{"name", "simulation_results"}, {"description", "List generated plots, S-parameter files and other artifacts for a simulation job. Results from unfinished or failed jobs are marked partial."},
            {"inputSchema", schema(jobProperties, Array{"job_id"})}},
        Object{{"name", "cancel_simulation"}, {"description", "Stop a simulation job and its child processes. Completed jobs are left unchanged."},
            {"inputSchema", schema(jobProperties, Array{"job_id"})}}
    };
}

Object SimulationJobs::call(const QString& name, const Object& args) {
    if (name == "start_simulation") return result(start(args));
    if (name == "simulation_status") {
        keys(args, {"job_id", "log_tail_lines"});
        return result(status(find(args), intArg(args, "log_tail_lines", 30, 0, 200)));
    }
    keys(args, {"job_id"});
    auto& job = find(args);
    if (name == "simulation_results") return result(results(job));
    if (name == "cancel_simulation") {
        stop(job, "cancelled");
        return result(status(job));
    }
    throw std::invalid_argument("Unknown simulation tool");
}

Object SimulationJobs::start(const Object& args) {
    keys(args, {"script", "output_directory", "timeout_seconds", "preprocess_only"});
    for (const auto& [id, job] : jobs_)
        if (job->active()) throw std::runtime_error("A simulation is already active: " + id.toStdString());
    const auto scriptInfo = QFileInfo(stringArg(args, "script"));
    if (!scriptInfo.isFile() || scriptInfo.suffix() != "m")
        throw std::invalid_argument("script must be an existing .m file");
    if (scriptInfo.size() > 16 * 1024 * 1024) throw std::invalid_argument("Input script exceeds 16 MiB");
    const auto script = scriptInfo.canonicalFilePath();
    const auto directory = QDir::cleanPath(QFileInfo(stringArg(args, "output_directory")).absoluteFilePath());
    if (QFileInfo::exists(directory) || QFileInfo(directory).isSymLink())
        throw std::invalid_argument("Output directory must not already exist; use a new directory");
    const auto timeout = intArg(args, "timeout_seconds", 3600, 1, 86400);
    if (args.contains("preprocess_only") && !args.value("preprocess_only").isBool())
        throw std::invalid_argument("preprocess_only must be a boolean");
    // Resolve before changing the child's working directory. No shell command is built.
    const auto executable = QStandardPaths::findExecutable(octave_);
    if (executable.isEmpty()) throw std::runtime_error("GNU Octave executable not found: " + octave_.toStdString() + ". Install Octave or configure --octave.");
    if (!prefix_.isEmpty() && (!QFileInfo::exists(prefix_ + "/share/openEMS/matlab/InitFDTD.m") ||
        !QFileInfo::exists(prefix_ + "/share/CSXCAD/matlab/InitCSX.m")))
        throw std::runtime_error("--openems-prefix must contain the OpenEMS and CSXCAD Octave interfaces");
    // create_directory is exclusive; parents can be created without reusing an existing job folder.
    std::filesystem::create_directories(QFileInfo(directory).absolutePath().toStdString());
    if (!std::filesystem::create_directory(directory.toStdString()))
        throw std::runtime_error("Cannot create a new output directory");
    auto owned = std::make_unique<Job>();
    auto& job = *owned;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.directory = directory;
    job.script = script;
    job.log = directory + "/simulation.log";
    job.timeoutSeconds = timeout;
    job.preprocessOnly = args.value("preprocess_only").toBool(false);
    job.elapsed.start();
    const auto staged = directory + "/job_input.m";
    QFile input(script);
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read input script: " + input.errorString().toStdString());
    writeFile(staged, input.readAll());
    // source(), unlike run(), keeps the job working directory. Octave strings
    // escape apostrophes by doubling them; shell metacharacters stay literal.
    QString wrapper = "1;\nset(0, 'defaultfigurevisible', 'off');\n";
    wrapper += "addpath(" + quoted(QFileInfo(script).absolutePath()) + ");\n";
    if (!prefix_.isEmpty()) {
        wrapper += "addpath(" + quoted(prefix_ + "/share/openEMS/matlab") + ");\n";
        wrapper += "addpath(" + quoted(prefix_ + "/share/CSXCAD/matlab") + ");\n";
    }
    wrapper += "try\n  source(" + quoted(staged) + ", 'base');\ncatch simulation_error\n";
    wrapper += "  fprintf(2, 'Simulation failed: %s\\n', simulation_error.message);\n  exit(1);\nend\n";
    const auto runner = directory + "/runner.m";
    writeFile(runner, wrapper.toUtf8());
    job.process.setWorkingDirectory(directory);
    job.process.setStandardInputFile(QProcess::nullDevice());
    job.process.setProcessChannelMode(QProcess::MergedChannels);
    job.process.setStandardOutputFile(job.log);
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!prefix_.isEmpty()) environment.insert("OPENEMS_PREFIX", prefix_);
    job.process.setProcessEnvironment(environment);
#ifdef Q_OS_UNIX
    // Give Octave and all solver descendants their own process group.
    job.process.setChildProcessModifier([] { if (::setsid() < 0) ::_exit(127); });
#endif
    job.timeout.setSingleShot(true);
    job.killTimer.setSingleShot(true);
    QObject::connect(&job.process, &QProcess::started, this, [this, &job] {
        job.processGroup = job.process.processId();
        if (job.reason.isEmpty()) job.state = "running";
        job.timeout.start(job.timeoutSeconds * 1000);
        persist(job);
        if (!job.reason.isEmpty()) stop(job, job.reason);
    });
    QObject::connect(&job.process, &QProcess::errorOccurred, this, [this, &job](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            job.error = job.process.errorString();
            finish(job, -1, true);
        }
    });
    QObject::connect(&job.process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, &job](int code, QProcess::ExitStatus state) { finish(job, code, state == QProcess::CrashExit); });
    QObject::connect(&job.timeout, &QTimer::timeout, this, [this, &job] { stop(job, "timed_out"); });
    QObject::connect(&job.killTimer, &QTimer::timeout, this, [&job] {
#ifdef Q_OS_UNIX
        if (job.processGroup > 0) ::kill(-static_cast<pid_t>(job.processGroup), SIGKILL);
#endif
        if (job.process.state() != QProcess::NotRunning) job.process.kill();
    });
    persist(job);
    jobs_.emplace(job.id, std::move(owned));
    QStringList arguments{"--quiet", "--no-gui", "--no-window-system", runner, "--no-gui", "--batch"};
    if (job.preprocessOnly) arguments.append("--only-preprocess");
    job.process.start(executable, arguments);
    return status(job);
}

SimulationJobs::Job& SimulationJobs::find(const Object& args) {
    const auto id = stringArg(args, "job_id");
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) throw std::invalid_argument("Unknown job ID: " + id.toStdString());
    return *it->second;
}

Object SimulationJobs::status(Job& job, int tailLines) const {
    const qint64 elapsed = job.durationMs < 0 ? job.elapsed.elapsed() : job.durationMs;
    Object value{{"job_id", job.id}, {"state", job.state}, {"script", job.script},
        {"output_directory", job.directory}, {"log_file", job.log}, {"log_tail", logTail(job.log, tailLines)},
        {"started_at", job.startedAt}, {"elapsed_seconds", elapsed / 1000.0},
        {"timeout_seconds", job.timeoutSeconds}, {"preprocess_only", job.preprocessOnly}};
    if (!job.finishedAt.isEmpty()) value["finished_at"] = job.finishedAt;
    if (job.hasExitCode) value["exit_code"] = job.exitCode;
    if (!job.error.isEmpty()) value["error"] = job.error;
    if (!job.reason.isEmpty()) value["stop_reason"] = job.reason;
    return value;
}

Object SimulationJobs::results(Job& job) const {
    auto value = status(job);
    Array artifacts;
    bool truncated = false;
    QDirIterator it(job.directory, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const auto info = it.fileInfo();
        if (info.isSymLink()) continue;
        const auto suffix = info.suffix().toLower();
        QString kind;
        if (QStringList{"png", "svg", "pdf", "jpg", "gif"}.contains(suffix)) kind = "plot";
        else if (suffix.startsWith('s') && suffix.endsWith('p')) kind = "s_parameters";
        else if (QStringList{"csv", "dat", "h5", "hdf5", "vtk", "vtu", "mat"}.contains(suffix)) kind = "data";
        else if (suffix == "xml" || suffix == "csx") kind = "geometry";
        else continue;
        if (artifacts.size() == 2000) { truncated = true; break; }
        artifacts.append(Object{{"path", info.absoluteFilePath()}, {"kind", kind}, {"size_bytes", info.size()}});
    }
    value["files"] = artifacts;
    value["files_truncated"] = truncated;
    value["partial"] = job.state != "completed";
    // Even successful preprocessing does not establish simulation convergence.
    value["simulation_execution_requested"] = !job.preprocessOnly;
    return value;
}

void SimulationJobs::stop(Job& job, const QString& reason) {
    if (!job.active()) return;
    if (job.reason.isEmpty()) job.reason = reason;
    job.state = "cancelling";
    job.timeout.stop();
#ifdef Q_OS_UNIX
    if (job.processGroup > 0) ::kill(-static_cast<pid_t>(job.processGroup), SIGTERM);
#endif
    if (job.process.state() == QProcess::Running) job.process.terminate();
    if (!job.killTimer.isActive()) job.killTimer.start(3000);
    persist(job);
}

void SimulationJobs::finish(Job& job, int exitCode, bool crashed) {
    job.timeout.stop();
    job.killTimer.stop();
#ifdef Q_OS_UNIX
    // The parent can exit before its solver. Clean up its process group as well.
    if (job.processGroup > 0) ::kill(-static_cast<pid_t>(job.processGroup), SIGKILL);
#endif
    job.processGroup = 0;
    job.exitCode = exitCode;
    job.hasExitCode = true;
    job.finishedAt = now();
    job.durationMs = job.elapsed.elapsed();
    job.state = !job.reason.isEmpty() ? job.reason : (!crashed && exitCode == 0 ? "completed" : "failed");
    persist(job);
}

void SimulationJobs::persist(const Job& job) const {
    const Object value{{"job_id", job.id}, {"state", job.state}, {"script", job.script},
        {"output_directory", job.directory}, {"started_at", job.startedAt}, {"finished_at", job.finishedAt},
        {"timeout_seconds", job.timeoutSeconds}, {"preprocess_only", job.preprocessOnly},
        {"stop_reason", job.reason}, {"error", job.error},
        {"exit_code", job.hasExitCode ? QJsonValue(job.exitCode) : QJsonValue(QJsonValue::Null)}};
    // Status persistence failure should not throw out of a Qt signal callback.
    try { writeFile(job.directory + "/job.json", QJsonDocument(value).toJson()); }
    catch (const std::exception& e) { qWarning("Cannot persist simulation job: %s", e.what()); }
}

void SimulationJobs::shutdown() {
    for (auto& [id, job] : jobs_) {
        if (!job->active()) continue;
        stop(*job, "cancelled");
        if (!job->process.waitForFinished(3000)) {
#ifdef Q_OS_UNIX
            if (job->processGroup > 0) ::kill(-static_cast<pid_t>(job->processGroup), SIGKILL);
#endif
            job->process.kill();
            job->process.waitForFinished(1000);
        }
    }
}
