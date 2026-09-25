// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file embed-slot.cpp
 * @brief Implementation of the sidecar's embedding slot. The llama.cpp
 *        call sequence (GPU-first load with CPU fallback, embeddings
 *        context, tokenize → decode → llama_get_embeddings, dimension
 *        from llama_model_n_embd) mirrors the app's proven pre-sidecar
 *        embedding path; the parity test pins the produced vectors.
 * @layer Tool (separate-process sidecar)
 * @dependencies llama.cpp (guarded); Qt6::Core.
 */

#include "embed-slot.h"

#include "model-loader.h"

#include <QFileInfo>

#ifdef VERZETA_INFER_HAS_LLAMA
#include <llama.h>
#include <string>
#include <vector>
#endif

namespace Verzeta::Infer {

EmbedSlot::~EmbedSlot() {
    unload();
}

#ifdef VERZETA_INFER_HAS_LLAMA

bool EmbedSlot::load(const QString& path, QString* error) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("model file not found: %1").arg(path);
        return false;
    }
    // Replacing a loaded model: release first so RAM is never held
    // twice (the no-cycling contract lives at the HOST selection layer;
    // an explicit load of a different path is a user decision).
    unload();

    m_model = loadModelGpuFirst(path, &m_accel);
    if (!m_model) {
        if (error)
            *error = QStringLiteral("failed to load model");
        return false;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.embeddings = true;
    // CPU-path threads mirror the decode slots. Ignored on GPU.
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

void EmbedSlot::unload() {
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

bool EmbedSlot::isLoaded() const {
    return m_ctx != nullptr;
}

QString EmbedSlot::modelId() const {
    return m_loadedPath.isEmpty() ? QString() : QFileInfo(m_loadedPath).fileName();
}

QString EmbedSlot::accel() const {
    return m_accel;
}

int EmbedSlot::dim() const {
    if (!m_ctx)
        return 0;
    return llama_model_n_embd(llama_get_model(m_ctx));
}

bool EmbedSlot::embed(const QStringList& texts, QByteArray* outBlob, QString* error) {
    if (!m_ctx || !outBlob) {
        if (error)
            *error = QStringLiteral("slot not loaded");
        return false;
    }
    const struct llama_model* model = llama_get_model(m_ctx);
    const struct llama_vocab* vocab = llama_model_get_vocab(model);
    const int nEmbd = llama_model_n_embd(model);

    for (const QString& text : texts) {
        if (text.isEmpty()) {
            if (error)
                *error = QStringLiteral("empty text in batch");
            return false;
        }
        const std::string utf8 = text.toStdString();
        std::vector<llama_token> tokens(utf8.size() + 4);
        const int nTokens = llama_tokenize(vocab,
                                           utf8.c_str(),
                                           static_cast<int>(utf8.size()),
                                           tokens.data(),
                                           static_cast<int>(tokens.size()),
                                           /*add_special=*/true,
                                           /*parse_special=*/false);
        if (nTokens < 0) {
            if (error)
                *error = QStringLiteral("tokenization failed");
            return false;
        }
        tokens.resize(static_cast<size_t>(nTokens));

        llama_batch batch = llama_batch_get_one(tokens.data(), nTokens);
        if (llama_decode(m_ctx, batch) != 0) {
            if (error)
                *error = QStringLiteral("decode failed");
            return false;
        }
        const float* emb = llama_get_embeddings(m_ctx);
        if (!emb) {
            if (error)
                *error = QStringLiteral("no embeddings output");
            return false;
        }
        outBlob->append(reinterpret_cast<const char*>(emb),
                        static_cast<qsizetype>(nEmbd * sizeof(float)));
    }
    return true;
}

#else  // !VERZETA_INFER_HAS_LLAMA — protocol-valid degradation

bool EmbedSlot::load(const QString&, QString* error) {
    if (error)
        *error = QStringLiteral("built without llama.cpp");
    return false;
}
void EmbedSlot::unload() {}
bool EmbedSlot::isLoaded() const {
    return false;
}
QString EmbedSlot::modelId() const {
    return QString();
}
QString EmbedSlot::accel() const {
    return QString();
}
int EmbedSlot::dim() const {
    return 0;
}
bool EmbedSlot::embed(const QStringList&, QByteArray*, QString* error) {
    if (error)
        *error = QStringLiteral("built without llama.cpp");
    return false;
}

#endif  // VERZETA_INFER_HAS_LLAMA

}  // namespace Verzeta::Infer
