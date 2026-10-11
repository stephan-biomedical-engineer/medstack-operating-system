// SPDX-License-Identifier: MIT

#include "WaveformItem.h"

#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "EegClient.h"
#include "TraceStore.h"

namespace {

using Vertex = QSGGeometry::Point2D;

QSGGeometryNode* makeNode(QSGGeometry::DrawingMode mode) {
    auto* node = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(mode);
    geometry->setLineWidth(1.0F);
    node->setGeometry(geometry);
    node->setMaterial(new QSGFlatColorMaterial);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

void fill(QSGGeometryNode* node, const std::vector<Vertex>& vertices, const QColor& color) {
    QSGGeometry* geometry = node->geometry();
    geometry->allocate(static_cast<int>(vertices.size()));
    if (!vertices.empty()) {
        std::memcpy(geometry->vertexData(), vertices.data(), vertices.size() * sizeof(Vertex));
    }
    node->markDirty(QSGNode::DirtyGeometry);

    auto* material = static_cast<QSGFlatColorMaterial*>(node->material());
    if (material->color() != color) {
        material->setColor(color);
        node->markDirty(QSGNode::DirtyMaterial);
    }
}

Vertex at(double x, double y) {
    Vertex v;
    v.set(static_cast<float>(x), static_cast<float>(y));
    return v;
}

}  // namespace

WaveformItem::WaveformItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

QObject* WaveformItem::client() const {
    return client_;
}

void WaveformItem::setClient(QObject* client) {
    auto* typed = qobject_cast<EegClient*>(client);
    if (typed == client_) {
        return;
    }
    if (client_ != nullptr) {
        disconnect(client_, nullptr, this, nullptr);
    }
    client_ = typed;
    if (client_ != nullptr) {
        // update() only schedules: however many frames arrive between two
        // display refreshes, the scene graph asks for the vertices once.
        connect(client_, &EegClient::samplesAppended, this, &QQuickItem::update);
        connect(client_, &EegClient::layoutChanged, this, &QQuickItem::update);
        connect(client_, &QObject::destroyed, this, [this] {
            client_ = nullptr;
            update();
        });
    }
    emit clientChanged();
    update();
}

void WaveformItem::setScales(const QList<qreal>& scales) {
    if (scales == scales_) {
        return;
    }
    scales_ = scales;
    emit appearanceChanged();
    update();
}

void WaveformItem::setColors(const QStringList& colors) {
    if (colors == colors_) {
        return;
    }
    colors_ = colors;
    emit appearanceChanged();
    update();
}

void WaveformItem::setGridColor(const QColor& color) {
    if (color == gridColor_) {
        return;
    }
    gridColor_ = color;
    emit appearanceChanged();
    update();
}

void WaveformItem::setCursorColor(const QColor& color) {
    if (color == cursorColor_) {
        return;
    }
    cursorColor_ = color;
    emit appearanceChanged();
    update();
}

void WaveformItem::setLineWidth(qreal width) {
    if (qFuzzyCompare(width, lineWidth_) || width <= 0.0) {
        return;
    }
    lineWidth_ = width;
    emit appearanceChanged();
    update();
}

QColor WaveformItem::channelColor(int channel) const {
    if (colors_.isEmpty()) {
        return QColor(QStringLiteral("#4ec9b0"));
    }
    return QColor(colors_.at(channel % colors_.size()));
}

double WaveformItem::channelScale(int channel) const {
    const double scale = channel < scales_.size() ? scales_.at(channel) : 100.0;
    return scale > 0.0 ? scale : 100.0;
}

QSGNode* WaveformItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    const hmi::TraceStore* store = client_ != nullptr ? &client_->store() : nullptr;
    const int channels = store != nullptr ? store->channels() : 0;
    const double w = width();
    const double h = height();

    QSGNode* root = old != nullptr ? old : new QSGNode;

    // Children: the grid, one strip per channel, the sweep cursor.
    if (root->childCount() != channels + 2) {
        while (QSGNode* child = root->firstChild()) {
            root->removeChildNode(child);
            delete child;
        }
        root->appendChildNode(makeNode(QSGGeometry::DrawLines));
        for (int channel = 0; channel < channels; ++channel) {
            root->appendChildNode(makeNode(QSGGeometry::DrawTriangleStrip));
        }
        root->appendChildNode(makeNode(QSGGeometry::DrawTriangleStrip));
    }

