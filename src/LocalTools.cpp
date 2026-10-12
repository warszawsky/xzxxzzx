#include "LocalTools.h"

#include "HomeAssistant.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QJsonObject prop(const QString& type, const QString& description)
{
    return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}

QJsonObject function(const QString& name, const QString& description,
                     const QJsonObject& properties = {}, const QStringList& required = {})
{
    QJsonObject fn{{QStringLiteral("name"), name}, {QStringLiteral("description"), description}};
    if (!properties.isEmpty()) {
        QJsonObject params{{QStringLiteral("type"), QStringLiteral("object")},
                           {QStringLiteral("properties"), properties}};
        if (!required.isEmpty())
            params[QStringLiteral("required")] = QJsonArray::fromStringList(required);
        fn[QStringLiteral("parameters")] = params;
    }
    return fn;
}

QJsonObject error(const QString& message)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

// Секунды до момента с округлением вверх: таймер на 3 с не должен показывать «через 2 с».
qint64 secsUntil(const QDateTime& due)
{
    return (QDateTime::currentDateTime().msecsTo(due) + 999) / 1000;
}

} // namespace

LocalTools::LocalTools(HomeAssistant* ha, QObject* parent) : QObject(parent), m_ha(ha)
{
    load();
    // Один тикер на всё и сравнение с настенными часами: переживает сон ПК и не упирается
    // в предел QTimer (~24 дня) для далёких напоминаний.
    m_ticker.setInterval(1000);
    connect(&m_ticker, &QTimer::timeout, this, &LocalTools::tick);
    m_ticker.start();
}

bool LocalTools::hasSmartHome() const
{
    return m_ha && m_ha->isConfigured();
}

// ---------------------------------------------------------------------------------------------
// Function calling

QJsonArray LocalTools::declarations() const
{
    QJsonArray d;
    d << function(QStringLiteral("set_timer"), QStringLiteral("Запустить таймер обратного отсчёта."),
                  {{QStringLiteral("seconds"), prop(QStringLiteral("integer"), QStringLiteral("Длительность в секундах"))},
                   {QStringLiteral("label"), prop(QStringLiteral("string"), QStringLiteral("Короткая подпись, например «чай»"))}},
                  {QStringLiteral("seconds")});
    d << function(QStringLiteral("set_reminder"),
                  QStringLiteral("Напомнить пользователю о чём-то в заданное время. Передай либо at, либо in_minutes."),
                  {{QStringLiteral("text"), prop(QStringLiteral("string"), QStringLiteral("Текст напоминания"))},
                   {QStringLiteral("at"), prop(QStringLiteral("string"), QStringLiteral("Локальное время YYYY-MM-DDTHH:MM"))},
                   {QStringLiteral("in_minutes"), prop(QStringLiteral("integer"), QStringLiteral("Через сколько минут"))}},
                  {QStringLiteral("text")});
    d << function(QStringLiteral("list_timers"), QStringLiteral("Список активных таймеров и напоминаний."));
    d << function(QStringLiteral("cancel_timer"), QStringLiteral("Отменить таймер или напоминание по id."),
                  {{QStringLiteral("id"), prop(QStringLiteral("integer"), QStringLiteral("id из list_timers"))}},
                  {QStringLiteral("id")});
    d << function(QStringLiteral("add_note"), QStringLiteral("Сохранить заметку."),
                  {{QStringLiteral("text"), prop(QStringLiteral("string"), QStringLiteral("Текст заметки"))}},
                  {QStringLiteral("text")});
    d << function(QStringLiteral("list_notes"), QStringLiteral("Последние заметки пользователя."),
                  {{QStringLiteral("limit"), prop(QStringLiteral("integer"), QStringLiteral("Сколько вернуть, по умолчанию 10"))}});

    if (hasSmartHome()) {
        d << function(QStringLiteral("smart_home_list"),
                      QStringLiteral("Устройства умного дома, доступные питомцу: entity_id, имя, состояние."));
        d << function(QStringLiteral("smart_home_state"), QStringLiteral("Состояние одного устройства."),
                      {{QStringLiteral("entity_id"), prop(QStringLiteral("string"), QStringLiteral("Например light.desk"))}},
                      {QStringLiteral("entity_id")});
        d << function(QStringLiteral("smart_home_call"),
                      QStringLiteral("Выполнить действие с устройством Home Assistant. Домен берётся из entity_id."),
                      {{QStringLiteral("entity_id"), prop(QStringLiteral("string"), QStringLiteral("Например light.desk"))},
                       {QStringLiteral("service"), prop(QStringLiteral("string"), QStringLiteral("turn_on, turn_off, toggle, set_temperature, ..."))},
                       {QStringLiteral("data_json"), prop(QStringLiteral("string"), QStringLiteral("Доп. параметры JSON-строкой, например {\"brightness_pct\":40}"))}},
                      {QStringLiteral("entity_id"), QStringLiteral("service")});
    }
    return d;
}

