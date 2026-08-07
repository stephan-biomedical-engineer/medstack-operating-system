// SPDX-License-Identifier: MIT

#include "EegClient.h"

#include <QSocketNotifier>

#include <cstring>

#include <medplatform/MedicalDevice.h>

EegClient::EegClient(QString socketPath, QObject* parent)
    : QObject(parent), socketPath_(std::move(socketPath)) {
    status_ = QStringLiteral("connecting to %1").arg(socketPath_);

    // The acquisition service may not have created its socket yet, and it may
    // restart under us. Neither is an error state for an HMI - it just keeps
    // trying and says so on screen.
    reconnectTimer_.setInterval(1000);
    connect(&reconnectTimer_, &QTimer::timeout, this, &EegClient::attemptConnection);
    reconnectTimer_.start();

    attemptConnection();
}

EegClient::~EegClient() = default;

void EegClient::setStatus(const QString& status) {
    if (status_ != status) {
        status_ = status;
        emit statusChanged();
    }
}

void EegClient::attemptConnection() {
    if (channel_) {
        return;
    }

    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = socketPath_.toStdString();

    med::Result<std::unique_ptr<med::MedicalIpcChannel>> channel =
        med::MedicalIpcChannel::connect(endpoint);
    if (!channel) {
        setStatus(QStringLiteral("waiting for the acquisition service (%1)")
                      .arg(QString::fromLatin1(med::toString(channel.status()))));
        return;
    }

    channel_ = channel.take();

    notifier_ = new QSocketNotifier(channel_->descriptor(), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &EegClient::readPending);

    lastSequence_ = 0;
    setStatus(QStringLiteral("acquiring"));
    emit connectionChanged();
}

void EegClient::dropConnection(const QString& reason) {
    if (notifier_ != nullptr) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    channel_.reset();

    setStatus(reason);
    emit connectionChanged();
}

void EegClient::readPending() {
    if (!channel_) {
        return;
    }

    unsigned char buffer[65536];
    med::Result<std::size_t> received =
        channel_->receive(buffer, sizeof(buffer), std::chrono::milliseconds(0));
    if (!received) {
        if (received.status() == med::Status::Timeout) {
            return;
        }
        dropConnection(QStringLiteral("acquisition service went away, reconnecting"));
        return;
    }

    const std::size_t length = received.value();
    if (length < sizeof(med::amp::FrameHeader)) {
        ++framesRejected_;
        return;
    }

    med::amp::FrameHeader header;
    std::memcpy(&header, buffer, sizeof(header));

    const std::size_t payloadBytes = static_cast<std::size_t>(header.channelCount) *
                                     header.samplesPerChannel * sizeof(qint32);

    // A display that draws a corrupt frame is worse than one that draws
    // nothing: the operator cannot tell the difference.
    if (header.magic != med::amp::kFrameMagic || header.version != med::amp::kFrameVersion ||
        payloadBytes == 0 || length < sizeof(med::amp::FrameHeader) + payloadBytes ||
        med::amp::crc32(buffer + sizeof(med::amp::FrameHeader), payloadBytes) != header.crc32) {
        ++framesRejected_;
        emit frameReceived();
        return;
    }

    if (channelCount_ != header.channelCount) {
        channelCount_ = header.channelCount;
        history_.clear();
        history_.resize(channelCount_);
    }
    sampleRateHz_ = header.sampleRateMilliHz / 1000.0;

    const unsigned char* payload = buffer + sizeof(med::amp::FrameHeader);
    const double scale = static_cast<double>(header.scaleNanoUnitsPerLsb) / 1000.0;

    for (quint32 sample = 0; sample < header.samplesPerChannel; ++sample) {
        for (int channel = 0; channel < channelCount_; ++channel) {
            const std::size_t index =
                static_cast<std::size_t>(sample) * header.channelCount +
                static_cast<std::size_t>(channel);
            qint32 raw = 0;
            std::memcpy(&raw, payload + index * sizeof(qint32), sizeof(raw));

            QVector<double>& trace = history_[channel];
            trace.append(raw * scale);
            if (trace.size() > windowSamples_) {
                trace.remove(0, trace.size() - windowSamples_);
            }
        }
    }

    if (lastSequence_ != 0 && header.sequence != lastSequence_ + 1) {
        setStatus(QStringLiteral("acquiring (gap after frame %1)").arg(lastSequence_));
    } else if (status_ != QStringLiteral("acquiring")) {
        setStatus(QStringLiteral("acquiring"));
    }
    lastSequence_ = header.sequence;

    ++framesReceived_;
    emit frameReceived();
}

QVariantList EegClient::waveform(int channel) const {
    QVariantList out;
    if (channel < 0 || channel >= history_.size()) {
        return out;
    }
    const QVector<double>& trace = history_.at(channel);
    out.reserve(trace.size());
    for (const double sample : trace) {
        out.append(sample);
    }
    return out;
}
