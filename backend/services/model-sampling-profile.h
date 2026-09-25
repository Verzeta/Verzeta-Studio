// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file model-sampling-profile.h
 * @brief Per-(provider, model) sampling-parameter profile catalogue.
 *
 *        Some inference-server + model combinations have known quirks
 *        that require workaround sampling parameters to produce
 *        usable output.  The clearest example: Qwen 3.5/3.6 on stock
 *        Ollama exhibits early-EOS truncation under stochastic
 *        sampling because Ollama's compiled chat-template renderer
 *        inherits the upstream Qwen template bug.  Forcing greedy
 *        decoding (`top_k = 1`, `temperature = 0`) bypasses the
 *        failure mode at the cost of deterministic responses.
 *
 *        The profile registry centralises every such workaround in
 *        one read-only catalogue, consulted at request-build time.
 *        Each profile carries:
 *          - an additive sampling override (any subset of
 *            temperature / top_k / top_p / repeat_penalty / seed),
 *          - a human-readable reason (logged once when the profile
 *            first applies in a process lifetime),
 *          - a user-facing warning string that the app surfaces to
 *            the conversation settings UI so the user knows a
 *            workaround is active.
 *
 *        Profile fields are additive: they fill in `cfg` slots the
 *        user has not explicitly set (`LlmConfig` uses `-1` as the
 *        "not set" sentinel for sampling doubles / ints).  Any field
 *        the user HAS set wins over the profile; the profile is a
 *        default, not a lock.
 *
 *        The catalogue is intentionally small (one entry as of this
 *        ship) and static.  New entries are compile-time additions
 *        in `model-sampling-profile.cpp`; the registry has no runtime
 *        mutation surface.
 *
 * @layer Service
 * @dependencies Qt6::Core
 */

#pragma once

#include <optional>
#include <QString>
#include <vector>

struct LlmConfig;

namespace Verzeta::Models {

/**
 * @brief A single sampling-parameter workaround entry.
 *
 * Fields use `std::optional` so a profile can override exactly the
 * subset of sampling parameters it cares about without disturbing
 * the rest.  Treat the struct as a value-type and copy freely; profiles
 * are tiny.
 */
struct ModelSamplingProfile {
    /**
     * @brief Human-readable explanation of why this profile exists.
     *
     * Logged once per process lifetime when the profile first applies
     * to a request.  Not surfaced to end users; engineers reading
     * logs see this.
     */
    QString reason;

    /**
     * @brief Optional `temperature` override.
     *
     * If set, applied to `LlmConfig::temperature` when the user has
     * not explicitly chosen a value (i.e. `cfg.temperature < 0`).
     */
    std::optional<double> temperature;

    /// Optional `top_k` override.  Same merge semantics as `temperature`.
    std::optional<int> topK;

    /// Optional `top_p` override.  Same merge semantics as `temperature`.
    std::optional<double> topP;

    /// Optional `repeat_penalty` override.  Same merge semantics as `temperature`.
    std::optional<double> repeatPenalty;

    /// Optional `presence_penalty` override.  Same merge semantics as `temperature`.
    std::optional<double> presencePenalty;

    /// Optional `frequency_penalty` override.  Same merge semantics as `temperature`.
    std::optional<double> frequencyPenalty;

    /// Optional fixed-seed override.  Rarely used; kept for completeness.
    std::optional<int> seed;

    /**
     * @brief User-facing warning surfaced via `samplingProfileWarning`
     *        Q_PROPERTY on `AgentSettingsController`.
     *
     * Empty string means "no banner".  When non-empty, the conversation
     * settings panel renders a banner explaining the workaround is
     * active and how to opt out per-conversation.
     */
    QString warning;
};

/**
 * @brief Read-only catalogue of known (provider, model) sampling profiles.
 *
 * Singleton accessed via `instance()`.  Construction loads the static
 * catalogue from `model-sampling-profile.cpp`; no runtime mutation.
 *
 * Lookup is a simple linear pattern match because the catalogue is small
 * (<10 entries expected for the foreseeable future).  Pattern matching
 * supports prefix wildcards on the model name (e.g. `qwen3.5:*` matches
 * `qwen3.5:9b`, `qwen3.5:14b`, etc.) since model families share quirks.
 */
class ModelSamplingProfileRegistry {
  public:
    /**
     * @brief Process-wide singleton accessor.
     * @returns Reference to the registry instance.
     */
    static const ModelSamplingProfileRegistry& instance();

