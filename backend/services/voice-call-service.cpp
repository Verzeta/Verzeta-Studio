// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-call-service.cpp
 * @brief Detection (ordered filesystem search, authoritative explicit
 *        path, event-driven re-detect) and daemon lifecycle policy
 *        (enable/disable, greet version check, spawn-failure latch,
 *        autostart).
 * @layer Service
 * @dependencies VoiceSidecarHost, Qt6::Core (QStandardPaths, QFileInfo,
 *               QDir), voice-protocol.h.
 */


#include "voice-call-service.h"

#include "../voice/voice-protocol.h"
#include "../voice/voice-text.h"
#include "voice-sidecar-host.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QUuid>

Q_LOGGING_CATEGORY(verzetaVoice, "verzeta.voice", QtInfoMsg)

namespace Verzeta::Voice {

namespace {
/** Consecutive start failures after which enable() refuses until the
 *  next Detect. Degrades a broken install once, quietly. */
constexpr int kMaxConsecutiveStartFailures = 3;
}  // namespace

VoiceCallService::VoiceCallService(Persistence persistence, QObject* parent)
    : QObject(parent)
    , m_persist(std::move(persistence))
    , m_host(std::make_unique<VoiceSidecarHost>()) {
    if (m_persist.readExplicitPath) {
        m_explicitPath = m_persist.readExplicitPath();
    }
    if (m_persist.readAutostart) {
        m_autostart = m_persist.readAutostart();
    }
    if (m_persist.readDefaultVoice) {
        m_defaultVoice = m_persist.readDefaultVoice();
    }
    if (m_persist.readPttHotkey) {
        m_pttHotkey = m_persist.readPttHotkey();
        m_pttSequence = QKeySequence(m_pttHotkey, QKeySequence::PortableText);
    }
    if (m_persist.readVoiceAssignments) {
        // {conversationId: {alias: voiceId}}. A value that is not a map
        // is from the earlier flat per-alias shape, which shipped in no
        // release; it is ignored rather than migrated.
        const QVariantMap saved = m_persist.readVoiceAssignments();
        for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
            if (it.key().isEmpty())
                continue;
            if (it.value().typeId() != QMetaType::QVariantMap)
                continue;
            const QVariantMap members = it.value().toMap();
            QHash<QString, QString> perMember;
            for (auto m = members.cbegin(); m != members.cend(); ++m) {
                const QString voiceId = m.value().toString();
                if (!voiceId.isEmpty())
                    perMember.insert(m.key(), voiceId);
            }
            if (!perMember.isEmpty()) {
                m_voiceAssignments.insert(it.key(), perMember);
            }
        }
    }

    connect(m_host.get(), &VoiceSidecarHost::greeted, this, &VoiceCallService::onGreeted);
    connect(m_host.get(), &VoiceSidecarHost::startFailed, this, &VoiceCallService::onStartFailed);
    connect(m_host.get(), &VoiceSidecarHost::stopped, this, &VoiceCallService::onStopped);
    connect(m_host.get(), &VoiceSidecarHost::eventReceived, this, &VoiceCallService::onDaemonEvent);

    // Application-start detection (the only automatic one; every later
    // run is user-triggered — no watcher, no timer, no polling).
    detect();

    if (m_autostart && m_detected) {
        enable();
    }
}

VoiceCallService::~VoiceCallService() = default;

QString VoiceCallService::binaryFileName() {
    QString name = QLatin1String(kVoiceBinaryBaseName);
#ifdef Q_OS_WIN
    name += QLatin1String(".exe");
#endif
    return name;
}

