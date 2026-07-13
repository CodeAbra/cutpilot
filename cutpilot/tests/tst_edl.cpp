#include <QtTest/QtTest>

#include <QRegularExpression>

#include "cutpilot/core/timeline/EdlWriter.h"

using namespace cutpilot::core::timeline;

namespace {

// One parsed CMX 3600 cut event.
struct EdlEvent {
    int number = 0;
    QString reel;
    QString channel;
    QString transition;
    QString srcIn, srcOut, recIn, recOut;
    QString clipName;
};

struct ParsedEdl {
    QString title;
    QString fcm;
    QVector<EdlEvent> events;
    QString error;
};

// A strict CMX 3600 reader: a TITLE line, an FCM line, then numbered events
// whose statement line carries reel, channel, transition, and four
// timecodes, each optionally followed by FROM CLIP NAME comments. Anything
// else is a parse error.
ParsedEdl parseEdl(const QString &text)
{
    ParsedEdl parsed;
    const QRegularExpression eventLine(QStringLiteral(
        "^(\\d{3})\\s+(\\S{1,8})\\s+(V|A|B|A2|AA|NONE)\\s+(C|D|W\\d{3}|KB|K)"
        "\\s+(?:\\d+\\s+)?"
        "(\\d{2}:\\d{2}:\\d{2}[:;]\\d{2}) (\\d{2}:\\d{2}:\\d{2}[:;]\\d{2}) "
        "(\\d{2}:\\d{2}:\\d{2}[:;]\\d{2}) (\\d{2}:\\d{2}:\\d{2}[:;]\\d{2})$"));

    const QStringList lines =
        text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("TITLE: "))) {
            parsed.title = line.mid(7);
            continue;
        }
        if (line.startsWith(QStringLiteral("FCM: "))) {
            parsed.fcm = line.mid(5);
            continue;
        }
        if (line.startsWith(QStringLiteral("* FROM CLIP NAME: "))) {
            if (parsed.events.isEmpty()) {
                parsed.error =
                    QStringLiteral("clip name comment before any event");
                return parsed;
            }
            parsed.events.last().clipName = line.mid(18);
            continue;
        }
        if (line.startsWith(QLatin1Char('*')))
            continue; // other comments are legal
        const QRegularExpressionMatch match = eventLine.match(line);
        if (!match.hasMatch()) {
            parsed.error = QStringLiteral("unparseable line: %1").arg(line);
            return parsed;
        }
        EdlEvent event;
        event.number = match.captured(1).toInt();
        event.reel = match.captured(2);
        event.channel = match.captured(3);
        event.transition = match.captured(4);
        event.srcIn = match.captured(5);
        event.srcOut = match.captured(6);
        event.recIn = match.captured(7);
        event.recOut = match.captured(8);
        parsed.events.append(event);
    }
    if (parsed.title.isEmpty())
        parsed.error = QStringLiteral("missing TITLE");
    else if (parsed.fcm.isEmpty())
        parsed.error = QStringLiteral("missing FCM");
    return parsed;
}

qint64 timecodeFrames(const QString &tc, int rate)
{
    const QStringList parts = tc.split(QLatin1Char(':'));
    return ((parts[0].toLongLong() * 60 + parts[1].toLongLong()) * 60
            + parts[2].toLongLong())
        * rate
        + parts[3].toLongLong();
}

TimelineProject shotsProject()
{
    TimelineProject project;
    project.name = QStringLiteral("Shots");

    MediaAsset one;
    one.id = QStringLiteral("a1");
    one.filePath = QStringLiteral("/media/opening shot!.png");
    one.name = QStringLiteral("opening shot!.png");
    one.fps = Fps{ 24, 1 };
    one.durationFrames = 96;

    MediaAsset two;
    two.id = QStringLiteral("a2");
    two.filePath = QStringLiteral("/media/closer.png");
    two.name = QStringLiteral("closer.png");
    two.fps = Fps{ 24, 1 };
    two.durationFrames = 48;

    project.assets = { one, two };

    Segment first;
    first.id = QStringLiteral("s1");
    first.type = SegmentType::Generator;
    first.assetId = QStringLiteral("a1");
    first.timelineIn = 0;
    first.timelineOut = 96;
    first.sourceIn = 0;
    first.sourceOut = 96;

    Segment gap;
    gap.id = QStringLiteral("g1");
    gap.type = SegmentType::Gap;
    gap.timelineIn = 96;
    gap.timelineOut = 120;

    Segment second;
    second.id = QStringLiteral("s2");
    second.type = SegmentType::Clip;
    second.assetId = QStringLiteral("a2");
    second.timelineIn = 120;
    second.timelineOut = 168;
    second.sourceIn = 0;
    second.sourceOut = 48;

    Track track;
    track.name = QStringLiteral("V1");
    track.segments = { first, gap, second };

    Sequence sequence;
    sequence.name = QStringLiteral("Main");
    sequence.fps = Fps{ 24, 1 };
    sequence.tracks = { track };
    project.sequences = { sequence };
    return project;
}

} // namespace

