// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-slot.cpp
 * @brief Implementation of the sidecar's RAGP completion slot. The
 *        llama.cpp sequence (KV clear per call → chunked prompt decode
 *        with logits only on the final token → greedy sampler → one
 *        token per llama_decode step) intentionally MIRRORS the app's
 *        proven in-process local RAGP engine so completions match for
 *        the same GGUF; the prompt-fit guard and clamps are identical
 *        in spirit (reject prompts that cannot fit n_ctx with room to
 *        generate).
 * @layer Tool (separate-process sidecar)
 * @dependencies llama.cpp (guarded); Qt6::Core.
 */

#include "ragp-slot.h"

#include "model-loader.h"

#include <QFileInfo>

#ifdef VERZETA_INFER_HAS_LLAMA
#include <algorithm>
#include <cstdio>
#include <llama.h>
#include <string>
#include <vector>
#endif

namespace Verzeta::Infer {

RagpSlot::~RagpSlot() {
    unload();
}

#ifdef VERZETA_INFER_HAS_LLAMA

namespace {
/// Per-decode batch size; mirrors the in-process engine's value.
constexpr uint32_t kBatchTokens = 1024;
}  // namespace

bool RagpSlot::load(const QString& path, QString* error, int nCtx) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("model file not found: %1").arg(path);
        return false;
    }
    unload();  // replacing: never hold two models

    m_model = loadModelGpuFirst(path, &m_accel);
    if (!m_model) {
        if (error)
            *error = QStringLiteral("failed to load model");
        return false;
    }
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = static_cast<uint32_t>(std::clamp(nCtx, 512, 131072));
    cparams.n_batch = kBatchTokens;
    cparams.n_ubatch = kBatchTokens;
    // CPU-path decode threads: every hardware thread, as the
    // in-process engine configured. Ignored when layers are on GPU.
    cparams.n_threads = cpuThreadCount();
    cparams.n_threads_batch = cparams.n_threads;
    m_ctx = llama_init_from_model(m_model, cparams);
    if (!m_ctx) {
        llama_model_free(m_model);
        m_model = nullptr;
        if (error)
            *error = QStringLiteral("failed to create context");
        return false;
    }
    m_loadedPath = path;
    return true;
}

void RagpSlot::unload() {
    if (m_ctx) {
        llama_free(m_ctx);
        m_ctx = nullptr;
    }
    if (m_model) {
        llama_model_free(m_model);
        m_model = nullptr;
    }
    m_loadedPath.clear();
    m_accel.clear();
}

bool RagpSlot::isLoaded() const {
    return m_ctx != nullptr;
}

QString RagpSlot::modelId() const {
    return m_loadedPath.isEmpty() ? QString() : QFileInfo(m_loadedPath).fileName();
}

QString RagpSlot::accel() const {
    return m_accel;
}

namespace {

/**
 * @brief Wraps a prompt body as one user message in the model's own
 *        chat template (ChatML fallback when the GGUF carries none or
 *        the apply fails). Port of the pre-sidecar engine's
 *        wrapInChatTemplate. The wrapping moved here with the model
 *        handle when the engine moved out of the app.
 * @param model The loaded model (template source).
 * @param body  The raw prompt body built by the host.
 * @returns The fully templated prompt ready to tokenize.
 */
QString wrapInChatTemplate(const struct llama_model* model, const QString& body) {
    const QByteArray bodyUtf8 = body.toUtf8();
    const auto chatMlFallback = [&body]() {
        QString prompt;
        prompt.reserve(body.size() + 64);
        prompt += QStringLiteral("<|im_start|>user\n");
        prompt += body;
        prompt += QStringLiteral("\n<|im_end|>\n<|im_start|>assistant\n");
        return prompt;
    };

    const char* tmpl = llama_model_chat_template(model, nullptr);
    if (tmpl == nullptr) {
        return chatMlFallback();
    }

    llama_chat_message msg{};
    msg.role = "user";
    msg.content = bodyUtf8.constData();

    const int32_t needed = llama_chat_apply_template(tmpl, &msg, 1, /*add_ass=*/true, nullptr, 0);
    if (needed <= 0) {
        std::fprintf(stderr,
                     "verzeta-inference: llama_chat_apply_template "
                     "sizing returned %d — ChatML fallback\n",
                     needed);
        return chatMlFallback();
    }

    std::vector<char> buf(static_cast<size_t>(needed) + 1, '\0');
    const int32_t written = llama_chat_apply_template(
        tmpl, &msg, 1, /*add_ass=*/true, buf.data(), static_cast<int32_t>(buf.size()));
    if (written <= 0) {
        std::fprintf(stderr,
                     "verzeta-inference: llama_chat_apply_template "
                     "apply returned %d — ChatML fallback\n",
                     written);
        return chatMlFallback();
    }
    return QString::fromUtf8(buf.data(), written);
}

}  // namespace

