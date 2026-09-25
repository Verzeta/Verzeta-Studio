// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include "../../backend/voice/voice-protocol.h"

using namespace Verzeta::Voice;

namespace {

void writeFrame(const QJsonObject& header) {
    const QByteArray bytes = encodeInferFrame(header);
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}

bool readExact(char* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const size_t r = std::fread(dst + got, 1, n - got, stdin);
        if (r == 0)
            return false;
        got += r;
    }
    return true;
}

bool readFrame(VoiceFrame* out) {
    char lenBuf[4];
    if (!readExact(lenBuf, 4))
        return false;
    const quint32 frameLen = (static_cast<quint8>(lenBuf[0]) << 24) |
                             (static_cast<quint8>(lenBuf[1]) << 16) |
                             (static_cast<quint8>(lenBuf[2]) << 8) | static_cast<quint8>(lenBuf[3]);
    if (static_cast<qint64>(frameLen) > Verzeta::Infer::kMaxInferFrameBytes) {
        return false;
    }
    QByteArray payload(static_cast<qsizetype>(frameLen), Qt::Uninitialized);
    if (!readExact(payload.data(), frameLen))
        return false;
    return decodeInferPayload(payload, out);
}

}  // namespace

int main() {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char* modeEnv = std::getenv("FAKE_VOICE_MODE");
    const QByteArray mode(modeEnv ? modeEnv : "");

    if (mode == "die-instantly") {
        return 3;
    }

    VoiceFrame greet;
    if (!readFrame(&greet))
        return 1;
    if (greet.header.value(QLatin1String("op")).toString() != QLatin1String(kOpGreet)) {
        return 2;
    }

    if (mode == "silent") {
        char sink;
        while (std::fread(&sink, 1, 1, stdin) == 1) {
        }
        return 0;
    }

    QJsonObject greeted;
    greeted.insert(QLatin1String("op"), QLatin1String(kEvGreeted));
    greeted.insert(QLatin1String("proto_version"), mode == "skew" ? 99 : kVoiceProtocolVersion);
    greeted.insert(QLatin1String("pack_version"), QLatin1String("0.0-test"));
    greeted.insert(QLatin1String("voices"),
                   QJsonArray{QLatin1String("test-alto"), QLatin1String("test-bass")});
    greeted.insert(QLatin1String("stt_models"), QJsonArray{QLatin1String("base")});
    writeFrame(greeted);

    if (mode == "die-after-greet") {
        return 3;
    }
    if (mode == "emit-event") {
        QJsonObject levels;
        levels.insert(QLatin1String("op"), QLatin1String(kEvLevels));
        levels.insert(QLatin1String("user_rms"), 0.5);
        writeFrame(levels);
    }

    QString speakingMsgId;
    VoiceFrame frame;
    while (readFrame(&frame)) {
        const QString op = frame.header.value(QLatin1String("op")).toString();
        if (op == QLatin1String(kOpShutdown)) {
            return 0;
        }
        if (mode != "call") {
            continue;
        }
        if (op == QLatin1String(kOpCallStart)) {
            QJsonObject started;
            started.insert(QLatin1String("op"), QLatin1String(kEvCallStarted));
            started.insert(QLatin1String("call_id"), frame.header.value(QLatin1String("call_id")));
            writeFrame(started);
            continue;
        }
        if (op == QLatin1String(kOpTtsSpeak)) {
            const QString msgId = frame.header.value(QLatin1String("msg_id")).toString();
            if (msgId != speakingMsgId) {
                speakingMsgId = msgId;
                QJsonObject ev;
                ev.insert(QLatin1String("op"), QLatin1String(kEvTtsStarted));
                ev.insert(QLatin1String("msg_id"), msgId);
                writeFrame(ev);
            }
            continue;
        }
        if (op == QLatin1String(kOpTtsCancel)) {
            if (!speakingMsgId.isEmpty()) {
                QJsonObject ev;
                ev.insert(QLatin1String("op"), QLatin1String(kEvTtsCancelled));
                ev.insert(QLatin1String("msg_id"), speakingMsgId);
                speakingMsgId.clear();
                writeFrame(ev);
            }
            continue;
        }
        if (op == QLatin1String(kOpPttSet) &&
            !frame.header.value(QLatin1String("pressed")).toBool()) {
            QJsonObject ev;
            ev.insert(QLatin1String("op"), QLatin1String(kEvSttFinal));
            ev.insert(QLatin1String("text"), QLatin1String("stop and listen"));
            ev.insert(QLatin1String("dur_ms"), 900);
            writeFrame(ev);
            continue;
        }
        if (op == QLatin1String(kOpCallEnd)) {
            QJsonObject ev;
            ev.insert(QLatin1String("op"), QLatin1String(kEvCallEnded));
            ev.insert(QLatin1String("call_id"), frame.header.value(QLatin1String("call_id")));
            writeFrame(ev);
            continue;
        }
    }
    return 0;
}