    /**
     * @brief Look up the profile (if any) for the given config.
     * @param providerId  Provider id as it appears on `LlmConfig::providerId`
     *                     (e.g. `"ollama"`, `"llamacpp"`, `"openai_compat"`,
     *                     `"openai"`, `"anthropic"`, etc.).
     * @param modelName   Full model identifier as configured on
     *                     `LlmConfig::modelName` (e.g. `"qwen3.5:9b"`,
     *                     `"gemma4:e4b"`).
     * @returns Pointer to the matching profile, or `nullptr` when no
     *          profile applies (the common case for most pairs).
     *
     * The returned pointer's lifetime is tied to the singleton's
     * lifetime (process-wide), so callers may hold it across stack
     * frames without copying.
     *
     * @complexity O(N) in catalogue size, which is bounded small.
     */
    const ModelSamplingProfile* find(const QString& providerId, const QString& modelName) const;

    /**
     * @brief Count of profile entries in the catalogue.
     *
     * Exposed for tests; production callers should not need it.
     *
     * @returns Number of profiles in the static catalogue.
     */
    int catalogueSize() const;

  private:
    /**
     * @brief Internal catalogue entry.
     *
     * Pairs a provider/model match rule with the profile to apply.
     * Pattern matching:
     *   - `providerIdPattern` is matched exactly against `providerId`.
     *   - `modelNamePattern` supports a single `*` suffix wildcard so
     *     `"qwen3.5:*"` matches `qwen3.5:9b`, `qwen3.5:14b`, etc.
     *     Without a wildcard, the match is exact.
     *   - Patterns are matched case-insensitively to tolerate
     *     `Qwen3.5:9b` vs `qwen3.5:9b` variations across providers.
     */
    struct Entry {
        QString providerIdPattern;
        QString modelNamePattern;
        ModelSamplingProfile profile;
    };

    ModelSamplingProfileRegistry();

    static bool matches(const QString& pattern, const QString& value);

    std::vector<Entry> m_catalogue;
};

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

/**
 * @brief Apply the matching profile's overrides to `cfg` in-place.
 *
 * Looks up the profile via `ModelSamplingProfileRegistry::find` using
 * `cfg.providerId` + `cfg.modelName`.  When a profile is found, the
 * merge mode is selected by `cfg.forceAppSampling`:
 *
 *   - `forceAppSampling = true` (FORCE mode, default):
 *     Every field the profile carries (temperature / topK / topP /
 *     repeatPenalty / presencePenalty / frequencyPenalty) OVERRIDES
 *     the user's value unconditionally.  This is the behaviour the
 *     "Use app-recommended sampling" conversation-settings toggle
 *     enables.
 *
 *   - `forceAppSampling = false` (ADDITIVE mode):
 *     Each profile field is applied only where `cfg` has not been
 *     explicitly set (still at the `-1` sentinel).  User values
 *     always win.  This is the historical behaviour from the initial
 *     K registry ship.
 *
 * @param cfg  Configuration to mutate.  Provider id + model name
 *              must be set; the function reads those to do the lookup.
 *              `cfg.forceAppSampling` selects merge mode.
 * @returns Pointer to the profile that was applied, or `nullptr` when
 *          no profile matched the (provider, model) pair.  The returned
 *          pointer is owned by the singleton and remains valid for the
 *          process lifetime.
 */
const ModelSamplingProfile* applyProfileTo(LlmConfig& cfg);

/**
 * @brief Returns the warning string for the profile that WOULD apply
 *        to `cfg`, but only if the workaround is effective for this
 *        configuration.
 *
 * The "effective" check: if the user has explicitly set any field that
 * the profile cares about (i.e. would have overridden), the workaround
 * has been opted out for this conv, so return an empty string so the UI
 * does not display a misleading banner.
 *
 * Does NOT mutate `cfg`. Use this to drive UI surfaces (the
 * `samplingProfileWarning` Q_PROPERTY on `AgentSettingsController`).
 *
 * @param cfg  Configuration to query.  Provider id + model name read
 *              for the lookup; sampling fields read to determine whether
 *              the workaround is effective.
 * @returns Profile warning text on hit-and-effective; empty string
 *          otherwise.
 */
QString effectiveProfileWarning(const LlmConfig& cfg);

}  // namespace Verzeta::Models