QStringList VoiceCallService::binaryNamePatterns() {
    // The add-on ships in several shapes and users rename downloads, so
    // match every form it plausibly arrives as rather than one literal
    // file name. Order is preference: a plain executable first, then the
    // packaged single-file forms.
#ifdef Q_OS_WIN
    return {QStringLiteral("verzeta-voice.exe"),
            QStringLiteral("Verzeta-Voice.exe"),
            QStringLiteral("Verzeta-Voice*.exe")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("verzeta-voice"), QStringLiteral("Verzeta-Voice")};
#else
    // An AppImage is directly executable and speaks stdio like any other
    // binary, so it can be spawned as-is once found.
    return {QStringLiteral("verzeta-voice"),
            QStringLiteral("Verzeta-Voice"),
            QStringLiteral("verzeta-voice*.AppImage"),
            QStringLiteral("Verzeta-Voice*.AppImage")};
#endif
}

bool VoiceCallService::isExecutableFile(const QString& filePath) {
    const QFileInfo fi(filePath);
    return fi.exists() && fi.isFile() && fi.isExecutable();
}

QString VoiceCallService::resolveInDirectory(const QString& dir, const QStringList& patterns) {
    const QDir d(dir);
    if (!d.exists()) {
        return {};
    }
    for (const QString& pattern : patterns) {
        if (pattern.contains(QLatin1Char('*'))) {
            // Newest first, so an upgraded AppImage left beside an old
            // one wins without the user deleting anything.
            const QStringList hits = d.entryList({pattern}, QDir::Files, QDir::Time);
            for (const QString& hit : hits) {
                const QString full = d.absoluteFilePath(hit);
                if (isExecutableFile(full))
                    return full;
            }
            continue;
        }
        const QString full = d.absoluteFilePath(pattern);
        if (isExecutableFile(full))
            return full;
    }
    return {};
}

QString VoiceCallService::resolveBinaryPath(const QString& explicitPath,
                                            const QStringList& wellKnownDirs,
                                            const QStringList& pathDirs,
                                            const QString& appDir,
                                            const QStringList& patterns) {
    // An explicit path is authoritative: never fall through to a
    // different copy than the one the user named (predictability beats
    // availability here; the failure is surfaced, not papered over).
    // A directory is accepted too, so pointing at an unpacked download
    // or a macOS bundle's MacOS folder works without hunting for the
    // executable inside it.
    if (!explicitPath.isEmpty()) {
        if (isExecutableFile(explicitPath))
            return explicitPath;
        const QFileInfo fi(explicitPath);
        if (fi.isDir()) {
            const QString inDir = resolveInDirectory(explicitPath, patterns);
            if (!inDir.isEmpty())
                return inDir;
            // A macOS .app is a directory; look where its binary lives.
            const QString inBundle =
                resolveInDirectory(explicitPath + QLatin1String("/Contents/MacOS"), patterns);
            if (!inBundle.isEmpty())
                return inBundle;
        }
        return {};
    }
    QStringList dirs = wellKnownDirs;
    dirs += pathDirs;
    if (!appDir.isEmpty()) {
        dirs += appDir;
    }
    for (const QString& dir : dirs) {
        if (dir.isEmpty())
            continue;
        const QString hit = resolveInDirectory(dir, patterns);
        if (!hit.isEmpty())
            return hit;
    }
    return {};
}

QStringList VoiceCallService::commonInstallDirs() {
    // Where the add-on realistically lands: the per-user folder this app
    // documents, system prefixes, package/opt layouts, and the folders
    // browsers and users actually drop an AppImage or a portable build
    // into. PATH is searched separately, so this list is about the
    // places a working install can exist WITHOUT being on PATH.
    QStringList dirs;
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                QLatin1String("/voice");
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
#ifdef Q_OS_WIN
    const QString localApp = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    dirs << localApp + QLatin1String("/Programs/Verzeta-Voice")
         << qEnvironmentVariable("ProgramFiles") + QLatin1String("/Verzeta-Voice")
         << qEnvironmentVariable("ProgramFiles(x86)") + QLatin1String("/Verzeta-Voice")
         << home + QLatin1String("/Downloads");
#elif defined(Q_OS_MACOS)
    dirs << QLatin1String("/Applications/Verzeta-Voice.app/Contents/MacOS")
         << home + QLatin1String("/Applications/Verzeta-Voice.app/Contents/MacOS")
         << QLatin1String("/usr/local/bin") << QLatin1String("/opt/homebrew/bin")
         << home + QLatin1String("/Downloads");
#else
    dirs << QLatin1String("/usr/local/bin") << QLatin1String("/usr/bin")
         << QLatin1String("/opt/verzeta-voice/bin") << QLatin1String("/opt/verzeta-voice")
         << home + QLatin1String("/.local/bin") << home + QLatin1String("/Applications")
         << home + QLatin1String("/.local/share/applications")
         << home + QLatin1String("/Downloads");
#endif
    dirs.removeAll(QString());
    dirs.removeDuplicates();
    return dirs;
}

