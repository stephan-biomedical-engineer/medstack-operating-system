// SPDX-License-Identifier: MIT
//
// The traces, drawn by the scene graph.
//
// The display this replaces repainted a Canvas from JavaScript on every frame:
// every sample boxed into a QVariant, crossed into the QML engine and stroked
// by a software rasteriser on the GUI thread. Measured on the board
// 2026-10-11, that cost one whole core of two at 250 samples a second - 8.6 s
// of GUI thread in every 10 s.
//
// Here the samples never leave C++. Each channel is one triangle strip whose
// vertices are computed from the store when the scene graph asks, and the GPU
// does the rest. The work per repaint follows the number of screen columns, not
// the sample rate.

#pragma once

#include <QColor>
#include <QList>
#include <QQuickItem>
#include <QStringList>

class EegClient;
class QSGGeometryNode;

class WaveformItem : public QQuickItem {
    Q_OBJECT

    /// The EegClient to draw. QObject* so that QML needs no registered type
    /// for a class it only ever passes through.
    Q_PROPERTY(QObject* client READ client WRITE setClient NOTIFY clientChanged)
    /// Per channel, the amplitude in microvolts that reaches the edge of its
    /// lane. Fixed numbers the operator chooses: a trace that rescaled itself
    /// would make two screens incomparable.
    Q_PROPERTY(QList<qreal> scales READ scales WRITE setScales NOTIFY appearanceChanged)
    Q_PROPERTY(QStringList colors READ colors WRITE setColors NOTIFY appearanceChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY appearanceChanged)
    Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor NOTIFY appearanceChanged)
    Q_PROPERTY(qreal lineWidth READ lineWidth WRITE setLineWidth NOTIFY appearanceChanged)

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    QObject* client() const;
    void setClient(QObject* client);
    QList<qreal> scales() const { return scales_; }
    void setScales(const QList<qreal>& scales);
    QStringList colors() const { return colors_; }
    void setColors(const QStringList& colors);
    QColor gridColor() const { return gridColor_; }
    void setGridColor(const QColor& color);
    QColor cursorColor() const { return cursorColor_; }
    void setCursorColor(const QColor& color);
    qreal lineWidth() const { return lineWidth_; }
    void setLineWidth(qreal width);

signals:
    void clientChanged();
    void appearanceChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;

private:
    QColor channelColor(int channel) const;
    double channelScale(int channel) const;

    EegClient* client_ = nullptr;
    QList<qreal> scales_;
    QStringList colors_;
    QColor gridColor_{QStringLiteral("#1e2733")};
    QColor cursorColor_{QStringLiteral("#3fb6ff")};
    qreal lineWidth_ = 1.6;
};
