// SPDX-License-Identifier: MIT
//
// What the operator display may ask of the acquisition service, and how it
// learns the answer.
//
// It asks. Whether a session opens, and what is in it, is decided by the
// service: this class builds a request with the framework's codec, sends it
// over the framework's channel and reports what came back, refusals included
// and worded as the service worded them. It holds no session of its own - what
// it shows as "recording" is what the service last said, and it asks again
// every second, so a session opened or closed from a command line shows here
// too.

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <map>
#include <string>

class SessionControl : public QObject {
    Q_OBJECT

    /// False while the service cannot be reached. Not an error state for a
    /// display - the service may be starting, or restarting under us.
    Q_PROPERTY(bool reachable READ reachable NOTIFY changed)
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    /// The session the service says it is recording; empty when idle.
    Q_PROPERTY(QString session READ session NOTIFY changed)
    /// Frames the service has written to that session.
    Q_PROPERTY(qulonglong frames READ frames NOTIFY changed)
    /// Seconds since this display first saw the session open. Its own clock,
    /// for the operator's eye; the record's duration is the record's.
    Q_PROPERTY(int elapsedSeconds READ elapsedSeconds NOTIFY changed)
    /// What the last request came to, in the service's words.
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool refused READ refused NOTIFY changed)

public:
    explicit SessionControl(QString socketPath, QObject* parent = nullptr);

    bool reachable() const { return reachable_; }
    bool recording() const { return recording_; }
    QString session() const { return session_; }
    qulonglong frames() const { return frames_; }
    int elapsedSeconds() const { return elapsedSeconds_; }
    QString message() const { return message_; }
    bool refused() const { return refused_; }

    /// Asks for a session. The two names are what the operator typed, sent as
    /// typed; the service records them as affirmed, not as verified.
    Q_INVOKABLE void start(const QString& operatorName, const QString& exam);
    Q_INVOKABLE void stop();

signals:
    void changed();

private slots:
    void refresh();

private:
    struct Answer {
        bool delivered = false;
        bool accepted = false;
        QString detail;
    };
    Answer ask(const std::map<std::string, std::string>& request);
    void applyStatus(const Answer& answer);

    QString socketPath_;
    QTimer timer_;
    bool reachable_ = false;
    bool recording_ = false;
    QString session_;
    qulonglong frames_ = 0;
    int elapsedSeconds_ = 0;
    QString message_;
    bool refused_ = false;
};