QString VoiceCallService::wellKnownDir() const {
    // AppDataLocation resolves under the app's own org/app identity, so
    // the install hint shown in Settings always matches what the search
    // actually scans; documentation cannot drift from the code.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QLatin1String("/voice");
}

void VoiceCallService::setExplicitPath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed == m_explicitPath) {
        return;
    }
    m_explicitPath = trimmed;
    if (m_persist.writeExplicitPath) {
        m_persist.writeExplicitPath(m_explicitPath);
    }
    emit explicitPathChanged();
    detect();
}

void VoiceCallService::setAutostart(bool on) {
    if (on == m_autostart) {
        return;
    }
    m_autostart = on;
    if (m_persist.writeAutostart) {
        m_persist.writeAutostart(on);
    }
    emit autostartChanged();
}

void VoiceCallService::setDefaultVoice(const QString& voiceId) {
    if (voiceId == m_defaultVoice)
        return;
    m_defaultVoice = voiceId;
    if (m_persist.writeDefaultVoice) {
        m_persist.writeDefaultVoice(voiceId);
    }
    // A live call picks this up on the next thing anyone says: the voice
    // id travels with every speak request, not just with call.start.
    emit voiceAssignmentsChanged();
}

void VoiceCallService::setPttHotkey(const QString& sequence) {
    if (sequence == m_pttHotkey)
        return;
    m_pttHotkey = sequence;
    m_pttSequence = QKeySequence(sequence, QKeySequence::PortableText);
    if (m_persist.writePttHotkey) {
        m_persist.writePttHotkey(sequence);
    }
    updatePttFilter();
    emit pttHotkeyChanged();
}

QString VoiceCallService::hotkeyFromEvent(int key, int modifiers) {
    // A modifier by itself is half a combination; binding it would fire
    // on every Ctrl+C, so the capture field must wait for a real key.
    switch (key) {
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Alt:
        case Qt::Key_Meta:
        case Qt::Key_AltGr:
        case Qt::Key_unknown:
            return {};
        default:
            break;
    }
    return QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers), Qt::Key(key)))
        .toString(QKeySequence::PortableText);
}

void VoiceCallService::updatePttFilter() {
    // The filter exists exactly while it can do something: a live call
    // with a bound key. Outside that window the application's event flow
    // is byte-for-byte what it was before voice existed.
    const bool wanted = callActive() && !m_pttSequence.isEmpty();
    if (wanted == m_pttFilterInstalled)
        return;
    if (wanted) {
        QCoreApplication::instance()->installEventFilter(this);
    } else {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    m_pttFilterInstalled = wanted;
}

bool VoiceCallService::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if (type != QEvent::KeyPress && type != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->isAutoRepeat()) {
        // Holding the key streams repeats; the mic is already open.
        // Swallow repeats of OUR key only, pass everything else.
        return QKeyCombination(key->modifiers(), Qt::Key(key->key())) == m_pttSequence[0];
    }
    if (QKeyCombination(key->modifiers(), Qt::Key(key->key())) != m_pttSequence[0]) {
        return QObject::eventFilter(watched, event);
    }
    // Never steal keys from typing: with a text editor focused the combo
    // is the user's text, not push-to-talk.
    if (const QGuiApplication* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        if (QObject* focus = gui->focusObject()) {
            if (focus->inherits("QQuickTextInput") || focus->inherits("QQuickTextEdit")) {
                return QObject::eventFilter(watched, event);
            }
        }
    }
    setTalking(type == QEvent::KeyPress);
    return true;  // Consumed as push-to-talk.
}

