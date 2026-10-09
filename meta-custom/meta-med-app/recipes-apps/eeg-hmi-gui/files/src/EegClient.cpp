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

    electrodeOff_ = false;  // no frames, no contact status: unknown, not off
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

    // A display that draws a corrupt frame is worse than one that draws
    // nothing: the operator cannot tell the difference. The framework's
    // decoder is the one the acquisition side uses, so the two cannot
    // disagree about what a valid frame is.
    double rateHz = 0.0;
    med::Result<med::SampleFrame> decoded =
        med::amp::decodeFrame(buffer, received.value(), &rateHz);
    if (!decoded) {
        ++framesRejected_;
        emit frameReceived();
        return;
    }
    const med::SampleFrame& frame = decoded.value();

    if (channelCount_ != frame.channelCount) {
        channelCount_ = frame.channelCount;
        history_.clear();
        history_.resize(channelCount_);
    }
    sampleRateHz_ = rateHz;

    for (quint32 sample = 0; sample < frame.samplesPerChannel; ++sample) {
        for (int channel = 0; channel < channelCount_; ++channel) {
            QVector<double>& trace = history_[channel];
            trace.append(frame.at(static_cast<std::uint16_t>(channel), sample));
            if (trace.size() > windowSamples_) {
                trace.remove(0, trace.size() - windowSamples_);
            }
        }
    }

    // Electrode contact goes in the status line the operator already reads.
    // Only monitored electrodes are named: "nothing detached" is said only
    // where something was checked.
    QString contact;
    for (int channel = 0; channel < 16; ++channel) {
        const unsigned bit = 1U << channel;
        if ((frame.leadOff.detachedPositive() & bit) != 0) {
            contact += QStringLiteral(" %1+").arg(channel + 1);
        }
        if ((frame.leadOff.detachedNegative() & bit) != 0) {
            contact += QStringLiteral(" %1-").arg(channel + 1);
        }
    }

    QString status = QStringLiteral("acquiring");
    if (lastSequence_ != 0 && frame.sequence != lastSequence_ + 1) {
        status = QStringLiteral("acquiring (gap after frame %1)").arg(lastSequence_);
    }
    if (!contact.isEmpty()) {
        status += QStringLiteral(" - ELECTRODE OFF:") + contact;
    }
    // Before setStatus(), whose signal also notifies this property.
    electrodeOff_ = !contact.isEmpty();
    if (status_ != status) {
        setStatus(status);
    }
    lastSequence_ = frame.sequence;

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
