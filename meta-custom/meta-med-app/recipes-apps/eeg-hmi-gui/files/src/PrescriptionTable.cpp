// SPDX-License-Identifier: MIT

#include "PrescriptionTable.h"

#include <memory>
#include <string>

#include <medplatform/MedicalConfiguration.h>

PrescriptionTable::PrescriptionTable(const QString& path, QObject* parent) : QObject(parent) {
    // Verified, like every store this platform reads: a table with no seal,
    // or one that does not match it, is no table.
    med::Result<std::unique_ptr<med::MedicalConfiguration>> loaded =
        med::MedicalConfiguration::load(path.toStdString(), true);
    if (!loaded) {
        return;
    }
    const med::MedicalConfiguration& table = *loaded.value();

    keys_ = QString::fromStdString(table.getString("choices.keys", ""))
                .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const long long count = table.getInt("choices.count", 0);
    for (long long index = 0; index < count && index < 4096; ++index) {
        QMap<QString, QString> row;
        for (const QString& key : keys_) {
            row.insert(key, QString::fromStdString(table.getString(
                                "choice." + std::to_string(index) + "." + key.toStdString(), "")));
        }
        rows_.append(row);
    }
    for (const QString& key : keys_) {
        const QString name =
            QString::fromStdString(table.getString("choices.affirm." + key.toStdString(), ""));
        if (!name.isEmpty()) {
            affirmations_.insert(key, name);
        }
    }
    factory_ = static_cast<int>(table.getInt("choices.factory", -1));
}

QStringList PrescriptionTable::values(const QString& key) const {
    QStringList seen;
    for (const QMap<QString, QString>& row : rows_) {
        const QString value = row.value(key);
        if (!seen.contains(value)) {
            seen.append(value);
        }
    }
    return seen;
}

QVariantMap PrescriptionTable::choice(int index) const {
    QVariantMap out;
    if (index < 0 || index >= rows_.size()) {
        return out;
    }
    const QMap<QString, QString>& row = rows_.at(index);
    for (QMap<QString, QString>::const_iterator it = row.constBegin(); it != row.constEnd(); ++it) {
        out.insert(it.key(), it.value());
    }
    return out;
}

int PrescriptionTable::indexOf(const QVariantMap& settings) const {
    for (int index = 0; index < rows_.size(); ++index) {
        bool same = true;
        for (const QString& key : keys_) {
            if (settings.value(key).toString() != rows_.at(index).value(key)) {
                same = false;
                break;
            }
        }
        if (same) {
            return index;
        }
    }
    return -1;
}

QString PrescriptionTable::affirmation(const QString& key) const {
    return affirmations_.value(key);
}