QString VoiceCallService::assignedVoiceForMember(const QString& conversationId,
                                                 const QString& alias) const {
    return m_voiceAssignments.value(conversationId).value(alias);
}

QString VoiceCallService::voiceForMember(const QString& conversationId,
                                         const QString& alias) const {
    const QHash<QString, QString> perMember = m_voiceAssignments.value(conversationId);
    QString voiceId = perMember.value(alias);
    if (voiceId.isEmpty() && !alias.isEmpty()) {
        // A project one-to-one can speak under a member alias while its
        // voice was set through the conversation's own Voice section
        // (stored under the empty alias), so that rung is next.
        voiceId = perMember.value(QString());
    }
    return voiceId.isEmpty() ? m_defaultVoice : voiceId;
}

void VoiceCallService::setVoiceForMember(const QString& conversationId,
                                         const QString& alias,
                                         const QString& voiceId) {
    if (conversationId.isEmpty())
        return;
    if (assignedVoiceForMember(conversationId, alias) == voiceId)
        return;
    if (voiceId.isEmpty()) {
        auto it = m_voiceAssignments.find(conversationId);
        if (it != m_voiceAssignments.end()) {
            it->remove(alias);  // Back to the default-voice rung.
            if (it->isEmpty())
                m_voiceAssignments.erase(it);
        }
    } else {
        m_voiceAssignments[conversationId].insert(alias, voiceId);
    }
    if (m_persist.writeVoiceAssignments) {
        QVariantMap out;
        for (auto it = m_voiceAssignments.cbegin(); it != m_voiceAssignments.cend(); ++it) {
            QVariantMap perMember;
            for (auto m = it.value().cbegin(); m != it.value().cend(); ++m) {
                perMember.insert(m.key(), m.value());
            }
            out.insert(it.key(), perMember);
        }
        m_persist.writeVoiceAssignments(out);
    }
    // Keep the live call's map in step so the change is audible on the
    // next thing this member says, not at the next call.
    if (callActive() && m_callConvId == conversationId) {
        if (voiceId.isEmpty()) {
            m_aliasVoices.remove(alias);
        } else {
            m_aliasVoices.insert(alias, voiceId);
        }
    }
    emit voiceAssignmentsChanged();
}

void VoiceCallService::detect() {
    // A user-triggered detect is a fresh chance: clear the latch.
    if (m_latched || m_consecutiveStartFailures > 0) {
        m_latched = false;
        m_consecutiveStartFailures = 0;
        emit serviceStateChanged();
    }

    const QString pathEnv = qEnvironmentVariable("PATH");
    const QStringList pathDirs = pathEnv.split(QDir::listSeparator(), Qt::SkipEmptyParts);

    const QString found = resolveBinaryPath(m_explicitPath,
                                            commonInstallDirs(),
                                            pathDirs,
                                            QCoreApplication::applicationDirPath(),
                                            binaryNamePatterns());

    const bool nowDetected = !found.isEmpty();
    if (nowDetected == m_detected && found == m_detectedPath) {
        return;  // Outcome unchanged; no signal churn.
    }
    m_detected = nowDetected;
    m_detectedPath = found;
    if (m_detected) {
        qCInfo(verzetaVoice) << "verzeta-voice detected at" << m_detectedPath;
    } else {
        qCInfo(verzetaVoice) << "verzeta-voice not detected"
                             << (m_explicitPath.isEmpty()
                                     ? "(searched well-known dir, PATH, appDir)"
                                     : "(explicit path invalid)");
    }
    emit detectionChanged();
}