bool RagpSlot::complete(const QString& prompt,
                        int maxTokens,
                        bool jsonStop,
                        bool chatTemplate,
                        QString* outText,
                        QString* error) {
    if (!m_ctx || !outText) {
        if (error)
            *error = QStringLiteral("slot not loaded");
        return false;
    }
    const int genCap = std::clamp(maxTokens, 1, 1024);

    const struct llama_model* model = llama_get_model(m_ctx);
    const struct llama_vocab* vocab = llama_model_get_vocab(model);

    const QString effectivePrompt = chatTemplate ? wrapInChatTemplate(model, prompt) : prompt;

    // Fresh call = fresh sequence: clear the KV cache so positions
    // restart at 0 (same per-call reset as the in-process engine).
    if (llama_memory_t mem = llama_get_memory(m_ctx)) {
        llama_memory_clear(mem, /*data=*/true);
    }

    // Tokenize.
    const std::string utf8 = effectivePrompt.toStdString();
    std::vector<llama_token> tokens(utf8.size() + 8);
    const int nTokens = llama_tokenize(vocab,
                                       utf8.c_str(),
                                       static_cast<int>(utf8.size()),
                                       tokens.data(),
                                       static_cast<int>(tokens.size()),
                                       /*add_special=*/true,
                                       /*parse_special=*/true);
    if (nTokens <= 0) {
        if (error)
            *error = QStringLiteral("tokenization failed");
        return false;
    }
    tokens.resize(static_cast<size_t>(nTokens));

    // Prompt-fit guard: prompt + generation must fit the live n_ctx.
    const uint32_t liveCtx = llama_n_ctx(m_ctx);
    if (static_cast<uint32_t>(nTokens + genCap) > liveCtx) {
        if (error)
            *error = QStringLiteral("prompt too long: %1 tokens (+%2 to generate) exceeds %3")
                         .arg(nTokens)
                         .arg(genCap)
                         .arg(liveCtx);
        return false;
    }

    // Chunked prompt decode — logits requested only for the final
    // prompt token of the final chunk.
    llama_batch batch = llama_batch_init(static_cast<int>(kBatchTokens), 0, 1);
    /** @brief Frees the prompt batch on scope exit. */
    struct BatchGuard {
        llama_batch b;
        ~BatchGuard() { llama_batch_free(b); }
    } batchGuard{batch};

    int processed = 0;
    while (processed < nTokens) {
        const int chunkSize = std::min<int>(static_cast<int>(kBatchTokens), nTokens - processed);
        const bool isLastChunk = (processed + chunkSize) >= nTokens;
        batch.n_tokens = chunkSize;
        for (int i = 0; i < chunkSize; ++i) {
            batch.token[i] = tokens[static_cast<size_t>(processed + i)];
            batch.pos[i] = processed + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = false;
        }
        if (isLastChunk)
            batch.logits[chunkSize - 1] = true;
        if (llama_decode(m_ctx, batch) != 0) {
            if (error)
                *error = QStringLiteral("prompt decode failed at offset %1").arg(processed);
            return false;
        }
        processed += chunkSize;
    }

    // Greedy sampler — deterministic completions (classification and
    // YES/NO confirms must not wobble with temperature).
    llama_sampler* rawSampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!rawSampler) {
        if (error)
            *error = QStringLiteral("sampler init failed");
        return false;
    }
    /** @brief Frees the greedy sampler chain on scope exit. */
    struct SamplerGuard {
        llama_sampler* s;
        ~SamplerGuard() {
            if (s)
                llama_sampler_free(s);
        }
    } samplerGuard{rawSampler};
    llama_sampler_chain_add(rawSampler, llama_sampler_init_greedy());

    QString generated;
    generated.reserve(512);
    int braceDepth = 0;
    bool seenOpenBrace = false;

    for (int gen = 0; gen < genCap; ++gen) {
        const llama_token newToken = llama_sampler_sample(rawSampler, m_ctx, -1);
        if (llama_vocab_is_eog(vocab, newToken))
            break;

        char piece[256] = {};
        const int nChars =
            llama_token_to_piece(vocab, newToken, piece, sizeof(piece) - 1, 0, false);
        if (nChars > 0) {
            const QString pieceStr = QString::fromUtf8(piece, nChars);
            generated.append(pieceStr);
            if (jsonStop) {
                for (QChar ch : pieceStr) {
                    if (ch == QLatin1Char('{')) {
                        ++braceDepth;
                        seenOpenBrace = true;
                    } else if (ch == QLatin1Char('}')) {
                        --braceDepth;
                    }
                }
                if (seenOpenBrace && braceDepth == 0)
                    break;
            }
        }

        // Feed the sampled token back for the next step.
        llama_batch nextBatch = llama_batch_init(1, 0, 1);
        /** @brief Frees the per-token batch on scope exit. */
        struct NextGuard {
            llama_batch b;
            ~NextGuard() { llama_batch_free(b); }
        } nextGuard{nextBatch};
        nextBatch.n_tokens = 1;
        nextBatch.token[0] = newToken;
        nextBatch.pos[0] = nTokens + gen;
        nextBatch.n_seq_id[0] = 1;
        nextBatch.seq_id[0][0] = 0;
        nextBatch.logits[0] = true;
        if (llama_decode(m_ctx, nextBatch) != 0) {
            if (error)
                *error = QStringLiteral("decode failed mid-generation");
            return false;
        }
    }

    *outText = generated;
    return true;
}

