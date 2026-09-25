// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"

#include <iostream>
#include <QCoreApplication>
#include <QString>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::cerr << "usage: db-lock-test-helper <db-path>" << std::endl;
        return 1;
    }

    const QString dbPath = QString::fromLocal8Bit(argv[1]);

    if (DbManager::instance().open(dbPath)) {
        DbManager::instance().close();
        return 0;
    }
    return 2;
}