void VoiceCallService::enable() {
    if (m_running) {
        return;
    }
    if (!m_detected) {
        m_lastError = tr("Verzeta-Voice is not detected. Install it and "
                         "press Detect.");
        emit serviceStateChanged();
        return;
    }
    if (m_latched) {
        m_lastError = tr("Voice service start failed repeatedly. Check the "
                         "installation, then press Detect to try again.");
        emit serviceStateChanged();
        return;
    }
    qCInfo(verzetaVoice) << "enabling voice service:" << m_detectedPath;
    m_host->start(m_detectedPath);
}

void VoiceCallService::disable() {
    m_userRequestedStop = true;
    m_host->stop();
}

void VoiceCallService::onGreeted(const QJsonObject& payload) {
    const int proto = payload.value(QLatin1String("proto_version")).toInt(-1);
    if (proto != kVoiceProtocolVersion) {
        // Refuse politely with both versions; no latch — every retry
        // re-fails fast and the message stays accurate.
        m_lastError = tr("Verzeta-Voice speaks protocol v%1 but this app "
                         "needs v%2. Update the Verzeta-Voice add-on.")
                          .arg(proto)
                          .arg(kVoiceProtocolVersion);
        qCWarning(verzetaVoice) << "greet version skew:" << proto << "needed"
                                << kVoiceProtocolVersion;
        m_host->stop();
        emit serviceStateChanged();
        return;
    }
    m_packVersion = payload.value(QLatin1String("pack_version")).toString();
    // Where the daemon reads models from, so downloads land in the
    // right place. Older packs omit the field; the dialog then keeps
    // its downloads disabled rather than guessing a path.
    m_modelsDir = payload.value(QLatin1String("models_dir")).toString();
    m_voices.clear();
    const QJsonArray voices = payload.value(QLatin1String("voices")).toArray();
    for (const auto& v : voices) {
        m_voices.append(v.toString());
    }
    m_running = true;
    m_consecutiveStartFailures = 0;
    m_lastError.clear();
    qCInfo(verzetaVoice) << "voice service running, pack" << m_packVersion << "voices"
                         << m_voices.size();
    emit serviceStateChanged();
}

void VoiceCallService::onStartFailed(const QString& reason) {
    m_running = false;
    ++m_consecutiveStartFailures;
    if (m_consecutiveStartFailures >= kMaxConsecutiveStartFailures) {
        m_latched = true;
    }
    m_lastError = m_latched ? tr("%1 (start failed %2 times; press Detect to try again)")
                                  .arg(reason)
                                  .arg(m_consecutiveStartFailures)
                            : reason;
    qCWarning(verzetaVoice) << "voice service start failed:" << reason << "consecutive"
                            << m_consecutiveStartFailures;
    emit serviceStateChanged();
}

void VoiceCallService::startCall(const QString& conversationId,
                                 const QVariantMap& aliasVoices,
                                 bool groupCall) {
    if (conversationId.isEmpty())
        return;
    if (!m_running) {
        m_lastError = tr("Start the voice service before calling.");
        emit serviceStateChanged();
        return;
    }
    if (callActive()) {
        m_lastError = tr("End the current call first.");
        emit serviceStateChanged();
        return;
    }
    m_callConvId = conversationId;
    m_callId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_callLive = false;
    m_groupCall = groupCall;
    m_targetAlias.clear();

    // Saved per-member assignments win; the caller's map only fills in
    // members with nothing saved, which is how a fresh group still comes
    // up with distinct voices instead of everyone sharing one.
    const QHash<QString, QString> saved = m_voiceAssignments.value(conversationId);
    QJsonObject voices;
    m_aliasVoices.clear();
    for (auto it = aliasVoices.cbegin(); it != aliasVoices.cend(); ++it) {
        const QString alias = it.key();
        const QString assigned = saved.value(alias);
        const QString voiceId = assigned.isEmpty() ? it.value().toString() : assigned;
        if (voiceId.isEmpty())
            continue;
        voices.insert(alias, voiceId);
        m_aliasVoices.insert(alias, voiceId);
    }
    for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
        if (m_aliasVoices.contains(it.key()))
            continue;
        voices.insert(it.key(), it.value());
        m_aliasVoices.insert(it.key(), it.value());
    }
    QJsonObject payload;
    payload.insert(QLatin1String("call_id"), m_callId);
    payload.insert(QLatin1String("mode"), QLatin1String("ptt"));
    payload.insert(QLatin1String("voices"), voices);
    m_host->sendOp(QLatin1String(kOpCallStart), payload);

    updatePttFilter();
    emit callStateChanged();
    emit targetAliasChanged();
}

