// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-model-catalog.cpp
 * @brief The pinned entries. Sizes and SHA-256 values are copied from
 *        the Hugging Face LFS metadata (large files) or computed from
 *        the fetched content (voice configs) at pin time.
 * @layer Utility (Voice)
 * @dependencies Qt6::Core only.
 */

#include "voice-model-catalog.h"

namespace Verzeta::Voice {

namespace {

/** Piper voice repository base (each voice is model + config). */
const char kPiperBase[] = "https://huggingface.co/rhasspy/piper-voices/resolve/main/";

/** whisper.cpp model repository base. */
const char kWhisperBase[] = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

/** @brief Builds one Piper voice entry (an .onnx and its .json). */
VoiceModelEntry piperVoice(const char* id,
                           const char* path,
                           const char* display,
                           const char* license,
                           qint64 onnxBytes,
                           const char* onnxSha,
                           qint64 jsonBytes,
                           const char* jsonSha) {
    VoiceModelEntry e;
    e.id = QLatin1String(id);
    e.kind = QStringLiteral("voice");
    e.displayName = QLatin1String(display);
    e.license = QLatin1String(license);
    const QString base =
        QLatin1String(kPiperBase) + QLatin1String(path) + QLatin1Char('/') + QLatin1String(id);
    e.files.append({QLatin1String(id) + QLatin1String(".onnx"),
                    base + QLatin1String(".onnx"),
                    onnxBytes,
                    QLatin1String(onnxSha)});
    e.files.append({QLatin1String(id) + QLatin1String(".onnx.json"),
                    base + QLatin1String(".onnx.json"),
                    jsonBytes,
                    QLatin1String(jsonSha)});
    return e;
}

/** @brief Builds one whisper speech-model entry (a single .bin). */
VoiceModelEntry
whisperModel(const char* fileBase, const char* display, qint64 bytes, const char* sha) {
    VoiceModelEntry e;
    e.id = QLatin1String(fileBase);
    e.kind = QStringLiteral("stt");
    e.displayName = QLatin1String(display);
    // OpenAI released the whisper weights under MIT; whisper.cpp
    // redistributes the converted files under the same terms.
    e.license = QStringLiteral("MIT");
    e.files.append({QLatin1String(fileBase) + QLatin1String(".bin"),
                    QLatin1String(kWhisperBase) + QLatin1String(fileBase) + QLatin1String(".bin"),
                    bytes,
                    QLatin1String(sha)});
    return e;
}

}  // namespace

qint64 VoiceModelEntry::totalBytes() const {
    qint64 total = 0;
    for (const VoiceModelFile& f : files)
        total += f.sizeBytes;
    return total;
}

QList<VoiceModelEntry> voiceModelCatalog() {
    // Voices: licence-audited one by one from the per-voice MODEL_CARD.
    // Included: CC0, public-domain (LibriVox) and CC-BY datasets.
    // Excluded and why, so nobody re-adds them without checking:
    //   en_US-ryan (CC BY-NC-SA), en_US-hfc_* (CC BY-NC-SA) — the
    //   non-commercial clause; en_US-amy, en_GB-alan — the card only
    //   says "See URL" and the dataset licence could not be pinned.
    return {
        piperVoice("en_US-joe-medium",
                   "en/en_US/joe/medium",
                   "Joe (US, male)",
                   "CC0",
                   63201294,
                   "58afce0321b8d9c46d7cdf9c16500cc55a793b4220212dba"
                   "6b70fb788b3baf06",
                   4794,
                   "3d6d5410b3795cb1950595247ef8f06190719e6fdbfa3a23"
                   "56d8ec368e1aad33"),
        piperVoice("en_US-norman-medium",
                   "en/en_US/norman/medium",
                   "Norman (US, male)",
                   "Public domain (LibriVox)",
                   63531379,
                   "b9739443232a80a59c7d18810dd856899bf16a7964725f5a"
                   "b81ea49b1351cb71",
                   4968,
                   "6c2db7f558a4a8deb9fe822583c1c5105f6c4e834dd0f9de"
                   "8ad17a888ee9fe1d"),
        piperVoice("en_US-kristin-medium",
                   "en/en_US/kristin/medium",
                   "Kristin (US, female)",
                   "Public domain (LibriVox)",
                   63531379,
                   "5849957f929cbf720c258f8458692d6103fff2f0e3d3b19c"
                   "8259474bb06a18d4",
                   4968,
                   "5681426d4aead22195de70531eeeeddb46493cfaffc5764b"
                   "2ea3db73428b651c"),
        piperVoice("en_GB-alba-medium",
                   "en/en_GB/alba/medium",
                   "Alba (UK, female)",
                   "CC BY 4.0",
                   63201294,
                   "401369c4a81d09fdd86c32c5c864440811dbdcc66466cde2"
                   "d64f7133a66ad03b",
                   4888,
                   "aa965a2f02ecced632c2694e1fc72bbff6d65f265fab567c"
                   "a945918c73dd89f4"),
        piperVoice("en_US-john-medium",
                   "en/en_US/john/medium",
                   "John (US, male)",
                   "Public domain (LibriVox)",
                   63531379,
                   "789c6c875726e627ddee93d51d8727859abe9c091c3d1415"
                   "91f4b83c2072e988",
                   4965,
                   "af60f177b6b550f3d7a302720c0fb89e7f94a82b5dca4647"
                   "75ef63b1c69ba09a"),
        piperVoice("en_GB-cori-high",
                   "en/en_GB/cori/high",
                   "Cori (UK, female)",
                   "Public domain (LibriVox)",
                   114219352,
                   "470b4dd634c98f8a4850d7626ffc3dfc90774628eeef6605"
                   "a6dd8f88f30a5903",
                   4963,
                   "9e7fb5b5671612c22f3c81cbe46c1ae87b031a4632bcb509"
                   "e499dad6f1e2adec"),
        whisperModel("ggml-base.en-q5_1",
                     "English recognition (base)",
                     59721011,
                     "4baf70dd0d7c4247ba2b81fafd9c01005ac77c2f9ef064e0"
                     "0dcf195d0e2fdd2f"),
        whisperModel("ggml-small.en-q5_1",
                     "English recognition (small, better)",
                     190098681,
                     "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4"
                     "844a1bf8e478ad30"),
        whisperModel("ggml-medium.en-q5_0",
                     "English recognition (medium, great)",
                     539225533,
                     "76733e26ad8fe1c7a5bf7531a9d41917b2adc0f20f2e4f55"
                     "31688a8c6cd88eb0"),
        whisperModel("ggml-large-v3-turbo-q5_0",
                     "Recognition (large turbo, best, all languages)",
                     574041195,
                     "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188"
                     "294c6d5a55ffa7e2"),
    };
}

}  // namespace Verzeta::Voice
