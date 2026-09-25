// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-console-model.h
 * @brief Push-based list model for the canvas execution console.
 *
 *        Per the project's "minimal surface" mandate the model:
 *          - Caps total rows at 50 (FIFO trim, newest rows visible).
 *          - Truncates each line at 500 chars with a "…" trailer to
 *            prevent giant single-line dumps.
 *          - Receives ONE row per stdout line (the runner splits chunks).
 *          - Buffers stderr silently inside the runner; on a non-zero
 *            exit a single "last stderr line" row is appended via
 *            `onStderrChunk`. On exit == 0 stderr never reaches the
 *            console.
 *          - Always finishes with one summary row (success or fail).
 *
 *        Pure C++ ownership of state. QML reads roles, never holds a
 *        JS shadow. Q_INVOKABLE `clear()` is the only mutation QML can
 *        call.
 * @layer Service (Model)
 * @dependencies Qt6::Core only.
 */


#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief QAbstractListModel exposing canvas-run console rows. Push-driven
 *        from CanvasRunner signals; QML and wire clients consume it
 *        through roles or the `lineAppended` signal.
 */
class CanvasConsoleModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum Roles {
        KindRole = Qt::UserRole +
                   1,  ///< "stdout" / "stderr" / "system" / "input" / "exit-ok" / "exit-err"
        TextRole,      ///< Already-truncated display text.
        TimestampRole  ///< HH:mm:ss for the row.
    };
    Q_ENUM(Roles)

    /**
     * @brief Constructs an empty console model.
     * @param parent  Optional Qt parent.
     */
    explicit CanvasConsoleModel(QObject* parent = nullptr);

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Role from the Roles enum.
     * @returns QVariant with the role value.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping Roles values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @brief Returns the current row count.
     * @returns Row count.
     */
    int count() const;

    /**
     * @brief Drops every row. Bound to the Clear button on the console
     *        panel.
     */
    Q_INVOKABLE void clear();

    /** @brief Maximum rows kept. Older rows are FIFO-trimmed when the
     *         cap is hit. Exposed for tests. */
    static constexpr int kRowCap = 50;

    /** @brief Per-line truncation cap, applied here as a defence-in-depth
     *         when callers don't pre-trim. The runner already truncates
     *         at the same value. */
    static constexpr int kMaxLineChars = 500;

  public slots:
    /**
     * @brief Appends a "system" row announcing a run has started.
     * @param filename  Canvas filename being executed.
     */
    void onRunStarted(const QString& filename);

    /**
     * @brief Appends one stdout row from the runner.
     * @param line  Already-line-split text. Truncated to kMaxLineChars
     *              on append.
     */
    void onStdoutChunk(const QString& line);

    /**
     * @brief Appends one stderr row from the runner.
     * @param line  Stderr text (only forwarded by the runner on non-zero exit).
     */
    void onStderrChunk(const QString& line);

    /**
     * @brief Appends the terminal "exit-ok" / "exit-err" summary row.
     * @param exitCode   Process exit code.
     * @param elapsedMs  Wall-clock time the run took.
     */
    void onRunFinished(int exitCode, qint64 elapsedMs);

    /**
     * @brief Appends a "system" row describing why the runner could not start.
     * @param reason  Human-readable reason text.
     */
    void onRunFailedToStart(const QString& reason);

    /**
     * @brief Echoes a line the user typed into the running script's stdin, so
     *        the console reads as a prompt/answer transcript.
     * @param text  The text the user sent (without a trailing newline).
     */
    void onInputEcho(const QString& text);

  signals:
    /** @brief Emitted whenever the row count changes. */
    void countChanged();

    /**
     * @brief Fired for every appended row.
     * @param kind  One of "system", "stdout", "stderr", "exit-ok",
     *              "exit-err".
     * @param text  Already-truncated display text (≤ kMaxLineChars).
     *
     * Wire-host-bridge subscribes to this signal so console output can
     * reach paired remote clients (Android, etc.) over the WS protocol
     * in addition to the local Qt ListView.
     */
    void lineAppended(const QString& kind, const QString& text);

  private:
    /**
     * @brief One console row. Stored verbatim; display truncation
     *        happens before insertion.
     */
    struct Row {
        QString kind;       ///< role string for KindRole
        QString text;       ///< already-truncated display text
        QString timestamp;  ///< HH:mm:ss
    };
    QList<Row> m_rows;

    void appendRow(const QString& kind, const QString& text);
    static QString trimLine(const QString& s);
    static QString nowStamp();
};