void VoiceCallService::endCall() {
    if (!callActive())
        return;
    QJsonObject payload;
    payload.insert(QLatin1String("call_id"), m_callId);
    m_host->sendOp(QLatin1String(kOpCallEnd), payload);
    // Do not wait for call.ended: the UI closes now, and a daemon that
    // died mid-call must not leave the app stuck in a call.
    resetCallState();
}

bool VoiceCallService::cancelAllSpeech() {
    if (!callActive())
        return false;
    const QString held = m_heldMsgId;
    const bool wasSpeaking = !m_speakingAlias.isEmpty() || !held.isEmpty();
    QJsonObject payload;
    payload.insert(QLatin1String("all"), true);
    m_host->sendOp(QLatin1String(kOpTtsCancel), payload);
    if (!m_speakingAlias.isEmpty()) {
        m_speakingAlias.clear();
        emit speakingAliasChanged();
    }
    // Free the cascade before anything else can queue behind it. The
    // daemon's tts.cancelled echo arrives later and finds nothing held,
    // which is why the release is keyed on the message id.
    if (!held.isEmpty()) {
        m_heldMsgId.clear();
        m_spokenMsgIds.insert(held);
        while (m_spokenMsgIds.size() > 64) {
            m_spokenMsgIds.erase(m_spokenMsgIds.begin());
        }
        emit speechFinished(held, false);
    }
    return wasSpeaking;
}

void VoiceCallService::stopSpeaking() {
    if (!cancelAllSpeech())
        return;
    qCInfo(verzetaVoice) << "speech stopped by the user";
}

void VoiceCallService::setTalking(bool held) {
    if (!callActive() || held == m_talking)
        return;
    if (held && m_micMuted)
        return;  // Muted: the press does nothing.
    m_talking = held;
    QJsonObject payload;
    payload.insert(QLatin1String("pressed"), held);
    m_host->sendOp(QLatin1String(kOpPttSet), payload);
    if (!held) {
        m_micLevel = 0.0;
        emit micLevelChanged();
    }
    emit talkingChanged();
}

void VoiceCallService::setMicMuted(bool muted) {
    if (muted == m_micMuted)
        return;
    m_micMuted = muted;
    if (callActive()) {
        QJsonObject payload;
        payload.insert(QLatin1String("muted"), muted);
        m_host->sendOp(QLatin1String(kOpMicSet), payload);
    }
    if (muted && m_talking) {
        m_talking = false;
        emit talkingChanged();
    }
    emit micMutedChanged();
}

void VoiceCallService::setTargetAlias(const QString& alias) {
    if (alias == m_targetAlias)
        return;
    m_targetAlias = alias;
    emit targetAliasChanged();
}

