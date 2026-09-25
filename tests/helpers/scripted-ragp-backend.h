// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#pragma once

#include "services/ragp/iragp-backend.h"
#include "services/ragp/ragp-types.h"

#include <QtConcurrent>

#include <QFuture>
#include <QString>

class ScriptedRagpBackend : public Ragp::IRagpBackend {
  public:
    Ragp::Classification response;

    bool available = true;

    mutable int callCount = 0;

    QFuture<Ragp::Classification> classifyAsync(const Ragp::Request& req) override {
        Q_UNUSED(req);
        ++callCount;
        return QtFuture::makeReadyValueFuture(response);
    }

    bool isAvailable() const override { return available; }

    QString backendName() const override { return QStringLiteral("test-scripted"); }
};