    auto* gridNode = static_cast<QSGGeometryNode*>(root->childAtIndex(0));
    auto* cursorNode = static_cast<QSGGeometryNode*>(root->childAtIndex(channels + 1));

    std::vector<Vertex> vertices;
    if (channels <= 0 || w < 2.0 || h < 2.0) {
        fill(gridNode, vertices, gridColor_);
        fill(cursorNode, vertices, cursorColor_);
        return root;
    }

    const double lane = h / channels;

    // --- grid: a baseline per lane, a separator between lanes, a tick per
    // second (per five beyond half a minute, where one a second is a fence).
    for (int channel = 0; channel < channels; ++channel) {
        const double baseline = std::floor(lane * (channel + 0.5)) + 0.5;
        vertices.push_back(at(0.0, baseline));
        vertices.push_back(at(w, baseline));
        if (channel > 0) {
            const double edge = std::floor(lane * channel) + 0.5;
            vertices.push_back(at(0.0, edge));
            vertices.push_back(at(w, edge));
        }
    }
    const double seconds = store->windowSeconds();
    if (seconds > 0.0) {
        const double step = seconds > 30.0 ? 5.0 : 1.0;
        for (double t = step; t < seconds - 1e-9; t += step) {
            const double x = std::floor(t / seconds * w) + 0.5;
            vertices.push_back(at(x, 0.0));
            vertices.push_back(at(x, h));
        }
    }
    fill(gridNode, vertices, gridColor_);

    // --- traces -------------------------------------------------------------
    const int capacity = store->capacity();
    const int columns = std::max(1, std::min(capacity, static_cast<int>(std::ceil(w))));
    const double half = lineWidth_ / 2.0;
    std::vector<hmi::Column> reduced;

    for (int channel = 0; channel < channels; ++channel) {
        auto* node = static_cast<QSGGeometryNode*>(root->childAtIndex(channel + 1));
        vertices.clear();
        store->envelope(channel, columns, reduced);

        const double baseline = lane * (channel + 0.5);
        const double top = lane * channel + 1.0;
        const double bottom = lane * (channel + 1) - 1.0;
        const double perMicrovolt = lane * 0.45 / channelScale(channel);

        // One pair of vertices per column, from the column's maximum to its
        // minimum, widened by half the line width: a strip through them is the
        // trace where samples are sparse and its envelope where they are
        // dense. A run of empty columns ends the strip, and two repeated
        // vertices carry the pen across without drawing.
        bool drawing = false;
        for (int column = 0; column < static_cast<int>(reduced.size()); ++column) {
            const hmi::Column& c = reduced[static_cast<std::size_t>(column)];
            if (c.empty()) {
                drawing = false;
                continue;
            }
            const double x = (column + 0.5) / columns * w;
            // A value beyond the lane stops at its edge. It must not wander
            // into the neighbour's lane and be read as the neighbour's signal.
            const double high = std::clamp(baseline - static_cast<double>(c.hi) * perMicrovolt - half, top, bottom);
            const double low = std::clamp(baseline - static_cast<double>(c.lo) * perMicrovolt + half, top, bottom);
            if (!drawing && !vertices.empty()) {
                vertices.push_back(vertices.back());
                vertices.push_back(at(x, high));
            }
            vertices.push_back(at(x, high));
            vertices.push_back(at(x, low));
            drawing = true;
        }
        fill(node, vertices, channelColor(channel));
    }

    // --- the sweep cursor -----------------------------------------------------
    vertices.clear();
    if (store->total() > 0 && capacity > 0) {
        const double x = static_cast<double>(store->head()) / capacity * w;
        vertices.push_back(at(x, 0.0));
        vertices.push_back(at(x, h));
        vertices.push_back(at(x + 2.0, 0.0));
        vertices.push_back(at(x + 2.0, h));
    }
    fill(cursorNode, vertices, cursorColor_);

    return root;
}
