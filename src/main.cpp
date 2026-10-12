#include "App.h"

#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkProxyFactory>

// Запуск:
//   geminik               питомец (если уже запущен, откроет/скроет чат)
//   geminik --screenshot  попросить запущенного питомца разобрать экран
//                         (удобно повесить на системный хоткей там, где глобальные
//                         хоткеи приложениям недоступны)
//   geminik --quit        закрыть запущенного питомца
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Geminik"));
    QApplication::setApplicationName(QStringLiteral("geminik"));
    QApplication::setApplicationDisplayName(QStringLiteral("Джеминик"));
    QApplication::setQuitOnLastWindowClosed(false);
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    QString command = QStringLiteral("chat");
    if (app.arguments().contains(QStringLiteral("--screenshot")))
        command = QStringLiteral("screenshot");
    else if (app.arguments().contains(QStringLiteral("--quit")))
        command = QStringLiteral("quit");

    // Один питомец на пользователя: второй запуск передаёт команду первому и выходит.
    const QString serverName = QStringLiteral("geminik-")
        + qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", QStringLiteral("user")));
    {
        QLocalSocket probe;
        probe.connectToServer(serverName);
        if (probe.waitForConnected(300)) {
            probe.write(command.toUtf8() + '\n');
            probe.waitForBytesWritten(500);
            return 0;
        }
    }
    if (command == QLatin1String("quit"))
        return 0;

    App pet;
    QLocalServer::removeServer(serverName);   // хвост от упавшего процесса
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(serverName);
    QObject::connect(&server, &QLocalServer::newConnection, &pet, [&server, &pet] {
        while (QLocalSocket* s = server.nextPendingConnection()) {
            QObject::connect(s, &QLocalSocket::readyRead, &pet, [s, &pet] {
                if (!s->canReadLine())
                    return;
                pet.handleCommand(QString::fromUtf8(s->readLine()).trimmed());
                s->disconnectFromServer();
            });
            QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        }
    });

    pet.start();
    return app.exec();
}
