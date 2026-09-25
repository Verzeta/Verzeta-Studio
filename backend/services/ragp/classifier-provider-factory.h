// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file classifier-provider-factory.h
 * @brief Factory that constructs a dedicated, RAGP-owned ILLMProvider
 *        instance for the active main provider so the @-mention
 *        classifier can run against ANY configured provider, not
 *        just Ollama.
 * @layer Service
 * @dependencies Qt6::Core, ILLMProvider implementations, HttpClient,
 *               SettingsService.
 *
 * Why a dedicated instance instead of ModelRouter's slots:
 *   - The FOREGROUND slot streams live conversation turns. A user
 *     message arriving mid-classification would collide on the same
 *     provider instance (ILLMProvider is single-request-per-instance).
 *   - The BACKGROUND slot is single-in-flight by design
 *     (`routeBackground` tears down any prior relay) and is owned by
 *     HeartbeatSubagentService's dispatch lifecycle. Sharing it would
 * let a heartbeat fire orphan an in-flight classification (or
 *     vice versa).
 *   - A classifier-owned instance is fully isolated: its lifetime,
 *     cancellation, and signal wiring belong to the RAGP backend
 *     alone. This mirrors the established precedent of per-slot
 *     provider instances sharing NO state.
 *
 * Supported provider ids:
 *   ollama, openai, anthropic, gemini, openrouter, deepseek,
 *   llamacpp_remote, custom.\<slug\> (any user-configured
 *   OpenAI-compatible server).
 *
 * Unsupported (returns unavailable with an explicit reason):
 *   llamacpp (in-process embedded): constructing a second in-process
 *   instance would double model RAM (a 7B Q4_K_M is ~4 GiB) and
 *   double the exposure to llama.cpp's GGML_ASSERT abort()s (B-5).
 *   The local-llama RAGP backend (separate small GGUF) is the
 *   designed local path; when it isn't configured, classification
 *   for embedded-llamacpp users falls to Tier 1 rules, and that
 *   state is now VISIBLE via the unavailability reason instead of
 *   silent.
 */

#pragma once

#include <memory>
#include <QObject>
#include <QString>

class HttpClient;
class ILLMProvider;
class SettingsService;

namespace Ragp {

/**
 * @brief Bundle of a classifier-owned provider instance and the
 *        HttpClient it depends on.
 *
 * Member order matters: `http` is declared BEFORE `provider` so the
 * compiler-generated destruction order (reverse declaration) destroys
 * the provider FIRST, then the HttpClient it references. A provider
 * outliving its HttpClient would dangle (providers hold
 * `HttpClient&`).
 */
struct ClassifierProvider {
    /** Owned HTTP transport. One per classifier instance, isolated
     *  from the foreground / background HttpClient pools so a stuck
     *  classifier call can never poison conversation traffic. */
    std::unique_ptr<HttpClient> http;

    /** Owned provider instance configured for `providerId`. Null when
     *  construction was not possible. */
    std::unique_ptr<ILLMProvider> provider;

    /** The provider id this bundle was built for (e.g. "openai",
     *  "custom.lm-studio-home"). Used by the caller to detect when
     *  the active provider changed and the bundle must be rebuilt. */
    QString providerId;

    /**
     * @brief Whether the bundle is usable.
     * @returns true when `provider` is non-null and ready to dispatch.
     */
    bool valid() const { return provider != nullptr; }
};

/**
 * @brief Reports whether a classifier provider can be constructed for
 *        `providerId` with the current settings.
 * @param providerId Active main provider id.
 * @param settings   Source of API keys / base URLs / custom-server rows.
 * @param whyNot     Optional out-param receiving a human-readable
 *                   reason when the answer is false (e.g. "openai:
 *                   no API key configured"). Untouched on true.
 * @returns true iff makeClassifierProvider() would produce a valid
 *          bundle.
 */
bool classifierProviderAvailable(const QString& providerId,
                                 SettingsService& settings,
                                 QString* whyNot = nullptr);

/**
 * @brief Constructs a classifier-owned provider bundle for `providerId`.
 * @param providerId Active main provider id (see supported list above).
 * @param settings   Source of API keys / base URLs / custom-server rows.
 * @param parent     QObject parent for the HttpClient + provider
 *                   (typically the RemoteBackend). Ownership of the
 *                   QObject parts still flows through the unique_ptrs
 *                   in the returned bundle; the QObject parent is a
 *                   safety net only and is cleared by unique_ptr
 *                   destruction first.
 * @param whyNot     Optional out-param receiving the reason when the
 *                   returned bundle is invalid.
 * @returns Bundle with `valid() == true` on success; invalid bundle
 *          (null provider) with `whyNot` populated on failure.
 */
ClassifierProvider makeClassifierProvider(const QString& providerId,
                                          SettingsService& settings,
                                          QObject* parent,
                                          QString* whyNot = nullptr);

}  // namespace Ragp
