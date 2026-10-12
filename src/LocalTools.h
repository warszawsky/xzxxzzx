#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>

class HomeAssistant;

// Локальные функции питомца. Используются двумя путями:
//  1) как function calling для Gemini (declarations() + invoke());
//  2) как мгновенные команды «/timer 5m» без обращения к модели (handleCommand()).
//
// Намеренно НЕТ функций вроде «выполни команду в shell» или «открой URL»: модель читает
// скриншоты, а на экране может быть чужой текст с инструкциями (prompt injection).
// Всё, что может сделать модель, должно быть безопасно при любом её ответе.
class LocalTools : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(const QJsonObject&)>;

    explicit LocalTools(HomeAssistant* ha, QObject* parent = nullptr);

    QJsonArray declarations() const;
    void invoke(const QString& name, const QJsonObject& args, const Done& done);

    // true, если это была локальная команда (тогда в reply ответ для пользователя).
    bool handleCommand(const QString& input, QString* reply);

    bool hasSmartHome() const;

signals:
    void alarm(const QString& title, const QString& text);
    // Действие с физическим устройством из «опасного» домена: нужен ответ человека.
    void confirmationNeeded(const QString& question, const std::function<void(bool)>& answer);

private:
    struct Entry
    {
        int id = 0;
        QString kind;   // timer | reminder
        QString text;
        QDateTime due;
    };

    QJsonObject addEntry(const QString& kind, const QDateTime& due, const QString& text);
    QJsonObject listEntries() const;
    QJsonObject cancelEntry(int id);
    QJsonObject addNote(const QString& text);
    QJsonObject listNotes(int limit) const;
    void smartHomeCall(const QJsonObject& args, const Done& done);

    void tick();
    void load();
    void saveSchedule() const;
    void saveNotes() const;
    QString dataFile(const QString& name) const;

    static qint64 parseDurationSecs(const QString& s);
    static QDateTime parseWhen(const QString& s);
    static QString formatLeft(qint64 secs);

    HomeAssistant* m_ha;
    QList<Entry> m_entries;
    QJsonArray m_notes;
    int m_nextId = 1;
    QTimer m_ticker;
};