bool RagpSlot::completeStream(const QString& prompt,
                              int maxTokens,
                              double temperature,
                              const std::function<void(const QString&)>& onPiece,
                              const std::function<bool()>& shouldCancel,
                              QString* outFinish,
                              QString* error) {
    if (!m_ctx || !outFinish) {
        if (error)
            *error = QStringLiteral("slot not loaded");
        return false;
    }
    const int genCap = std::clamp(maxTokens, 1, 32768);

    const struct llama_model* model = llama_get_model(m_ctx);
    const struct llama_vocab* vocab = llama_model_get_vocab(model);

    if (llama_memory_t mem = llama_get_memory(m_ctx)) {
        llama_memory_clear(mem, /*data=*/true);
    }

    const std::string utf8 = prompt.toStdString();
    std::vector<llama_token> tokens(utf8.size() + 8);
    const int nTokens = llama_tokenize(vocab,
                                       utf8.c_str(),
                                       static_cast<int>(utf8.size()),
                                       tokens.data(),
                                       static_cast<int>(tokens.size()),
                                       /*add_special=*/true,
                                       /*parse_special=*/true);
    if (nTokens <= 0) {
        if (error)
            *error = QStringLiteral("tokenization failed");
        return false;
    }
    tokens.resize(static_cast<size_t>(nTokens));

    const uint32_t liveCtx = llama_n_ctx(m_ctx);
    if (static_cast<uint32_t>(nTokens) + 16 > liveCtx) {
        if (error)
            *error = QStringLiteral("prompt too long: %1 tokens exceeds context %2")
                         .arg(nTokens)
                         .arg(liveCtx);
        return false;
    }

    // Chunked prompt decode (same shape as complete()).
    llama_batch batch = llama_batch_init(static_cast<int>(kBatchTokens), 0, 1);
    /** @brief Frees the prompt batch on scope exit. */
    struct BatchGuard {
        llama_batch b;
        ~BatchGuard() { llama_batch_free(b); }
    } batchGuard{batch};
    int processed = 0;
    while (processed < nTokens) {
        const int chunkSize = std::min<int>(static_cast<int>(kBatchTokens), nTokens - processed);
        const bool isLastChunk = (processed + chunkSize) >= nTokens;
        batch.n_tokens = chunkSize;
        for (int i = 0; i < chunkSize; ++i) {
            batch.token[i] = tokens[static_cast<size_t>(processed + i)];
            batch.pos[i] = processed + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = false;
        }
        if (isLastChunk)
            batch.logits[chunkSize - 1] = true;
        if (llama_decode(m_ctx, batch) != 0) {
            if (error)
                *error = QStringLiteral("prompt decode failed at offset %1").arg(processed);
            return false;
        }
        processed += chunkSize;
    }

    // Sampler: temperature+dist for chat (the old in-process chat
    // provider's exact chain); greedy when temperature <= 0.
    llama_sampler* rawSampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!rawSampler) {
        if (error)
            *error = QStringLiteral("sampler init failed");
        return false;
    }
    /** @brief Frees the sampler chain on scope exit. */
    struct SamplerGuard {
        llama_sampler* s;
        ~SamplerGuard() {
            if (s)
                llama_sampler_free(s);
        }
    } samplerGuard{rawSampler};
    if (temperature > 0.0) {
        llama_sampler_chain_add(rawSampler,
                                llama_sampler_init_temp(static_cast<float>(temperature)));
        llama_sampler_chain_add(rawSampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    } else {
        llama_sampler_chain_add(rawSampler, llama_sampler_init_greedy());
    }

    int generated = 0;
    QString finish = QStringLiteral("length");
    for (; generated < genCap; ++generated) {
        if (shouldCancel && shouldCancel()) {
            finish = QStringLiteral("cancelled");
            break;
        }
        const llama_token newToken = llama_sampler_sample(rawSampler, m_ctx, -1);
        if (llama_vocab_is_eog(vocab, newToken)) {
            finish = QStringLiteral("stop");
            break;
        }
        char piece[256] = {};
        const int nChars =
            llama_token_to_piece(vocab, newToken, piece, sizeof(piece) - 1, 0, false);
        if (nChars > 0 && onPiece) {
            onPiece(QString::fromUtf8(piece, nChars));
        }
        llama_batch nextBatch = llama_batch_init(1, 0, 1);
        /** @brief Frees the per-token batch on scope exit. */
        struct NextGuard {
            llama_batch b;
            ~NextGuard() { llama_batch_free(b); }
        } nextGuard{nextBatch};
        nextBatch.n_tokens = 1;
        nextBatch.token[0] = newToken;
        nextBatch.pos[0] = nTokens + generated;
        nextBatch.n_seq_id[0] = 1;
        nextBatch.seq_id[0][0] = 0;
        nextBatch.logits[0] = true;
        if (llama_decode(m_ctx, nextBatch) != 0) {
            if (error)
                *error = QStringLiteral("decode failed mid-generation");
            return false;
        }
    }
    *outFinish = finish;
    return true;
}

#else  // !VERZETA_INFER_HAS_LLAMA — protocol-valid degradation

bool RagpSlot::load(const QString&, QString* error, int) {
    if (error)
        *error = QStringLiteral("built without llama.cpp");
    return false;
}
void RagpSlot::unload() {}
bool RagpSlot::isLoaded() const {
    return false;
}
QString RagpSlot::modelId() const {
    return QString();
}
QString RagpSlot::accel() const {
    return QString();
}
bool RagpSlot::complete(const QString&, int, bool, bool, QString*, QString* error) {
    if (error)
        *error = QStringLiteral("built without llama.cpp");
    return false;
}
bool RagpSlot::completeStream(const QString&,
                              int,
                              double,
                              const std::function<void(const QString&)>&,
                              const std::function<bool()>&,
                              QString*,
                              QString* error) {
    if (error)
        *error = QStringLiteral("built without llama.cpp");
    return false;
}

#endif  // VERZETA_INFER_HAS_LLAMA

}  // namespace Verzeta::Infer