void LocalTools::invoke(const QString& name, const QJsonObject& args, const Done& done)
{
    const QDateTime now = QDateTime::currentDateTime();

    if (name == QLatin1String("set_timer")) {
        const qint64 secs = qRound64(args.value(QStringLiteral("seconds")).toDouble());
        if (secs <= 0 || secs > 7 * 24 * 3600)
            return done(error(QStringLiteral("seconds должно быть от 1 до 604800")));
        return done(addEntry(QStringLiteral("timer"), now.addSecs(secs), args.value(QStringLiteral("label")).toString()));
    }
    if (name == QLatin1String("set_reminder")) {
        const QString text = args.value(QStringLiteral("text")).toString().trimmed();
        QDateTime due;
        if (args.contains(QStringLiteral("at")))
            due = parseWhen(args.value(QStringLiteral("at")).toString());
        else if (args.contains(QStringLiteral("in_minutes")))
            due = now.addSecs(qRound64(args.value(QStringLiteral("in_minutes")).toDouble() * 60));
        if (text.isEmpty())
            return done(error(QStringLiteral("пустой text")));
        if (!due.isValid() || due <= now)
            return done(error(QStringLiteral("время не распознано или уже прошло")));
        return done(addEntry(QStringLiteral("reminder"), due, text));
    }
    if (name == QLatin1String("list_timers"))
        return done(listEntries());
    if (name == QLatin1String("cancel_timer"))
        return done(cancelEntry(int(args.value(QStringLiteral("id")).toDouble())));
    if (name == QLatin1String("add_note"))
        return done(addNote(args.value(QStringLiteral("text")).toString()));
    if (name == QLatin1String("list_notes"))
        return done(listNotes(args.value(QStringLiteral("limit")).toInt(10)));

    if (name.startsWith(QLatin1String("smart_home_"))) {
        if (!hasSmartHome())
            return done(error(QStringLiteral("умный дом не подключён")));
        if (name == QLatin1String("smart_home_list"))
            return m_ha->listEntities(done);
        if (name == QLatin1String("smart_home_state"))
            return m_ha->state(args.value(QStringLiteral("entity_id")).toString(), done);
        if (name == QLatin1String("smart_home_call"))
            return smartHomeCall(args, done);
    }
    done(error(QStringLiteral("неизвестная функция ") + name));
}

void LocalTools::smartHomeCall(const QJsonObject& args, const Done& done)
{
    const QString entity = args.value(QStringLiteral("entity_id")).toString();
    const QString service = args.value(QStringLiteral("service")).toString();
    QJsonObject data;
    const QString rawData = args.value(QStringLiteral("data_json")).toString().trimmed();
    if (!rawData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(rawData.toUtf8());
        if (!doc.isObject())
            return done(error(QStringLiteral("data_json должен быть JSON-объектом")));
        data = doc.object();
    }
    if (!m_ha->isAllowed(entity))
        return done(error(QStringLiteral("устройство не в белом списке homeassistant/allowedEntities")));

    if (m_ha->needsConfirmation(entity)) {
        if (!isSignalConnected(QMetaMethod::fromSignal(&LocalTools::confirmationNeeded)))
            return done(error(QStringLiteral("нужно подтверждение, но спросить некого")));
        const QString question = tr("Джеминик хочет выполнить «%1» для %2. Разрешить?").arg(service, entity);
        emit confirmationNeeded(question, [this, entity, service, data, done](bool allowed) {
            if (allowed)
                m_ha->call(entity, service, data, done);
            else
                done(error(QStringLiteral("пользователь запретил это действие")));
        });
        return;
    }
    m_ha->call(entity, service, data, done);
}

