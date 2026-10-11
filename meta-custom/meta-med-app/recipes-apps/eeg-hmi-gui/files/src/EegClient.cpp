// SPDX-License-Identifier: MIT

#include "EegClient.h"

#include <QSocketNotifier>

#include <cmath>

namespace {
// How many frames one wakeup may take before the event loop gets its turn. At
// 250 samples a second a wakeup finds one; at the fastest rate a backlog can
// form, and draining it whole would starve the very repaint it is feeding.
constexpr int kFramesPerWakeup = 256;
// Four times a second: fast enough to read as live, slow enough that the text
// on the screen is not what the processor is spent on.
constexpr int kStatisticsIntervalMs = 250;
}  // namespace

EegClient::EegClient(QString socketPath, QObject* parent)
    : QObject(parent), socketPath_(std::move(socketPath)) {
    status_ = QStringLiteral("connecting to %1").arg(socketPath_);
    clock_.start();

    // The acquisition service may not have created its socket yet, and it may
    // restart under us. Neither is an error state for an HMI - it just keeps
    // trying and says so on screen.
    reconnectTimer_.setInterval(1000);
    connect(&reconnectTimer_, &QTimer::timeout, this, &EegClient::attemptConnection);
    reconnectTimer_.start();

    statisticsTimer_.setInterval(kStatisticsIntervalMs);
    connect(&statisticsTimer_, &QTimer::timeout, this, &EegClient::refreshStatistics);
    statisticsTimer_.start();

    attemptConnection();
}

EegClient::~EegClient() = default;

void EegClient::setStatus(const QString& status) {
    if (status_ != status) {
        status_ = status;
        emit statusChanged();
    }
}

void EegClient::reconfigure() {
    store_.configure(channelCount_, sampleRateHz_, windowSeconds_);
    store_.setFilters(highPassHz_, notchHz_);
    emit samplesAppended();
}

void EegClient::setWindowSeconds(double seconds) {
    if (seconds <= 0.0 || qFuzzyCompare(seconds, windowSeconds_)) {
        return;
    }
    windowSeconds_ = seconds;
    reconfigure();
    emit viewChanged();
}

void EegClient::setHighPassHz(double hz) {
    if (hz < 0.0 || qFuzzyCompare(hz + 1.0, highPassHz_ + 1.0)) {
        return;
    }
    highPassHz_ = hz;
    store_.setFilters(highPassHz_, notchHz_);
    emit viewChanged();
}

void EegClient::setNotchHz(double hz) {
    if (hz < 0.0 || qFuzzyCompare(hz + 1.0, notchHz_ + 1.0)) {
        return;
    }
    notchHz_ = hz;
    store_.setFilters(highPassHz_, notchHz_);
    emit viewChanged();
}

void EegClient::setFrozen(bool frozen) {
    if (frozen == frozen_) {
        return;
    }
    frozen_ = frozen;
    emit viewChanged();
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

    haveSequence_ = false;
    meter_.reset();
    setStatus(QStringLiteral("acquiring"));
    emit connectionChanged();
    emit statsChanged();
}

void EegClient::dropConnection(const QString& reason) {
    if (notifier_ != nullptr) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    channel_.reset();

    // No frames, no contact status and no rate: unknown, not "off" and not
    // "zero". The last trace stays on screen under a status that says why it
    // stopped moving.
    contact_ = med::LeadOff{};
    meter_.reset();
    setStatus(reason);
    emit connectionChanged();
    emit contactChanged();
    emit statsChanged();
}

void EegClient::readPending() {
    bool appended = false;

    for (int taken = 0; taken < kFramesPerWakeup && channel_; ++taken) {
        unsigned char buffer[65536];
        med::Result<std::size_t> received =
            channel_->receive(buffer, sizeof(buffer), std::chrono::milliseconds(0));
        if (!received) {
            if (received.status() != med::Status::Timeout) {
                dropConnection(QStringLiteral("acquisition service went away, reconnecting"));
            }
            break;
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
            continue;
        }
        const med::SampleFrame& frame = decoded.value();

        if (channelCount_ != frame.channelCount || !qFuzzyCompare(sampleRateHz_ + 1.0, rateHz + 1.0)) {
            channelCount_ = frame.channelCount;
            sampleRateHz_ = rateHz;
            meter_.reset();
            reconfigure();
            emit layoutChanged();
        }

        // Counted whether or not the display is frozen: freezing the picture
        // must not make the stream look healthier or sicker than it is.
        meter_.add(clock_.nsecsElapsed(), frame.samplesPerChannel);
        if (haveSequence_ && frame.sequence > lastSequence_ + 1) {
            framesMissed_ += frame.sequence - lastSequence_ - 1;
        }
        // A sequence that goes backwards is a service that restarted, and a
        // restart is not a loss on this link.
        lastSequence_ = frame.sequence;
        haveSequence_ = true;
        ++framesReceived_;

        if (frame.leadOff != contact_) {
            contact_ = frame.leadOff;
            emit contactChanged();
        }

        if (!frozen_ && static_cast<std::size_t>(frame.samplesPerChannel) * frame.channelCount <=
                            frame.samples.size()) {
            store_.append(frame.samples.data(), frame.channelCount,
                          static_cast<int>(frame.samplesPerChannel));
            appended = true;
        }
    }

    if (appended) {
        emit samplesAppended();
    }
}

void EegClient::refreshStatistics() {
    meter_.tick(clock_.nsecsElapsed());

    QList<qreal> rms;
    QList<qreal> level;
    QList<bool> flat;
    rms.reserve(channelCount_);
    level.reserve(channelCount_);
    flat.reserve(channelCount_);
    const int lastSecond = static_cast<int>(std::lround(sampleRateHz_));
    for (int channel = 0; channel < channelCount_; ++channel) {
        rms.append(store_.rms(channel, lastSecond));
        const hmi::TraceStore::RawLevel raw = store_.rawLevel(channel);
        level.append(raw.mean);
        // Only while a stream is attached: with none, nothing arrives, and
        // "did not vary" would be said of an input nobody is reading.
        flat.append(connected() && raw.measured && raw.span == 0.0);
    }
    rms_ = rms;
    level_ = level;
    flat_ = flat;

    emit statsChanged();
}
