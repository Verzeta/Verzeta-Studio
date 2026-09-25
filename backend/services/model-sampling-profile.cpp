// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file model-sampling-profile.cpp
 * @brief Static catalogue of per-(provider, model) sampling profiles.
 * @layer Service
 * @dependencies Qt6::Core, model-sampling-profile.h
 */

#include "model-sampling-profile.h"

#include "../models/llm-config.h"

namespace Verzeta::Models {

ModelSamplingProfileRegistry::ModelSamplingProfileRegistry() {
    // -----------------------------------------------------------------
    // Catalogue: one entry per known (provider, model_family) quirk.
    //
    // To add a new entry, append a fully-initialised Entry to the
    // vector below.  Document the reason field with enough context
    // that an engineer reading the log line can act on it without
    // hunting source history.
    // -----------------------------------------------------------------

    // Qwen 3.5 / 3.6 on stock Ollama.
    //
    // The model's bundled modelfile defaults are tuned for short single-
    // turn prompts and produce early-EOS truncation in long group-chat
    // contexts.  Specifically `presence_penalty=1.5` and `repeat_penalty
    // =1.1` combined with loose default sampling (`temperature=1`,
    // `top_k=20`, `top_p=0.95`) bias the model toward emitting end-of-
    // sequence after ~50-100 output tokens once the conversation
    // accumulates ~15k+ input tokens.
    //
    // Verified empirically against captured broken request bodies:
    // an initial 20-run characterisation with `repeat_penalty=1.0`
    // produced 20/20 complete responses (no truncation) but allowed
    // within-response paragraph duplication in longer outputs because
    // there was zero anti-repetition pressure.  A follow-up granular
    // sweep showed `repeat_penalty=1.03` is the highest value that
    // never triggered early-EOS truncation in 5 runs while raising
    // the tool-call rate from 20% to 60% on a known-looping body
    // ("I will do X" narration replaced by actual tool dispatch).
    // The current catalogue ships:
    //   temperature=0.6, top_k=10, top_p=0.5,
    //   presence_penalty=0, frequency_penalty=0,
    //   repeat_penalty=1.03 (mild — under the 1.04+ truncation threshold).
    // Each profile field individually read+tuned; no char-count heuristic.
    {
        Entry e;
        e.providerIdPattern = QStringLiteral("ollama");
        e.modelNamePattern = QStringLiteral("qwen3.5:*");
        e.profile.reason =
            QStringLiteral("Stock Ollama qwen3.5 modelfile defaults (presence_penalty=1.5, "
                           "repeat_penalty=1.1, loose sampling) cause early-EOS truncation "
                           "on long group-chat contexts. Applying tighter sampling + zeroed "
                           "penalties produced 20/20 complete responses in characterisation. "
                           "See Documentation/User/10-troubleshooting.md section 5.");
        e.profile.temperature = 0.6;
        e.profile.topK = 10;
        e.profile.topP = 0.5;
        e.profile.repeatPenalty = 1.03;
        e.profile.presencePenalty = 0.0;
        e.profile.frequencyPenalty = 0.0;
        e.profile.warning = QStringLiteral("App-recommended sampling for qwen 3.5 on Ollama.");
        m_catalogue.push_back(e);
    }

    {
        Entry e;
        e.providerIdPattern = QStringLiteral("ollama");
        e.modelNamePattern = QStringLiteral("qwen3.6:*");
        e.profile.reason =
            QStringLiteral("Stock Ollama qwen3.6 inherits the same modelfile penalty / "
                           "sampling combination as qwen3.5; same workaround applies. "
                           "See Documentation/User/10-troubleshooting.md section 5.");
        e.profile.temperature = 0.6;
        e.profile.topK = 10;
        e.profile.topP = 0.5;
        e.profile.repeatPenalty = 1.03;
        e.profile.presencePenalty = 0.0;
        e.profile.frequencyPenalty = 0.0;
        e.profile.warning = QStringLiteral("App-recommended sampling for qwen 3.6 on Ollama.");
        m_catalogue.push_back(e);
    }

    // -----------------------------------------------------------------
    // Future entries go here.  Examples that may belong eventually:
    //   - DeepSeek-R1 quirks that require a specific repeat_penalty.
    //   - Gemini 2.5 Pro thinking-mode budget defaults if the upstream
    //     ones change.
    //   - Mistral instruct variants with known sampler sensitivities.
    // Each entry should cite its source (an issue link or measurement
    // log) in the `reason` field so the workaround can be re-evaluated
    // when the upstream fix lands.
    // -----------------------------------------------------------------
}

const ModelSamplingProfileRegistry& ModelSamplingProfileRegistry::instance() {
    // Meyers singleton — thread-safe construction in C++11+.  The
    // registry is read-only after construction so concurrent reads are
    // safe without any explicit synchronisation.
    static const ModelSamplingProfileRegistry s_instance;
    return s_instance;
}

const ModelSamplingProfile* ModelSamplingProfileRegistry::find(const QString& providerId,
                                                               const QString& modelName) const {
    for (const Entry& e : m_catalogue) {
        if (matches(e.providerIdPattern, providerId) && matches(e.modelNamePattern, modelName)) {
            return &e.profile;
        }
    }
    return nullptr;
}

int ModelSamplingProfileRegistry::catalogueSize() const {
    return static_cast<int>(m_catalogue.size());
}

bool ModelSamplingProfileRegistry::matches(const QString& pattern, const QString& value) {
    // Single-suffix-wildcard match.  We deliberately keep this simpler
    // than QRegularExpression: the only wildcard form we accept is a
    // trailing `*`, which matches any suffix.  This is sufficient for
    // model-family matching (`qwen3.5:*`) and avoids the cost of
    // compiling a regex per lookup.
    if (pattern.endsWith(QLatin1Char('*'))) {
        const QString prefix = pattern.left(pattern.size() - 1);
        return value.startsWith(prefix, Qt::CaseInsensitive);
    }
    return pattern.compare(value, Qt::CaseInsensitive) == 0;
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

const ModelSamplingProfile* applyProfileTo(LlmConfig& cfg) {
    const ModelSamplingProfile* p =
        ModelSamplingProfileRegistry::instance().find(cfg.providerId, cfg.modelName);
    if (!p) {
        return nullptr;
    }
    if (cfg.forceAppSampling) {
        // FORCE mode: every profile field unconditionally overrides the
        // user's value.  Active when the conversation-settings "Use
        // app-recommended sampling" toggle is ON (the default).
        if (p->temperature.has_value()) {
            cfg.temperature = p->temperature.value();
        }
        if (p->topK.has_value()) {
            cfg.topK = static_cast<double>(p->topK.value());
        }
        if (p->topP.has_value()) {
            cfg.topP = p->topP.value();
        }
        if (p->repeatPenalty.has_value()) {
            cfg.repeatPenalty = p->repeatPenalty.value();
        }
        if (p->presencePenalty.has_value()) {
            cfg.presencePenalty = p->presencePenalty.value();
        }
        if (p->frequencyPenalty.has_value()) {
            cfg.frequencyPenalty = p->frequencyPenalty.value();
        }
    } else {
        // ADDITIVE mode: profile fills slots the user has not explicitly
        // set.  `-1` is the "not set" sentinel matching the provider-
        // side `>= 0` guards in `buildRequestBody`.  Active when the
        // user has opted out of the recommended sampling via the
        // conversation-settings toggle.
        if (cfg.temperature < 0 && p->temperature.has_value()) {
            cfg.temperature = p->temperature.value();
        }
        if (cfg.topK < 0 && p->topK.has_value()) {
            cfg.topK = static_cast<double>(p->topK.value());
        }
        if (cfg.topP < 0 && p->topP.has_value()) {
            cfg.topP = p->topP.value();
        }
        if (cfg.repeatPenalty < 0 && p->repeatPenalty.has_value()) {
            cfg.repeatPenalty = p->repeatPenalty.value();
        }
        if (cfg.presencePenalty < 0 && p->presencePenalty.has_value()) {
            cfg.presencePenalty = p->presencePenalty.value();
        }
        if (cfg.frequencyPenalty < 0 && p->frequencyPenalty.has_value()) {
            cfg.frequencyPenalty = p->frequencyPenalty.value();
        }
    }
    return p;
}

QString effectiveProfileWarning(const LlmConfig& cfg) {
    // The "warning" string is now used by the UI as the LABEL beside the
    // "Use app-recommended sampling" toggle — it surfaces only when a
    // profile actually exists for this (provider, model) pair.  The
    // toggle's checked state is driven by `cfg.forceAppSampling`
    // separately, so we no longer suppress the label on a per-field
    // opt-out — opt-out is now a single discrete flag.
    const ModelSamplingProfile* p =
        ModelSamplingProfileRegistry::instance().find(cfg.providerId, cfg.modelName);
    if (!p) {
        return QString();
    }
    return p->warning;
}

}  // namespace Verzeta::Models
