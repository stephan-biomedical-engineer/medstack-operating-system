// SPDX-License-Identifier: MIT
//
// MedPlatform EEG HMI.
//
// A Qt Quick client of the acquisition service. It never touches the
// front-end, the record volume or the update system: it renders the stream it
// is given. That separation is what allows the HMI to be a non-safety-relevant
// software item under IEC 62304 while acquisition is not.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QUrl>

#include "EegClient.h"
#include "ReviewRequest.h"
#include "SessionControl.h"
#include "WaveformItem.h"

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("MedPlatform EEG"));
    QGuiApplication::setOrganizationName(QStringLiteral("MedPlatform"));

    const QString socketPath =
        qEnvironmentVariable("MED_EEG_SOCKET", QStringLiteral("/run/medplatform/eeg.sock"));

    EegClient client(socketPath);

    // The one type QML instantiates. The client stays a context property: there
    // is exactly one stream, and the screen does not get to create another.
    qmlRegisterType<WaveformItem>("MedPlatform.Eeg", 1, 0, "WaveformItem");

    ReviewRequest review(qEnvironmentVariable("MED_REVIEW_REQUEST"));
    // Asking is not acquiring: this object can request a session and be
    // refused one, and that is all it can do to the service.
    SessionControl sessions(qEnvironmentVariable(
        "MED_EEG_CONTROL_SOCKET", QStringLiteral("/run/medplatform/eeg-control.sock")));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("eeg"), &client);
    engine.rootContext()->setContextProperty(QStringLiteral("review"), &review);
    engine.rootContext()->setContextProperty(QStringLiteral("sessions"), &sessions);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return QGuiApplication::exec();
}