class EdlTest : public QObject {
    Q_OBJECT

private slots:
    void writesAParseableCmx3600List();
    void recordTimecodesFollowTheTimelinePositions();
    void reelsAreSanitizedUppercaseStems();
    void emptySequenceStillWritesAValidHeader();
};

void EdlTest::writesAParseableCmx3600List()
{
    const TimelineProject project = shotsProject();
    const QString text = writeEdl(project.sequences[0], project,
                                  QStringLiteral("Canvas Cut"));

    const ParsedEdl parsed = parseEdl(text);
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
    QCOMPARE(parsed.title, QStringLiteral("Canvas Cut"));
    QCOMPARE(parsed.fcm, QStringLiteral("NON-DROP FRAME"));

    // Two cut events (the gap produces none), numbered from one, on V.
    QCOMPARE(parsed.events.size(), 2);
    QCOMPARE(parsed.events[0].number, 1);
    QCOMPARE(parsed.events[1].number, 2);
    for (const EdlEvent &event : parsed.events) {
        QCOMPARE(event.channel, QStringLiteral("V"));
        QCOMPARE(event.transition, QStringLiteral("C"));
    }
    QCOMPARE(parsed.events[0].clipName, QStringLiteral("opening shot!.png"));
    QCOMPARE(parsed.events[1].clipName, QStringLiteral("closer.png"));
}

void EdlTest::recordTimecodesFollowTheTimelinePositions()
{
    const TimelineProject project = shotsProject();
    const QString text =
        writeEdl(project.sequences[0], project, QStringLiteral("Cut"));
    const ParsedEdl parsed = parseEdl(text);
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));

    QCOMPARE(parsed.events[0].srcIn, QStringLiteral("00:00:00:00"));
    QCOMPARE(parsed.events[0].srcOut, QStringLiteral("00:00:04:00"));
    QCOMPARE(parsed.events[0].recIn, QStringLiteral("00:00:00:00"));
    QCOMPARE(parsed.events[0].recOut, QStringLiteral("00:00:04:00"));

    // The second event records after the gap: frame 120 = 00:00:05:00.
    QCOMPARE(parsed.events[1].recIn, QStringLiteral("00:00:05:00"));
    QCOMPARE(parsed.events[1].recOut, QStringLiteral("00:00:07:00"));

    // Source and record spans agree in length for every cut.
    for (const EdlEvent &event : parsed.events) {
        QCOMPARE(timecodeFrames(event.srcOut, 24)
                     - timecodeFrames(event.srcIn, 24),
                 timecodeFrames(event.recOut, 24)
                     - timecodeFrames(event.recIn, 24));
    }
}

void EdlTest::reelsAreSanitizedUppercaseStems()
{
    const TimelineProject project = shotsProject();
    const QString text =
        writeEdl(project.sequences[0], project, QStringLiteral("Cut"));
    const ParsedEdl parsed = parseEdl(text);
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));

    // "opening shot!.png" → letters and digits only, upper, eight chars.
    QCOMPARE(parsed.events[0].reel, QStringLiteral("OPENINGS"));
    QCOMPARE(parsed.events[1].reel, QStringLiteral("CLOSER"));
}

void EdlTest::emptySequenceStillWritesAValidHeader()
{
    TimelineProject project;
    project.name = QStringLiteral("Empty");
    Sequence sequence;
    sequence.name = QStringLiteral("Main");
    project.sequences = { sequence };

    const QString text = writeEdl(sequence, project, QString());
    const ParsedEdl parsed = parseEdl(text);
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
    QCOMPARE(parsed.title, QStringLiteral("Untitled"));
    QVERIFY(parsed.events.isEmpty());
}

QTEST_GUILESS_MAIN(EdlTest)
#include "tst_edl.moc"
