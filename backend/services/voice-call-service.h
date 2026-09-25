// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-call-service.h
 * @brief QML-facing service for voice calls with agents. Owns DETECTION
 *        of the external `verzeta-voice` daemon (a separately installed
 *        GPL pack, never shipped inside the app artifacts) and the
 *        daemon LIFECYCLE: enable/disable, the greet version check, the
 *        spawn-failure latch, and the start-with-app option. Call state
 *        and the cascade pacing gate arrive in later stages; until
 *        then the chat pipeline is untouched by construction: nothing
 *        here is reachable from any conversation code path.
 * @layer Service
 * @dependencies VoiceSidecarHost (transport), Qt6::Core. Persistence is
 *               injected as callbacks so the class has no link
 *               dependency on SettingsService and tests stay link-light.
 */

#pragma once

#include "chat/voice-turn-gate.h"

#include <functional>
#include <memory>
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace Verzeta::Voice {

class VoiceSidecarHost;

/**
 * @brief Detection + lifecycle of the external verzeta-voice daemon,
 *        exposed to QML (Settings → Voice Calls).
 *
 * Detection is event-driven only: it runs at construction (application
 * start), on the Detect button, and when the explicit path changes.
 * There is no filesystem watcher and no polling. Detection is a pure
 * filesystem probe; the daemon's pack version is learned from the greet
 * handshake when the service is enabled.
 *
 * Search order, first hit wins:
 *   1. The explicit path from settings, when set. An explicit path is
 *      authoritative: if it is invalid, detection FAILS rather than
 *      silently falling through to some other copy on the system.
 *   2. The per-user install dir: \<AppDataLocation\>/voice/.
 *   3. The PATH environment.
 *   4. applicationDirPath() (developer / from-source layouts only; inside
 *      an AppImage this is a read-only mount, so it can never be the
 *      end-user install location).
 *
 * Lifecycle policy owned here (the host is a dumb pipe):
 *   - enable() spawns the daemon at the detected path; the outcome is
 *     the greet handshake.
 *   - A greeted proto_version different from ours refuses politely with
 *     both versions in the error string; no latch (each retry re-fails
 *     fast and honestly).
 *   - Three consecutive start failures latch the service unavailable so
 *     a broken install degrades once, quietly. detect() clears the
 *     latch: a user action is a fresh chance.
 *   - The autostart option enables the service at application start
 *     when (and only when) detection succeeds.
 */
class VoiceCallService : public QObject, public ::Chat::IVoiceTurnGate {
    Q_OBJECT

    /** True when a verzeta-voice binary was found at the last detect. */
    Q_PROPERTY(bool detected READ detected NOTIFY detectionChanged)
    /** Absolute path of the detected binary; empty when not detected. */
    Q_PROPERTY(QString detectedPath READ detectedPath NOTIFY detectionChanged)
    /** User-set explicit binary path; empty means "search normally". */
    Q_PROPERTY(
        QString explicitPath READ explicitPath WRITE setExplicitPath NOTIFY explicitPathChanged)
    /** The per-user directory the app scans (shown in the install hint so
     *  documentation can never drift from the code). */
    Q_PROPERTY(QString wellKnownDir READ wellKnownDir CONSTANT)
    /** True while the daemon is running and has greeted. */
    Q_PROPERTY(bool running READ running NOTIFY serviceStateChanged)
    /** Daemon pack version from the greet; empty until first greet. */
    Q_PROPERTY(QString packVersion READ packVersion NOTIFY serviceStateChanged)
    /** TTS voice ids announced by the daemon's greet. */
    Q_PROPERTY(QStringList voices READ voices NOTIFY serviceStateChanged)
    /** The daemon's models directory from its greet; empty until the
     *  service has greeted. Downloads install here. */
    Q_PROPERTY(QString modelsDir READ modelsDir NOTIFY serviceStateChanged)
    /** Last enable/stop failure, empty when healthy. */
    Q_PROPERTY(QString lastError READ lastError NOTIFY serviceStateChanged)
    /** True after repeated start failures; cleared by Detect. */
    Q_PROPERTY(bool latched READ latched NOTIFY serviceStateChanged)
    /** Start the voice service automatically with the application. */
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged)
    /** Voice used by any agent without one of its own; empty means the
     *  daemon's own default. */
    Q_PROPERTY(
        QString defaultVoice READ defaultVoice WRITE setDefaultVoice NOTIFY voiceAssignmentsChanged)
    /** Optional hold-to-talk key, active ONLY while a call is live; empty
     *  means unbound (the on-screen button is always available). */
    Q_PROPERTY(QString pttHotkey READ pttHotkey WRITE setPttHotkey NOTIFY pttHotkeyChanged)

    /** True between callStarted and callEnded. */
    Q_PROPERTY(bool callActive READ callActive NOTIFY callStateChanged)
    /** Conversation the live call belongs to; empty when idle. */
    Q_PROPERTY(QString callConversationId READ callConversationId NOTIFY callStateChanged)
    /** True once the daemon has confirmed the call is live. */
    Q_PROPERTY(bool callLive READ callLive NOTIFY callStateChanged)
    /** True while the talk control is held (microphone open). */
    Q_PROPERTY(bool talking READ talking NOTIFY talkingChanged)
    /** True when the microphone is muted for this call. */
    Q_PROPERTY(bool micMuted READ micMuted WRITE setMicMuted NOTIFY micMutedChanged)
    /** Alias the next utterance is addressed to; empty means the group. */
    Q_PROPERTY(QString targetAlias READ targetAlias WRITE setTargetAlias NOTIFY targetAliasChanged)
    /** Alias currently being spoken by the daemon; empty when silent. */
    Q_PROPERTY(QString speakingAlias READ speakingAlias NOTIFY speakingAliasChanged)
    /** Live caption: the partial transcript while the user talks. */
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)
    /** Microphone level 0..1 while the talk control is held. */
    Q_PROPERTY(double micLevel READ micLevel NOTIFY micLevelChanged)

  public:
    /**
     * @brief Injected persistence, so the service never links the
     *        settings store and unit tests can observe every read and
     *        write. Any member may be null (tests): reads then default
     *        (empty path / autostart off) and writes stay in memory.
     */
    struct Persistence {
        /** @brief Reads the persisted explicit path ("" when unset). */
        std::function<QString()> readExplicitPath;
        /** @brief Persists the explicit path. */
        std::function<void(const QString&)> writeExplicitPath;
        /** @brief Reads the persisted autostart flag. */
        std::function<bool()> readAutostart;
        /** @brief Persists the autostart flag. */
        std::function<void(bool)> writeAutostart;
        /** @brief Reads the voice used when an agent has no own voice. */
        std::function<QString()> readDefaultVoice;
        /** @brief Persists the default voice id. */
        std::function<void(const QString&)> writeDefaultVoice;
        /** @brief Reads the persisted alias-to-voice assignments. */
        std::function<QVariantMap()> readVoiceAssignments;
        /** @brief Persists the alias-to-voice assignments. */
        std::function<void(const QVariantMap&)> writeVoiceAssignments;
        /** @brief Reads the push-to-talk hotkey ("" when unbound). */
        std::function<QString()> readPttHotkey;
        /** @brief Persists the push-to-talk hotkey. */
        std::function<void(const QString&)> writePttHotkey;
    };

    /**
     * @brief Constructs the service, runs the initial detection, and,
     *        when autostart is set and detection succeeded, enables the
     *        daemon.
     * @param persistence Injected persisted-settings accessors.
     * @param parent Owner in the Qt sense.
     */
    explicit VoiceCallService(Persistence persistence = {}, QObject* parent = nullptr);

    /**
     * @brief Stops the daemon (via the host's destructor discipline) and
     *        releases the IO thread.
     */
    ~VoiceCallService() override;

    /**
     * @brief Whether the daemon binary was found by the last detection.
     * @returns True when a runnable verzeta-voice binary was found.
     */
    bool detected() const { return m_detected; }

    /**
     * @brief The location the last detection found.
     * @returns The detected binary's absolute path, or empty.
     */
    QString detectedPath() const { return m_detectedPath; }

    /**
     * @brief The user-set explicit path override.
     * @returns The override, or empty when the standard search applies.
     */
    QString explicitPath() const { return m_explicitPath; }

    /**
     * @brief Sets (and persists) the explicit binary path, then re-runs
     *        detection. An empty string clears the override.
     * @param path Absolute path to a verzeta-voice binary, or empty.
     */
    void setExplicitPath(const QString& path);

    /**
     * @brief The per-user install directory the search scans, shown in
     *        the Settings install hint.
     * @returns Absolute directory path (may not exist yet).
     */
    QString wellKnownDir() const;

    /**
     * @brief Whether the daemon is running and has greeted.
     * @returns True between a version-accepted greet and a stop.
     */
    bool running() const { return m_running; }

    /**
     * @brief The daemon's self-reported pack version.
     * @returns Version string from the last greet, or empty.
     */
    QString packVersion() const { return m_packVersion; }

    /**
     * @brief TTS voices the daemon announced.
     * @returns Voice ids from the last greet; empty before it.
     */
    QStringList voices() const { return m_voices; }

    /**
     * @brief The last failure the user should see.
     * @returns Reason string, or empty when healthy.
     */
    QString lastError() const { return m_lastError; }

    /**
     * @brief Whether repeated start failures latched the service.
     * @returns True when enable() refuses until the next Detect.
     */
    bool latched() const { return m_latched; }

    /**
     * @brief Whether the daemon starts with the application.
     * @returns The persisted autostart flag.
     */
    bool autostart() const { return m_autostart; }

    /**
     * @brief Persists the autostart flag. Takes effect at the next
     *        application start; it does not start or stop the daemon now.
     * @param on True to spawn the daemon on application start.
     */
    void setAutostart(bool on);

    /**
     * @brief The voice used by agents with no voice of their own.
     * @returns The persisted default voice id; empty means the daemon
     *          picks (its first installed voice).
     */
    QString defaultVoice() const { return m_defaultVoice; }

    /**
     * @brief Persists the default voice. Applies to a call in progress,
     *        because the voice id travels with each speak request.
     * @param voiceId Daemon voice id, or empty to let the daemon pick.
     */
    void setDefaultVoice(const QString& voiceId);

    /**
     * @brief The push-to-talk hotkey as a portable key-sequence string.
     * @returns The persisted sequence, or empty when unbound.
     */
    QString pttHotkey() const { return m_pttHotkey; }

    /**
     * @brief Persists the hold-to-talk key and applies it to a call in
     *        progress. The key works exactly like holding the on-screen
     *        button: press opens the microphone, release sends.
     * @param sequence Portable key-sequence string; empty unbinds.
     */
    void setPttHotkey(const QString& sequence);

    /**
     * @brief Formats a QML key event into a portable sequence string.
     *        Bare modifiers (Ctrl alone, Shift alone) yield empty, so a
     *        capture field cannot bind half a combination.
     * @param key       The Qt::Key from the KeyEvent.
     * @param modifiers The Qt::KeyboardModifiers from the KeyEvent.
     * @returns "F9"-style portable text, or empty when not bindable.
     */
    Q_INVOKABLE static QString hotkeyFromEvent(int key, int modifiers);

    /**
     * @brief Application-wide key filter implementing the hold-to-talk
     *        hotkey. Installed ONLY while a call is live and a hotkey is
     *        set; removed the moment either stops being true, so outside
     *        a call the application's event flow is untouched. Keys are
     *        swallowed only on an exact sequence match, and never while
     *        a text editor has focus.
     * @param watched The object the event was addressed to.
     * @param event   The event under inspection.
     * @returns True when the key was consumed as push-to-talk.
     */
    bool eventFilter(QObject* watched, QEvent* event) override;

    /**
     * @brief The daemon's models directory.
     * @returns The absolute path the daemon reported in its greet, or
     *          empty when it has not greeted (or is too old to say).
     */
    QString modelsDir() const { return m_modelsDir; }

    /**
     * @brief Restarts the daemon so a freshly installed voice or speech
     *        model appears in its lists. A stopped service simply
     *        starts; a running one gets a clean stop followed by a
     *        start driven off the stopped signal, never a timer. A live
     *        call refuses, because models cannot change mid-call.
     */
    Q_INVOKABLE void restartService();

    /**
     * @brief Re-runs detection now (the Settings "Detect" button). Also
     *        clears the failure latch: a user action is a fresh chance.
     *        Emits detectionChanged() only when the outcome changed.
     */
    Q_INVOKABLE void detect();

    /**
     * @brief Starts the daemon at the detected path. Refused (with
     *        lastError set) when not detected or latched; a no-op when
     *        already running. The outcome arrives via serviceStateChanged.
     */
    Q_INVOKABLE void enable();

    /**
     * @brief Stops the daemon gracefully. A no-op when not running.
     */
    Q_INVOKABLE void disable();

    /**
     * @brief Pure search-order resolution, exposed for unit tests.
     * @param explicitPath When non-empty it is authoritative: returned if
     *        it names an executable file, otherwise the search FAILS.
     * @param wellKnownDirs Per-user install dirs, scanned in order.
     * @param pathDirs PATH entries, scanned in order.
     * @param appDir applicationDirPath(); scanned last; empty to skip.
     * @param patterns Accepted file names, wildcards allowed
     *        (binaryNamePatterns()).
     * @returns Absolute path of the first hit, or empty when not found.
     */
    static QString resolveBinaryPath(const QString& explicitPath,
                                     const QStringList& wellKnownDirs,
                                     const QStringList& pathDirs,
                                     const QString& appDir,
                                     const QStringList& patterns);

    /**
     * @brief The plain executable name of the daemon.
     * @returns "verzeta-voice", with ".exe" appended on Windows.
     */
    static QString binaryFileName();

    /**
     * @brief Every file name the add-on plausibly arrives as, in
     *        preference order: a plain executable first, then packaged
     *        single-file forms (an AppImage on Linux, a capitalised or
     *        versioned .exe on Windows). Wildcards are allowed.
     * @returns Platform-appropriate name patterns.
     */
    static QStringList binaryNamePatterns();

    /**
     * @brief Locations a working install can occupy WITHOUT being on
     *        PATH: the documented per-user folder, system prefixes,
     *        /opt and bundle layouts, and the folders downloads land in.
     * @returns Absolute directories, in search order.
     */
    static QStringList commonInstallDirs();

    /**
     * @brief First file in @p dir matching any of @p patterns.
     * @param dir      Directory to scan; missing directories are skipped.
     * @param patterns File names or wildcards; wildcard hits prefer the
     *                 newest file so an upgraded copy wins.
     * @returns Absolute path of the match, or empty.
     */
    static QString resolveInDirectory(const QString& dir, const QStringList& patterns);

    /**
     * @brief The transport host, for tests and diagnostics only (greet
     *        deadline override, raw event observation). Never null.
     * @returns The owned sidecar host.
     */
    VoiceSidecarHost* sidecarHostForTest() { return m_host.get(); }

    // ----- Call control (all no-ops unless the service is running) -----

    /**
     * @brief Whether a call is running.
     * @returns True between startCall() and the call ending.
     */
    bool callActive() const { return !m_callConvId.isEmpty(); }
    /**
     * @brief The conversation hosting the live call.
     * @returns Its id, or empty when idle.
     */
    QString callConversationId() const { return m_callConvId; }
    /**
     * @brief Whether the daemon confirmed the call.
     * @returns True once call.started arrived.
     */
    bool callLive() const { return m_callLive; }
    /**
     * @brief Whether the microphone is open.
     * @returns True while the talk control is held.
     */
    bool talking() const { return m_talking; }
    /**
     * @brief Whether the microphone is muted.
     * @returns True when muted for this call.
     */
    bool micMuted() const { return m_micMuted; }
    /**
     * @brief Who the next utterance is addressed to.
     * @returns The alias, or empty for the whole group.
     */
    QString targetAlias() const { return m_targetAlias; }
    /**
     * @brief Who is speaking right now.
     * @returns The alias, or empty when silent.
     */
    QString speakingAlias() const { return m_speakingAlias; }
    /**
     * @brief The live caption text.
     * @returns The partial transcript, or empty.
     */
    QString caption() const { return m_caption; }
    /**
     * @brief Current microphone level for the meter.
     * @returns Level 0..1; 0 when not capturing.
     */
    double micLevel() const { return m_micLevel; }

    /**
     * @brief Starts a call in @p conversationId. Refused (lastError set)
     *        when the service is not running or another call is live.
     * @param conversationId The conversation to talk in.
     * @param aliasVoices    Member alias to daemon voice id. An empty map
     *                       lets the daemon pick its default voice.
     * @param groupCall      True for a multi-member conversation. Only a
     *                       group call paces the cascade to speech; a
     *                       one-to-one call has no next member to order
     *                       against, so pacing it would stall the turn
     *                       for the length of the audio and buy nothing.
     */
    Q_INVOKABLE void startCall(const QString& conversationId,
                               const QVariantMap& aliasVoices = {},
                               bool groupCall = false);

    /** @brief Ends the live call. A no-op when none is active. */
    Q_INVOKABLE void endCall();

    /**
     * @brief The exact voice stored for one member of one conversation,
     *        with no fallback. This is what a picker shows: empty means
     *        "(Use default voice)" is selected.
     * @param conversationId The conversation.
     * @param alias Member alias; empty for a one-to-one conversation.
     * @returns The stored voice id, or empty when none is assigned.
     */
    Q_INVOKABLE QString assignedVoiceForMember(const QString& conversationId,
                                               const QString& alias) const;

    /**
     * @brief The voice a member actually speaks with, following the same
     *        override ladder as the member model override: the member's
     *        own assignment, then the conversation's one-to-one voice,
     *        then the default voice, then empty (daemon's pick).
     * @param conversationId The conversation.
     * @param alias Member alias; empty for a one-to-one conversation.
     * @returns The resolved voice id, or empty for the daemon default.
     */
    Q_INVOKABLE QString voiceForMember(const QString& conversationId, const QString& alias) const;

    /**
     * @brief Assigns a voice to one member of one conversation, the same
     *        scope as the member model override it sits beside in the
     *        sidebar. Persisted immediately; a call in progress hears the
     *        change on the next thing that member says, because the voice
     *        id is resolved per message.
     * @param conversationId The conversation. Ignored when empty.
     * @param alias   Member alias; empty targets the one-to-one agent.
     * @param voiceId Daemon voice id, or empty to fall back to the
     *                default voice.
     */
    Q_INVOKABLE void
    setVoiceForMember(const QString& conversationId, const QString& alias, const QString& voiceId);

    /**
     * @brief Stops the agent mid-sentence: drops all queued and playing
     *        speech and lets the cascade continue immediately.
     *
     * The chat text is untouched; only the audio stops. Safe to call when
     * nothing is speaking. This is the manual half of barge-in, for
     * "I can already tell this answer is wrong" without having to speak.
     */
    Q_INVOKABLE void stopSpeaking();

    /**
     * @brief Press or release the talk control. Pressing pauses agent
     *        speech and opens the microphone; releasing transcribes the
     *        window (or resumes the speech when nothing was said).
     * @param held True on press, false on release.
     */
    Q_INVOKABLE void setTalking(bool held);

    /**
     * @brief Mutes or unmutes the microphone for this call.
     * @param muted True to mute.
     */
    void setMicMuted(bool muted);

    /**
     * @brief Addresses the next utterance at one member, or the group.
     * @param alias Member alias; empty for the group (no mention added).
     */
    void setTargetAlias(const QString& alias);

    /**
     * @brief Speaks one assistant message. Called by the chat wiring when
     *        a reply finalizes in the call's conversation; safe to call
     *        with any text (non-speakable content produces no audio).
     * @param msgId    The message id, echoed in the daemon's events.
     * @param alias    Speaking member's alias (drives the tile ring).
     * @param markdown The message body as stored.
     */
    void speakMessage(const QString& msgId, const QString& alias, const QString& markdown);

    /**
     * @brief Whether the cascade must wait for this reply to be spoken.
     *        True only while a call is live in @p convId and the reply
     *        was persisted; the matching release is guaranteed because
     *        speakMessage() reports completion even when there is
     *        nothing speakable.
     * @param convId The conversation whose turn completed.
     * @param msgId  The persisted reply id, or empty.
     * @returns True to hold the cascade advance.
     */
    bool shouldHoldTurn(const QString& convId, const QString& msgId) override;


  signals:
    /** @brief The detected/detectedPath pair changed. */
    void detectionChanged();
    /** @brief The explicit-path override changed. */
    void explicitPathChanged();
    /** @brief running/packVersion/voices/lastError/latched changed. */
    void serviceStateChanged();
    /** @brief The autostart flag changed. */
    void autostartChanged();
    /** @brief The default voice or an alias assignment changed. */
    void voiceAssignmentsChanged();
    /** @brief The push-to-talk hotkey changed. */
    void pttHotkeyChanged();
    /** @brief callActive/callConversationId/callLive changed. */
    void callStateChanged();
    /** @brief The talk control was pressed or released. */
    void talkingChanged();
    /** @brief The microphone mute state changed. */
    void micMutedChanged();
    /** @brief The addressed alias changed. */
    void targetAliasChanged();
    /** @brief The speaking alias changed. */
    void speakingAliasChanged();
    /** @brief The live caption changed. */
    void captionChanged();
    /** @brief The microphone level changed. */
    void micLevelChanged();

    /**
     * @brief A finished utterance, already addressed if a target was
     *        selected. The chat wiring sends this as a user message.
     * @param conversationId The call's conversation.
     * @param text           The transcript, with any "@Alias " prefix.
     */
    void utteranceReady(const QString& conversationId, const QString& text);

    /**
     * @brief A spoken message finished or was cut short. Exactly one of
     *        these follows every speakMessage() that produced audio; the
     *        group pacing gate releases on it.
     * @param msgId    The message that stopped speaking.
     * @param complete True when it finished, false when cancelled.
     */
    void speechFinished(const QString& msgId, bool complete);

  private:
    /** @brief True when @p filePath names an existing executable file. */
    static bool isExecutableFile(const QString& filePath);
    /** @brief Handles the daemon's greeted frame (version policy). */
    void onGreeted(const QJsonObject& payload);
    /** @brief Handles a failed start attempt (latch counting). */
    void onStartFailed(const QString& reason);
    /** @brief Handles a post-greet stop (requested or crash). */
    void onStopped(const QString& reason);
    /** @brief Routes one daemon event to the call state. */
    void onDaemonEvent(const QString& op, const QJsonObject& payload);
    /** @brief Clears all call state and notifies (no daemon traffic). */
    void resetCallState();
    /**
     * @brief Installs or removes the application key filter so it exists
     *        exactly while (call live) AND (hotkey set). Idempotent.
     */
    void updatePttFilter();
    /**
     * @brief Stops all speech and frees whatever the cascade was waiting
     *        on. Chat text is never touched, only the audio.
     *
     * Emits speechFinished(held, false) for the reply the cascade was
     * parked on, so a cancel can never strand the turn. The daemon's own
     * tts.cancelled echo is idempotent against this.
     *
     * @returns True when a reply was being spoken or was still parked.
     */
    bool cancelAllSpeech();

    Persistence m_persist;
    std::unique_ptr<VoiceSidecarHost> m_host;

    bool m_detected = false;
    QString m_detectedPath;
    QString m_explicitPath;

    bool m_running = false;
    QString m_packVersion;
    QStringList m_voices;
    QString m_lastError;
    bool m_latched = false;
    int m_consecutiveStartFailures = 0;
    bool m_autostart = false;
    /** Voice for agents with no assignment; empty means daemon default. */
    QString m_defaultVoice;
    /** conversationId -> (alias -> voice id). Keyed per conversation and
     *  member, the same scope as the member model override. The empty
     *  alias key is the one-to-one agent's voice. */
    QHash<QString, QHash<QString, QString>> m_voiceAssignments;
    /** True between disable() and its stopped("") echo: only a stop the
     *  USER asked for may clear lastError; a policy stop (e.g. version
     *  skew) must leave its message visible. */
    bool m_userRequestedStop = false;
    /** The daemon's models directory, learned from the greet. */
    QString m_modelsDir;
    /** True between restartService() and the stopped echo that starts
     *  the fresh instance. */
    bool m_restartPending = false;

    // ----- Live call state (all empty/false when idle) -----
    QString m_callConvId;
    QString m_callId;
    bool m_callLive = false;
    bool m_talking = false;
    bool m_micMuted = false;
    QString m_targetAlias;
    /** Alias to daemon voice id for the live call. */
    QHash<QString, QString> m_aliasVoices;
    QString m_speakingAlias;
    QString m_caption;
    double m_micLevel = 0.0;
    /** Message id currently being spoken, for the speaking-alias map. */
    QHash<QString, QString> m_msgAlias;
    /** Reply the cascade is currently waiting on, or empty. */
    QString m_heldMsgId;
    /** Recently finished speech, so an already-spoken reply is never
     *  held (bounded; oldest entries drop). */
    QSet<QString> m_spokenMsgIds;
    /** True when the call's conversation has several members. Only a
     *  group call paces the cascade to speech (see startCall). */
    bool m_groupCall = false;
    /** Portable text of the optional hold-to-talk key ("" = unbound). */
    QString m_pttHotkey;
    /** Parsed form of m_pttHotkey for exact event matching. */
    QKeySequence m_pttSequence;
    /** True while this object is installed as the app event filter. */
    bool m_pttFilterInstalled = false;
};

}  // namespace Verzeta::Voice
