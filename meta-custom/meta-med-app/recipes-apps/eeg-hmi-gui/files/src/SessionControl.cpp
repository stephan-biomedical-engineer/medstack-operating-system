// SPDX-License-Identifier: MIT

#include "SessionControl.h"

#include <QStringList>

#include <chrono>
#include <memory>
#include <vector>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>

namespace {
// The service answers between two sample frames, so an answer is tens of
// milliseconds away. This is how long the display's own thread may wait for
// one before calling the service unreachable - short, because nothing is
// drawn while it waits.
constexpr std::chrono::milliseconds kAnswerTimeout(800);
}  // namespace

SessionControl::SessionControl(QString socketPath, QObject* parent)
    : QObject(parent), socketPath_(std::move(socketPath)) {
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &SessionControl::refresh);
    timer_.start();
    refresh();
}

SessionControl::Answer SessionControl::ask(const std::map<std::string, std::string>& request) {
    Answer answer;

    const med::Result<std::vector<unsigned char>> encoded = med::amp::encodeControl(request);
    if (!encoded) {
        // Said to the operator as a refusal, because that is what it is: a
        // name too long for the message is not sent short.
        answer.delivered = true;
        answer.detail = QString::fromStdString(encoded.message());
        return answer;
    }

    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = socketPath_.toStdString();
    med::Result<std::unique_ptr<med::MedicalIpcChannel>> channel =
        med::MedicalIpcChannel::connect(endpoint);
    if (!channel) {
        return answer;
    }
    if (channel.value()->send(encoded.value().data(), encoded.value().size()) != med::Status::Ok) {
        return answer;
    }
    unsigned char buffer[512];
    const med::Result<std::size_t> received =
        channel.value()->receive(buffer, sizeof(buffer), kAnswerTimeout);
    if (!received) {
        return answer;
    }
    const med::Result<med::amp::ControlAck> ack =
        med::amp::decodeControlAck(buffer, received.value());
    if (!ack) {
        return answer;
    }

    answer.delivered = true;
    answer.accepted = ack.value().rejectedIndex == 0;
    answer.detail = QString::fromUtf8(ack.value().detail);
    return answer;
}

void SessionControl::applyStatus(const Answer& answer) {
    reachable_ = answer.delivered && answer.accepted;
    if (!reachable_) {
        recording_ = false;
        session_.clear();
        frames_ = 0;
        elapsedSeconds_ = 0;
        return;
    }

    // "idle", or "recording <session> <frames>".
    const QStringList words = answer.detail.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const bool recording = words.size() >= 2 && words.at(0) == QLatin1String("recording");
    const QString session = recording ? words.at(1) : QString();
    if (recording && session == session_) {
        ++elapsedSeconds_;
    } else {
        elapsedSeconds_ = 0;
    }
    recording_ = recording;
    session_ = session;
    frames_ = recording && words.size() >= 3 ? words.at(2).toULongLong() : 0;
}

void SessionControl::refresh() {
    // What the screen is told, before: the service is asked every second and
    // answers the same thing almost every time, and an answer that changed
    // nothing must not make the engine re-evaluate everything bound to this
    // object. Measured on the board with the signal announced every second
    // regardless: 1.5 s of GUI thread in 20 s, against 0.4 s before this
    // object existed.
    const bool wasReachable = reachable_;
    const bool wasRecording = recording_;
    const QString wasSession = session_;
    const qulonglong wasFrames = frames_;
    const int wasElapsed = elapsedSeconds_;
    const int wasChoice = choice_;
    const QString wasMessage = message_;
    const bool wasRefused = refused_;

    applyStatus(ask({{"verb", "session.status"}}));

    // "choice <row>", or "unlisted".
    choice_ = -1;
    if (reachable_) {
        const Answer prescription = ask({{"verb", "device.prescription"}});
        const QStringList words =
            prescription.detail.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (prescription.delivered && prescription.accepted && words.size() == 2 &&
            words.at(0) == QLatin1String("choice")) {
            bool isNumber = false;
            const int row = words.at(1).toInt(&isNumber);
            choice_ = isNumber ? row : -1;
        }
    }

    if (wasReachable != reachable_ || wasRecording != recording_ || wasSession != session_ ||
        wasFrames != frames_ || wasElapsed != elapsedSeconds_ || wasChoice != choice_ ||
        wasMessage != message_ || wasRefused != refused_) {
        emit changed();
    }
}

void SessionControl::configure(const QVariantMap& settings) {
    std::map<std::string, std::string> request = {{"verb", "session.configure"}};
    for (QVariantMap::const_iterator it = settings.constBegin(); it != settings.constEnd(); ++it) {
        request[it.key().toStdString()] = it.value().toString().toStdString();
    }
    const Answer answer = ask(request);
    refused_ = !(answer.delivered && answer.accepted);
    message_ = !answer.delivered ? QStringLiteral("the acquisition service did not answer")
               : answer.accepted ? QStringLiteral("front-end settings applied")
                                 : answer.detail;
    refresh();
    emit changed();
}

void SessionControl::start(const QString& operatorName, const QString& exam) {
    std::map<std::string, std::string> request = {{"verb", "session.start"}};
    if (!operatorName.trimmed().isEmpty()) {
        request["operator"] = operatorName.trimmed().toStdString();
    }
    if (!exam.trimmed().isEmpty()) {
        request["exam"] = exam.trimmed().toStdString();
    }
    const Answer answer = ask(request);
    refused_ = !(answer.delivered && answer.accepted);
    message_ = !answer.delivered ? QStringLiteral("the acquisition service did not answer")
               : answer.accepted ? QStringLiteral("recording")
                                 : answer.detail;
    refresh();
    // Said whatever refresh() found: the answer to a request is news even
    // when it changed nothing - a refusal, most of all.
    emit changed();
}

void SessionControl::stop() {
    const Answer answer = ask({{"verb", "session.stop"}});
    refused_ = !(answer.delivered && answer.accepted);
    // "closed <session> <frames>" - said back as what it means for the
    // operator: where the record went, and that the clinical file follows.
    const QStringList words = answer.detail.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    message_ = !answer.delivered ? QStringLiteral("the acquisition service did not answer")
               : !answer.accepted ? answer.detail
               : words.size() >= 3
                     ? QStringLiteral("saved %1 (%2 frames) - the clinical file follows")
                           .arg(words.at(1), words.at(2))
                     : QStringLiteral("saved");
    refresh();
    // Said whatever refresh() found: the answer to a request is news even
    // when it changed nothing - a refusal, most of all.
    emit changed();
}
