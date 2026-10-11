// SPDX-License-Identifier: MIT
//
// The one thing on the operator display that reaches outside it, and it is a
// request, not an action.
//
// The reviewing program is a different software item running as a different
// user, and this process may not start it: it has no privilege to become
// anyone, and must not gain one. What it can do is say "the operator asked" by
// leaving a file in a spool directory. The init system watches the spool,
// empties it unread, and starts the reviewer under the identity and the
// sandbox its own unit declares. The request carries no parameter - not a file name, not a user -
// so there is nothing in it for the other side to trust.
//
// Where the path is comes from the environment, set by whoever ships the
// reviewer. An image without one sets nothing, and the screen then has no
// such button: the display does not offer what the device cannot do.

#pragma once

#include <QObject>
#include <QString>

class ReviewRequest : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit ReviewRequest(QString requestPath, QObject* parent = nullptr);

    bool available() const { return !requestPath_.isEmpty(); }

    /// Asks. True when the request was written; whether a reviewer appears is
    /// the init system's business and is not known here.
    Q_INVOKABLE bool open();

private:
    QString requestPath_;
    quint64 requests_ = 0;
};