// ---------------------------------------------------------------------------------------------
// Команды без модели

bool LocalTools::handleCommand(const QString& input, QString* reply)
{
    const QString in = input.trimmed();
    if (!in.startsWith(QLatin1Char('/')))
        return false;
    const QString cmd = in.section(QLatin1Char(' '), 0, 0).toLower();
    const QString rest = in.section(QLatin1Char(' '), 1).trimmed();
    const QString head = rest.section(QLatin1Char(' '), 0, 0);
    const QString tail = rest.section(QLatin1Char(' '), 1).trimmed();

    if (cmd == QLatin1String("/help")) {
        *reply = tr("**Команды** (работают без интернета):\n"
                    "- `/timer 25m [метка]`: таймер (s, m, h, мин, ч)\n"
                    "- `/remind 18:30 текст` или `/remind 15m текст`\n"
                    "- `/timers`, `/cancel <id>`\n"
                    "- `/note текст`, `/notes`\n"
                    "- `/web вопрос`: ответ с поиском Google\n"
                    "- `/clear`: забыть диалог\n\n"
                    "Остальное можно просто попросить словами.");
        return true;
    }
    if (cmd == QLatin1String("/timer")) {
        const qint64 secs = parseDurationSecs(head);
        if (secs <= 0) {
            *reply = tr("Формат: `/timer 25m [метка]`");
            return true;
        }
        const QJsonObject r = addEntry(QStringLiteral("timer"), QDateTime::currentDateTime().addSecs(secs), tail);
        *reply = tr("⏱ #%1 %2: через %3").arg(r.value(QStringLiteral("id")).toInt())
                     .arg(tail.isEmpty() ? tr("таймер") : tail, formatLeft(secs));
        return true;
    }
    if (cmd == QLatin1String("/remind")) {
        const QDateTime due = parseWhen(head);
        if (!due.isValid() || tail.isEmpty()) {
            *reply = tr("Формат: `/remind 18:30 текст` или `/remind 15m текст`");
            return true;
        }
        const QJsonObject r = addEntry(QStringLiteral("reminder"), due, tail);
        *reply = tr("🔔 #%1 напомню %2: %3").arg(r.value(QStringLiteral("id")).toInt())
                     .arg(due.toString(QStringLiteral("dd.MM HH:mm")), tail);
        return true;
    }
    if (cmd == QLatin1String("/timers")) {
        if (m_entries.isEmpty()) {
            *reply = tr("Ничего не тикает.");
            return true;
        }
        QStringList lines;
        const QDateTime now = QDateTime::currentDateTime();
        for (const Entry& e : std::as_const(m_entries))
            lines << QStringLiteral("- #%1 %2 %3 (через %4)")
                         .arg(e.id)
                         .arg(e.kind == QLatin1String("timer") ? QStringLiteral("⏱") : QStringLiteral("🔔"),
                              e.text.isEmpty() ? tr("таймер") : e.text, formatLeft(secsUntil(e.due)));
        *reply = lines.join(QLatin1Char('\n'));
        return true;
    }
    if (cmd == QLatin1String("/cancel")) {
        const QJsonObject r = cancelEntry(head.toInt());
        *reply = r.value(QStringLiteral("ok")).toBool() ? tr("Отменил #%1.").arg(head) : tr("Нет такого id.");
        return true;
    }
    if (cmd == QLatin1String("/note")) {
        if (rest.isEmpty()) {
            *reply = tr("Формат: `/note текст`");
            return true;
        }
        addNote(rest);
        *reply = tr("📝 Записал.");
        return true;
    }
    if (cmd == QLatin1String("/notes")) {
        const QJsonArray items = listNotes(10).value(QStringLiteral("notes")).toArray();
        QStringList lines;
        for (const QJsonValue& v : items)
            lines << QStringLiteral("- ") + v.toObject().value(QStringLiteral("text")).toString();
        *reply = lines.isEmpty() ? tr("Заметок пока нет.") : lines.join(QLatin1Char('\n'));
        return true;
    }
    *reply = tr("Не знаю команду %1. Список: `/help`").arg(cmd);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Таймеры, напоминания, заметки

QJsonObject LocalTools::addEntry(const QString& kind, const QDateTime& due, const QString& text)
{
    Entry e{m_nextId++, kind, text.trimmed().left(200), due};
    m_entries << e;
    saveSchedule();
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("id"), e.id},
                       {QStringLiteral("fires_at"), due.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))},
                       {QStringLiteral("in"), formatLeft(secsUntil(due))}};
}

