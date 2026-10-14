// SPDX-License-Identifier: MIT
//
// What this device's front-end can be set to, as the build sealed it.
//
// The display does not know what a front-end has. It reads the same sealed
// table the acquisition service chooses from, through the framework, and
// shows what is in it: the settings that exist, the values each can take, and
// which combinations are rows of the table. A setting a link cannot honour is
// not a row, so it is not on the screen - nothing here decides that.
//
// It applies nothing. Choosing is a request to the service, made by
// SessionControl; this class only says what there is to ask for.

#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class PrescriptionTable : public QObject {
    Q_OBJECT

    /// False when there is no table, or it failed verification: then nothing
    /// can be chosen, and the screen offers nothing.
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QStringList keys READ keys CONSTANT)
    Q_PROPERTY(int factory READ factory CONSTANT)

public:
    explicit PrescriptionTable(const QString& path, QObject* parent = nullptr);

    bool available() const { return !rows_.isEmpty(); }
    QStringList keys() const { return keys_; }
    int factory() const { return factory_; }

    /// The values a setting takes anywhere in the table, in the table's order.
    Q_INVOKABLE QStringList values(const QString& key) const;
    /// One row, as setting -> value. Empty for an index outside the table.
    Q_INVOKABLE QVariantMap choice(int index) const;
    /// The row that is exactly these settings, or -1: the combination is not
    /// offered on this link.
    Q_INVOKABLE int indexOf(const QVariantMap& settings) const;
    /// The word that has to be stated to change this setting, or empty.
    Q_INVOKABLE QString affirmation(const QString& key) const;
    /// What one row measures, as that row's record declares it: "input" (what
    /// every channel's inputs are switched to), and the one channel that is
    /// not a voltage of interest - "channel" (from 1; 0 for none), its
    /// "label", its "unit", and the three numbers of
    /// value = (uV - uvAtReference) / uvPerUnit + reference. The display
    /// converts by this and knows no sensor. "No such channel" for a row that
    /// declares nothing, or an index outside the table.
    Q_INVOKABLE QVariantMap physical(int index) const;

private:
    QStringList keys_;
    QList<QMap<QString, QString>> rows_;
    QMap<QString, QString> affirmations_;
    QList<QVariantMap> physical_;
    int factory_ = -1;
};
