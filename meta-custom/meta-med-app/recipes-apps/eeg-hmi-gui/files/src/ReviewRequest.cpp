// SPDX-License-Identifier: MIT

#include "ReviewRequest.h"

#include <QByteArray>
#include <QFile>

ReviewRequest::ReviewRequest(QString requestPath, QObject* parent)
    : QObject(parent), requestPath_(std::move(requestPath)) {}

bool ReviewRequest::open() {
    if (requestPath_.isEmpty()) {
        return false;
    }
    QFile file(requestPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    // The content is a counter and nothing reads it: what the watcher acts on
    // is the file being written and closed. It is here so that two requests
    // are two different files to anyone looking at the directory afterwards.
    const QByteArray line = QByteArray::number(++requests_) + '\n';
    const bool written = file.write(line) == line.size();
    file.close();
    return written;
}