void VoiceCallService::speakMessage(const QString& msgId,
                                    const QString& alias,
                                    const QString& markdown) {
    if (!callActive() || !m_callLive) {
        qCWarning(verzetaVoice) << "speak refused: no live call (active" << callActive() << "live"
                                << m_callLive << ") for" << msgId;
        return;
    }
    const QString speakable = toSpeakableText(markdown);
    const QStringList chunks = splitIntoSentences(speakable);
    qCInfo(verzetaVoice) << "speak" << msgId << "alias" << alias << "chunks" << chunks.size()
                         << "chars" << speakable.size();
    if (chunks.isEmpty()) {
        // Nothing to say (a tool-only turn, or code only). No audio is
        // requested, so no completion event will arrive: tell the caller
        // now so a pacing gate is never left waiting on silence.
        emit speechFinished(msgId, true);
        return;
    }
    m_msgAlias.insert(msgId, alias);
    // Resolved per message, not per call, so a voice changed mid-call
    // takes effect on the next thing that member says. The call map
    // carries the round-robin fill for members with no assignment;
    // voiceForMember covers the assignment, the one-to-one voice and
    // the default. Empty means the daemon picks.
    QString voiceId = m_aliasVoices.value(alias);
    if (voiceId.isEmpty())
        voiceId = voiceForMember(m_callConvId, alias);
    for (int i = 0; i < chunks.size(); ++i) {
        QJsonObject payload;
        payload.insert(QLatin1String("msg_id"), msgId);
        payload.insert(QLatin1String("seq"), i);
        payload.insert(QLatin1String("text"), chunks.at(i));
        payload.insert(QLatin1String("voice_id"), voiceId);
        payload.insert(QLatin1String("final"), i == chunks.size() - 1);
        m_host->sendOp(QLatin1String(kOpTtsSpeak), payload);
    }
}

bool VoiceCallService::shouldHoldTurn(const QString& convId, const QString& msgId) {
    // Hold only for the conversation actually on a call, and only for a
    // reply that exists. Anything else advances at text speed exactly as
    // it does without the voice add-on.
    if (msgId.isEmpty())
        return false;
    if (!m_callLive || convId != m_callConvId)
        return false;
    // One-to-one calls have no next member to order against, so holding
    // buys no correctness and costs a lot: the turn stays in flight for
    // the whole length of the audio, and anything the user says or types
    // meanwhile queues behind it instead of being answered.
    if (!m_groupCall)
        return false;
    // A reply whose speech already finished before the cascade asked
    // must not be waited on (the release can never come twice).
    if (m_spokenMsgIds.contains(msgId))
        return false;
    m_heldMsgId = msgId;
    return true;
}

void VoiceCallService::onDaemonEvent(const QString& op, const QJsonObject& payload) {
    qCDebug(verzetaVoice) << "daemon event" << op;
    if (op == QLatin1String(kEvCallStarted)) {
        m_callLive = true;
        emit callStateChanged();
        return;
    }
    if (op == QLatin1String(kEvCallEnded)) {
        resetCallState();
        return;
    }
    if (op == QLatin1String(kEvCallError)) {
        m_lastError = payload.value(QLatin1String("reason")).toString();
        qCWarning(verzetaVoice) << "call error:" << m_lastError;
        resetCallState();
        emit serviceStateChanged();
        return;
    }
    if (op == QLatin1String(kEvSttPartial)) {
        m_caption = payload.value(QLatin1String("text")).toString();
        emit captionChanged();
        return;
    }
    if (op == QLatin1String(kEvSttFinal)) {
        const QString text = payload.value(QLatin1String("text")).toString().trimmed();
        if (!m_caption.isEmpty()) {
            m_caption.clear();
            emit captionChanged();
        }
        if (text.isEmpty() || !callActive())
            return;
        // Barge-in. Speaking over an agent must cut it off, not wait it
        // out: the reply is already fully in chat, so only the audio is
        // lost. This MUST happen before the utterance is handed on,
        // because the cascade stays parked until the hold is freed and
        // anything sent while it is parked queues behind the turn
        // instead of being answered.
        const bool interrupted = cancelAllSpeech();
        // The mention comes from the UI selector, never from speech: the
        // user cannot say "@", and a transcript can never garble routing.
        const QString addressed =
            m_targetAlias.isEmpty() ? text : QStringLiteral("@%1 %2").arg(m_targetAlias, text);
        qCInfo(verzetaVoice) << "utterance" << text.size() << "chars"
                             << "target" << m_targetAlias << "interrupted speech" << interrupted;
        emit utteranceReady(m_callConvId, addressed);
        return;
    }
    if (op == QLatin1String(kEvTtsStarted)) {
        const QString msgId = payload.value(QLatin1String("msg_id")).toString();
        const QString alias = m_msgAlias.value(msgId);
        if (alias != m_speakingAlias) {
            m_speakingAlias = alias;
            emit speakingAliasChanged();
        }
        return;
    }
    if (op == QLatin1String(kEvTtsMsgDone) || op == QLatin1String(kEvTtsCancelled)) {
        const QString msgId = payload.value(QLatin1String("msg_id")).toString();
        m_msgAlias.remove(msgId);
        if (m_heldMsgId == msgId)
            m_heldMsgId.clear();
        // Bounded memory of finished speech, so a cascade that asks
        // about an already-spoken reply is not made to wait forever.
        m_spokenMsgIds.insert(msgId);
        while (m_spokenMsgIds.size() > 32) {
            m_spokenMsgIds.erase(m_spokenMsgIds.begin());
        }
        if (!m_speakingAlias.isEmpty()) {
            m_speakingAlias.clear();
            emit speakingAliasChanged();
        }
        emit speechFinished(msgId, op == QLatin1String(kEvTtsMsgDone));
        return;
    }
    if (op == QLatin1String(kEvLevels)) {
        m_micLevel = payload.value(QLatin1String("user_rms")).toDouble();
        emit micLevelChanged();
        return;
    }
    // vad.speech and model progress are not surfaced in this stage.
}

