// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <memory>
#include <map>

class SimulationJobs : public QObject {
public:
    SimulationJobs(QString octave, QString openemsPrefix);
    ~SimulationJobs() override;
    QJsonArray tools() const;
    QJsonObject call(const QString& name, const QJsonObject& arguments);
    void shutdown();
private:
    struct Job;
    QString octave_;
    QString prefix_;
    std::map<QString, std::unique_ptr<Job>> jobs_;
    QJsonObject start(const QJsonObject& arguments);
    Job& find(const QJsonObject& arguments);
    QJsonObject status(Job& job, int tailLines = 30) const;
    QJsonObject results(Job& job) const;
    void stop(Job& job, const QString& reason);
    void finish(Job& job, int exitCode, bool crashed);
    void persist(const Job& job) const;
};
