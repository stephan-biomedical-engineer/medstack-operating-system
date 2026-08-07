// SPDX-License-Identifier: MIT
//
// QML-facing model of the live EEG stream.
//
// Note what this class does *not* contain: no socket code, no framing, no
// endianness handling of its own. It consumes MedicalIPC and the AMP frame
// definition from MedFramework, which is why the HMI is indifferent to whether
// the samples originated in a Cortex-M4 or in the simulator.

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include <memory>

#include <medplatform/MedicalIPC.h>

class QSocketNotifier;

class EegClient : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY frameReceived)
    Q_PROPERTY(double sampleRateHz READ sampleRateHz NOTIFY frameReceived)
    Q_PROPERTY(qulonglong framesReceived READ framesReceived NOTIFY frameReceived)
    Q_PROPERTY(qulonglong framesRejected READ framesRejected NOTIFY frameReceived)

public:
    explicit EegClient(QString socketPath, QObject* parent = nullptr);
    ~EegClient() override;

    bool connected() const { return static_cast<bool>(channel_); }
    QString status() const { return status_; }
    int channelCount() const { return channelCount_; }
    double sampleRateHz() const { return sampleRateHz_; }
    qulonglong framesReceived() const { return framesReceived_; }
    qulonglong framesRejected() const { return framesRejected_; }

    /// The visible window of one channel, oldest sample first, in microvolts.
    Q_INVOKABLE QVariantList waveform(int channel) const;

signals:
    void connectionChanged();
    void statusChanged();
    void frameReceived();

private slots:
    void attemptConnection();
    void readPending();

private:
    void setStatus(const QString& status);
    void dropConnection(const QString& reason);

    QString socketPath_;
    std::unique_ptr<med::MedicalIpcChannel> channel_;
    QSocketNotifier* notifier_ = nullptr;
    QTimer reconnectTimer_;

    QString status_;
    int channelCount_ = 0;
    double sampleRateHz_ = 0.0;
    qulonglong framesReceived_ = 0;
    qulonglong framesRejected_ = 0;
    quint64 lastSequence_ = 0;

    /// Per channel ring of the most recent samples. Bounded: an HMI that grows
    /// its buffer for the length of a session eventually takes the device down.
    QVector<QVector<double>> history_;
    int windowSamples_ = 750;
};