QJsonObject LocalTools::listEntries() const
{
    QJsonArray items;
    const QDateTime now = QDateTime::currentDateTime();
    for (const Entry& e : m_entries)
        items << QJsonObject{{QStringLiteral("id"), e.id},
                             {QStringLiteral("kind"), e.kind},
                             {QStringLiteral("text"), e.text},
                             {QStringLiteral("fires_at"), e.due.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))},
                             {QStringLiteral("in"), formatLeft(secsUntil(e.due))}};
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("items"), items}};
}

QJsonObject LocalTools::cancelEntry(int id)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id == id) {
            m_entries.removeAt(i);
            saveSchedule();
            return QJsonObject{{QStringLiteral("ok"), true}};
        }
    }
    return error(QStringLiteral("нет такого id"));
}

QJsonObject LocalTools::addNote(const QString& text)
{
    const QString t = text.trimmed().left(4000);
    if (t.isEmpty())
        return error(QStringLiteral("пустая заметка"));
    m_notes.append(QJsonObject{{QStringLiteral("text"), t},
                               {QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate)}});
    saveNotes();
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("total"), m_notes.size()}};
}

QJsonObject LocalTools::listNotes(int limit) const
{
    limit = qBound(1, limit, 50);
    QJsonArray out;
    for (qsizetype i = qMax<qsizetype>(0, m_notes.size() - limit); i < m_notes.size(); ++i)
        out << m_notes.at(i);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("notes"), out}};
}

void LocalTools::tick()
{
    const QDateTime now = QDateTime::currentDateTime();
    QList<Entry> fired;
    for (int i = 0; i < m_entries.size();) {
        if (m_entries.at(i).due <= now)
            fired << m_entries.takeAt(i);
        else
            ++i;
    }
    if (fired.isEmpty())
        return;
    saveSchedule();

    for (const Entry& e : std::as_const(fired)) {
        QString text = e.text;
        if (e.due.secsTo(now) > 120)   // ПК спал или приложение было закрыто
            text += tr(" (опоздал: было на %1)").arg(e.due.toString(QStringLiteral("dd.MM HH:mm")));
        if (e.kind == QLatin1String("timer"))
            emit alarm(tr("⏱ Таймер"), text.isEmpty() ? tr("Время вышло!") : tr("%1: время вышло!").arg(text));
        else
            emit alarm(tr("🔔 Напоминание"), text);
    }
}

// ---------------------------------------------------------------------------------------------
// Хранение (AppData/Geminik/geminik/*.json)

QString LocalTools::dataFile(const QString& name) const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + name;
}

