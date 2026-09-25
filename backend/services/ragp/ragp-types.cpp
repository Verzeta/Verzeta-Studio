// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file ragp-types.cpp
 * @brief String-conversion and routing-policy helpers for RAGP types.
 * @layer Service
 * @dependencies Qt6::Core
 */

#include "ragp-types.h"

#include <QHash>

namespace Ragp {

namespace {

// Lookup tables built lazily on first use. Populated once per process.
const QHash<Intent, QString>& intentToStringTable() {
    static const QHash<Intent, QString> t = {
        {Intent::UNKNOWN, QStringLiteral("UNKNOWN")},
        {Intent::DELEGATE_RESPONSE, QStringLiteral("DELEGATE_RESPONSE")},
        {Intent::DELEGATE_TASK, QStringLiteral("DELEGATE_TASK")},
        {Intent::BROADCAST_REQUEST, QStringLiteral("BROADCAST_REQUEST")},
        {Intent::CLARIFICATION_REQUEST, QStringLiteral("CLARIFICATION_REQUEST")},
        {Intent::CORRECTION_REQUEST, QStringLiteral("CORRECTION_REQUEST")},
        {Intent::STATUS_CHECK, QStringLiteral("STATUS_CHECK")},
        {Intent::HANDOFF_COMPLETE, QStringLiteral("HANDOFF_COMPLETE")},
        {Intent::PLURAL_ADDRESS, QStringLiteral("PLURAL_ADDRESS")},
        {Intent::ESCALATION_TO_USER, QStringLiteral("ESCALATION_TO_USER")},
        {Intent::REFERENCE, QStringLiteral("REFERENCE")},
        {Intent::ACKNOWLEDGMENT, QStringLiteral("ACKNOWLEDGMENT")},
        {Intent::SUMMARY_LIST, QStringLiteral("SUMMARY_LIST")},
        {Intent::QUESTION_ABOUT, QStringLiteral("QUESTION_ABOUT")},
        {Intent::QUOTING, QStringLiteral("QUOTING")},
        {Intent::GREETING_FAREWELL, QStringLiteral("GREETING_FAREWELL")},
    };
    return t;
}

}  // anonymous namespace

QString intentToString(Intent intent) {
    const auto& t = intentToStringTable();
    const auto it = t.constFind(intent);
    return (it != t.constEnd()) ? it.value() : QStringLiteral("UNKNOWN");
}

Intent intentFromString(const QString& s) {
    const auto& table = intentToStringTable();
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
        if (it.value().compare(s, Qt::CaseInsensitive) == 0) {
            return it.key();
        }
    }
    return Intent::UNKNOWN;
}

bool shouldCascade(Intent intent) {
    switch (intent) {
        case Intent::DELEGATE_RESPONSE:
        case Intent::DELEGATE_TASK:
        case Intent::BROADCAST_REQUEST:
        case Intent::CLARIFICATION_REQUEST:
        case Intent::CORRECTION_REQUEST:
        case Intent::STATUS_CHECK:
        case Intent::HANDOFF_COMPLETE:
        case Intent::PLURAL_ADDRESS:
            return true;

        case Intent::ESCALATION_TO_USER:
        case Intent::REFERENCE:
        case Intent::ACKNOWLEDGMENT:
        case Intent::SUMMARY_LIST:
        case Intent::QUESTION_ABOUT:
        case Intent::QUOTING:
        case Intent::GREETING_FAREWELL:
        case Intent::UNKNOWN:
            return false;
    }
    // Exhaustive switch — compiler will warn if new enum values are added
    // without handling above. Default fallback for safety:
    return false;
}

}  // namespace Ragp
