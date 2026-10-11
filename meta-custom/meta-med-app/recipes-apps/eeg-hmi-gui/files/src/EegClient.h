// SPDX-License-Identifier: MIT
//
// QML-facing model of the live EEG stream.
//
// Note what this class does *not* contain: no socket code, no framing, no
// endianness handling of its own. It consumes MedicalIPC and the AMP frame
// definition from MedFramework, which is why the HMI is indifferent to whether
// the samples originated in a co-processor, a kernel driver or the simulator.
//
// It asks for nothing. Every property a control on the screen can write is a
// property of the DISPLAY - how long a window, which display filter, frozen or
// not. Nothing here reaches the acquisition service, which is why the screen
// can be wrong, slow or closed without the record noticing.

#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>

#include "TraceStore.h"

class QSocketNotifier;

class EegClient : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

    Q_PROPERTY(int channelCount READ channelCount NOTIFY layoutChanged)
    /// What the stream's header declares.
    Q_PROPERTY(double sampleRateHz READ sampleRateHz NOTIFY layoutChanged)
    /// What actually arrived, counted against this process's monotonic clock.
    /// Meaningless until rateMeasured.
    Q_PROPERTY(double deliveredRateHz READ deliveredRateHz NOTIFY statsChanged)
    Q_PROPERTY(bool rateMeasured READ rateMeasured NOTIFY statsChanged)

    Q_PROPERTY(qulonglong framesReceived READ framesReceived NOTIFY statsChanged)
    Q_PROPERTY(qulonglong framesRejected READ framesRejected NOTIFY statsChanged)
    /// Frames the sequence numbers say were published and never reached this
    /// viewer. A viewer may lose frames; the record does not lose them with it.
    Q_PROPERTY(qulonglong framesMissed READ framesMissed NOTIFY statsChanged)

    /// Electrode contact as bit masks, bit n for channel n. An electrode that
    /// is not monitored is neither attached nor detached: it is unknown, and
    /// the screen must be able to say so.
    Q_PROPERTY(int monitoredPositive READ monitoredPositive NOTIFY contactChanged)
    Q_PROPERTY(int monitoredNegative READ monitoredNegative NOTIFY contactChanged)
    Q_PROPERTY(int detachedPositive READ detachedPositive NOTIFY contactChanged)
    Q_PROPERTY(int detachedNegative READ detachedNegative NOTIFY contactChanged)
    Q_PROPERTY(bool electrodeOff READ electrodeOff NOTIFY contactChanged)

    /// Per channel, microvolts rms about the mean over the last second of what
    /// is DRAWN - display filters included.
    Q_PROPERTY(QList<qreal> rms READ rms NOTIFY statsChanged)
    /// Per channel, the mean level in microvolts of what ARRIVED over the last
    /// whole second, before any display filter.
    Q_PROPERTY(QList<qreal> level READ level NOTIFY statsChanged)
    /// Per channel, true when the input did not vary at all over that second.
    /// Not "quiet": a converter's own noise moves a real input. An input that
    /// does not move is at a rail, shorted to nothing, or not being converted.
    Q_PROPERTY(QList<bool> flat READ flat NOTIFY statsChanged)

    // --- the display, and only the display --------------------------------
    Q_PROPERTY(double windowSeconds READ windowSeconds WRITE setWindowSeconds NOTIFY viewChanged)
    Q_PROPERTY(double highPassHz READ highPassHz WRITE setHighPassHz NOTIFY viewChanged)
    Q_PROPERTY(double notchHz READ notchHz WRITE setNotchHz NOTIFY viewChanged)
    Q_PROPERTY(bool frozen READ frozen WRITE setFrozen NOTIFY viewChanged)

public:
    explicit EegClient(QString socketPath, QObject* parent = nullptr);
    ~EegClient() override;

    bool connected() const { return static_cast<bool>(channel_); }
    QString status() const { return status_; }
    int channelCount() const { return channelCount_; }
    double sampleRateHz() const { return sampleRateHz_; }
    double deliveredRateHz() const { return meter_.rateHz(); }
    bool rateMeasured() const { return meter_.measured(); }
    qulonglong framesReceived() const { return framesReceived_; }
    qulonglong framesRejected() const { return framesRejected_; }
    qulonglong framesMissed() const { return framesMissed_; }
    int monitoredPositive() const { return contact_.monitoredPositive; }
    int monitoredNegative() const { return contact_.monitoredNegative; }
    int detachedPositive() const { return contact_.detachedPositive(); }
    int detachedNegative() const { return contact_.detachedNegative(); }
    bool electrodeOff() const { return detachedPositive() != 0 || detachedNegative() != 0; }
    QList<qreal> rms() const { return rms_; }
    QList<qreal> level() const { return level_; }
    QList<bool> flat() const { return flat_; }

    double windowSeconds() const { return windowSeconds_; }
    void setWindowSeconds(double seconds);
    double highPassHz() const { return highPassHz_; }
    void setHighPassHz(double hz);
    double notchHz() const { return notchHz_; }
    void setNotchHz(double hz);
    bool frozen() const { return frozen_; }
    void setFrozen(bool frozen);

    /// Largest excursion from its mean in the window held, in microvolts.
    Q_INVOKABLE double peakUv(int channel) const { return store_.peak(channel); }

    /// What the waveform item draws from. Read on the render thread only while
    /// the GUI thread is held for it, which is the one moment the scene graph
    /// guarantees nothing here is being written.
    const hmi::TraceStore& store() const { return store_; }

signals:
    void connectionChanged();
    void statusChanged();
    void layoutChanged();
    void statsChanged();
    void contactChanged();
    void viewChanged();
    /// The store changed. Deliberately bound to nothing in QML: at the fastest
    /// rate this fires a thousand times a second, and the item it drives
    /// repaints once per display frame however often it is told.
    void samplesAppended();

private slots:
    void attemptConnection();
    void readPending();
    void refreshStatistics();

private:
    void setStatus(const QString& status);
    void dropConnection(const QString& reason);
    void reconfigure();

    QString socketPath_;
    std::unique_ptr<med::MedicalIpcChannel> channel_;
    QSocketNotifier* notifier_ = nullptr;
    QTimer reconnectTimer_;
    QTimer statisticsTimer_;
    QElapsedTimer clock_;

    QString status_;
    int channelCount_ = 0;
    double sampleRateHz_ = 0.0;
    qulonglong framesReceived_ = 0;
    qulonglong framesRejected_ = 0;
    qulonglong framesMissed_ = 0;
    quint64 lastSequence_ = 0;
    bool haveSequence_ = false;
    med::LeadOff contact_;
    QList<qreal> rms_;
    QList<qreal> level_;
    QList<bool> flat_;

    double windowSeconds_ = 10.0;
    double highPassHz_ = 0.5;
    double notchHz_ = 0.0;
    bool frozen_ = false;

    hmi::TraceStore store_;
    hmi::RateMeter meter_{2.0};
};