void VoiceCallService::resetCallState() {
    if (!callActive() && !m_callLive && !m_talking)
        return;
    m_callConvId.clear();
    // Call state is being torn down: the key must stop working NOW, not
    // at the next event. updatePttFilter reads callActive(), which is
    // false once m_callConvId clears.
    m_callId.clear();
    m_callLive = false;
    m_groupCall = false;
    m_msgAlias.clear();
    m_aliasVoices.clear();
    // A hold that outlived its call must not be carried into the next
    // one. The cascade releases its own parked turn when the gate is
    // detached, so nothing is stranded by clearing this here.
    m_heldMsgId.clear();
    if (m_talking) {
        m_talking = false;
        emit talkingChanged();
    }
    if (!m_speakingAlias.isEmpty()) {
        m_speakingAlias.clear();
        emit speakingAliasChanged();
    }
    if (!m_caption.isEmpty()) {
        m_caption.clear();
        emit captionChanged();
    }
    if (m_micLevel != 0.0) {
        m_micLevel = 0.0;
        emit micLevelChanged();
    }
    updatePttFilter();
    emit callStateChanged();
}

void VoiceCallService::restartService() {
    if (callActive()) {
        qCWarning(verzetaVoice) << "restart refused: a call is in progress";
        return;
    }
    if (!m_running) {
        enable();
        return;
    }
    // Signal-driven: the fresh start rides the stopped echo of the
    // clean shutdown, so there is no timer and no race with the old
    // process still holding the models open.
    m_restartPending = true;
    disable();
}

void VoiceCallService::onStopped(const QString& reason) {
    m_running = false;
    // A daemon that goes away takes the call with it; never leave the UI
    // believing a call is live.
    resetCallState();
    const bool userRequested = m_userRequestedStop;
    m_userRequestedStop = false;
    if (!reason.isEmpty()) {
        m_lastError = reason;
        qCWarning(verzetaVoice) << "voice service stopped:" << reason;
    } else if (userRequested) {
        // Only the user's own disable() clears the error display. A
        // policy stop (version skew) must keep its message visible.
        m_lastError.clear();
        qCInfo(verzetaVoice) << "voice service stopped (requested)";
    } else {
        qCInfo(verzetaVoice) << "voice service stopped (policy)";
    }
    emit serviceStateChanged();
    if (m_restartPending) {
        m_restartPending = false;
        // Only the requested half of a restart continues into a fresh
        // start; a crash during shutdown must not loop.
        if (userRequested)
            enable();
    }
}

}  // namespace Verzeta::Voice
