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
#include <QUrl>

#include "EegClient.h"

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("MedPlatform EEG"));
    QGuiApplication::setOrganizationName(QStringLiteral("MedPlatform"));

    const QString socketPath =
        qEnvironmentVariable("MED_EEG_SOCKET", QStringLiteral("/run/medplatform/eeg.sock"));

    EegClient client(socketPath);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("eeg"), &client);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return QGuiApplication::exec();
}
