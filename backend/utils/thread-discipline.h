// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file thread-discipline.h
 * @brief Single-line guards that assert a service method is executing
 *        on the main (application) thread.  All services that own a
 *        DbManager connection MUST only be called from the main thread
 *        because Qt's QSqlDatabase is per-thread and accessing it
 *        off-thread silently corrupts the connection.
 * @layer Utility
 * @dependencies Qt6::Core (QCoreApplication, QThread).
 *
 * Usage: put the macro as the first statement of every public method on
 * a service that touches the database, or any method that accesses
 * Qt Model/View state owned by the main thread.
 *
 *   QString MyService::doThing() {
 *       VERZETA_ASSERT_MAIN_THREAD();
 *       // ... rest of method ...
 *   }
 *
 * The check is active in every build type: a wrong-thread call ends
 * the process with qFatal(), naming the calling function.
 */

#pragma once

#include <QThread>

#include <QCoreApplication>
#include <QDebug>
#include <QObject>

namespace verzeta::detail {

/**
 * @brief Aborts with qFatal() when called off the application thread.
 * @param where Name of the calling function, for the fatal message.
 *
 * Does nothing when no QCoreApplication exists (some tests).
 */
inline void assertMainThread(const char* where) {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app)
        return;  // tests may run without an app instance
    QThread* appThread = app->thread();
    QThread* curThread = QThread::currentThread();
    if (curThread == appThread)
        return;
    qFatal("Thread-discipline violation: %s called on thread %p "
           "(expected main thread %p). This will corrupt SQLite state.",
           where,
           static_cast<void*>(curThread),
           static_cast<void*>(appThread));
}

}  // namespace verzeta::detail

/// Aborts the process when the enclosing function runs off the main thread.
#define VERZETA_ASSERT_MAIN_THREAD()                                                               \
    do {                                                                                           \
        ::verzeta::detail::assertMainThread(Q_FUNC_INFO);                                          \
    } while (0)