void LocalTools::load()
{
    QFile sched(dataFile(QStringLiteral("schedule.json")));
    if (sched.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(sched.readAll()).object();
        m_nextId = qMax(1, root.value(QStringLiteral("nextId")).toInt(1));
        for (const QJsonValue& v : root.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject o = v.toObject();
            Entry e{o.value(QStringLiteral("id")).toInt(), o.value(QStringLiteral("kind")).toString(),
                    o.value(QStringLiteral("text")).toString(),
                    QDateTime::fromSecsSinceEpoch(qint64(o.value(QStringLiteral("due")).toDouble()))};
            if (e.id > 0 && e.due.isValid())
                m_entries << e;
        }
    }
    QFile notes(dataFile(QStringLiteral("notes.json")));
    if (notes.open(QIODevice::ReadOnly))
        m_notes = QJsonDocument::fromJson(notes.readAll()).array();
}

void LocalTools::saveSchedule() const
{
    QJsonArray entries;
    for (const Entry& e : m_entries)
        entries << QJsonObject{{QStringLiteral("id"), e.id},
                               {QStringLiteral("kind"), e.kind},
                               {QStringLiteral("text"), e.text},
                               {QStringLiteral("due"), double(e.due.toSecsSinceEpoch())}};
    QSaveFile f(dataFile(QStringLiteral("schedule.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("nextId"), m_nextId},
                                          {QStringLiteral("entries"), entries}}).toJson());
        f.commit();
    }
}

void LocalTools::saveNotes() const
{
    QSaveFile f(dataFile(QStringLiteral("notes.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_notes).toJson());
        f.commit();
    }
}

// ---------------------------------------------------------------------------------------------
// Разбор времени

// "90s", "25m", "1h30m", "1ч 15мин", "25" (= минуты). -1, если не разобрали.
qint64 LocalTools::parseDurationSecs(const QString& s)
{
    static const QRegularExpression token(
        QStringLiteral("(\\d+)\\s*(час|ч|hr|h|мин|min|м|m|сек|sec|с|s)?"),
        QRegularExpression::CaseInsensitiveOption);
    const QString in = s.trimmed().toLower();
    if (in.isEmpty())
        return -1;

    qint64 total = 0;
    auto it = token.globalMatch(in);
    while (it.hasNext()) {
        const auto m = it.next();
        const qint64 n = m.captured(1).toLongLong();
        const QString unit = m.captured(2);
        qint64 mult = 60;
        if (unit.startsWith(QLatin1Char('h')) || unit.startsWith(QChar(0x0447)))   // h, ч, час
            mult = 3600;
        else if (unit == QLatin1String("s") || unit == QLatin1String("sec") || unit.startsWith(QChar(0x0441)))   // с, сек
            mult = 1;
        total += n * mult;
    }
    QString leftover = in;
    leftover.remove(token);
    if (!leftover.trimmed().isEmpty())
        return -1;
    return total > 0 ? total : -1;
}

// "18:30" (сегодня или завтра), "2026-10-13T09:00", "2026-10-13 09:00", "15m".
QDateTime LocalTools::parseWhen(const QString& s)
{
    const QString in = s.trimmed();
    const QDateTime now = QDateTime::currentDateTime();

    static const QRegularExpression hm(QStringLiteral("^(\\d{1,2})[:.](\\d{2})$"));
    if (const auto m = hm.match(in); m.hasMatch()) {
        const QTime t(m.captured(1).toInt(), m.captured(2).toInt());
        if (!t.isValid())
            return {};
        QDateTime dt(now.date(), t);
        if (dt <= now)
            dt = dt.addDays(1);
        return dt;
    }

    QString iso = in;
    iso.replace(QLatin1Char(' '), QLatin1Char('T'));
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    if (dt.isValid())
        return dt;

    const qint64 secs = parseDurationSecs(in);
    return secs > 0 ? now.addSecs(secs) : QDateTime();
}

QString LocalTools::formatLeft(qint64 secs)
{
    secs = qMax<qint64>(0, secs);
    const qint64 h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (h > 0)
        return m > 0 ? QStringLiteral("%1 ч %2 мин").arg(h).arg(m) : QStringLiteral("%1 ч").arg(h);
    if (m > 0)
        return s > 0 && m < 5 ? QStringLiteral("%1 мин %2 с").arg(m).arg(s) : QStringLiteral("%1 мин").arg(m);
    return QStringLiteral("%1 с").arg(s);
}
