// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-protocol.h
 * @brief Canonical vocabulary for the host ↔ `verzeta-voice` daemon stdio
 *        IPC: the protocol version, the op/event names, and the shared
 *        frame codec. The Verzeta-Voice repository mirrors this header
 *        (together with infer-protocol.h, which supplies the codec) and
 *        asserts kVoiceProtocolVersion at build time, the same discipline
 *        the wire clients apply to wire-protocol.h.
 * @layer Utility (Voice IPC)
 * @dependencies Verzeta::Infer frame codec (infer-protocol.h); Qt6::Core
 *               only. No sockets, no event loop.
 *
 * ## Wire format
 * Identical to the inference sidecar's frame codec:
 *   [u32 frameLen][u32 jsonLen][json bytes][blob bytes]
 * The codec is reused via the aliases below rather than duplicated; the
 * blob is unused by v1 voice frames (JSON only) but the framing keeps the
 * two sidecar protocols byte-compatible at the transport layer.
 *
 * ## Protocol v1 vocabulary (JSON header field "op")
 * Host → daemon:
 *   greet         {proto_version}
 *   call.start    {call_id, mode:"ptt", voices:{alias→voice_id}, stt_model}
 *   call.end      {call_id}                 (idempotent)
 *   mic.set       {muted}
 *   ptt.set       {pressed}   press: pause TTS playback + start capture;
 *                             release: end capture; an empty/filtered
 *                             utterance resumes the paused playback
 *   tts.speak     {msg_id, seq, text, voice_id, final}
 *   tts.cancel    {msg_id} | {all:true}
 *   shutdown      {}
 * Daemon → host:
 *   greeted       {proto_version, pack_version, voices[], stt_models[]}
 *   call.started | call.ended | call.error   {call_id, reason?}
 *   vad.speech    {active}
 *   stt.partial   {text}
 *   stt.final     {text, dur_ms}
 *   tts.started | tts.msg_done | tts.cancelled   {msg_id}
 *   levels        {user_rms}                (daemon-throttled, ~10 Hz)
 *   model.download.progress {pct, done_mb, total_mb}
 *
 * Every host request that opens a pacing hold (a tts.speak with
 * final=true) terminates in exactly one of tts.msg_done / tts.cancelled /
 * call.ended / daemon exit. The daemon exits cleanly on stdin EOF.
 */

#pragma once

#include "../inference/infer-protocol.h"

namespace Verzeta::Voice {

/** Protocol version exchanged in greet/greeted. A daemon announcing a
 *  different version is shut down and the UI reports the mismatch; no
 *  call can start across a skew. */
inline constexpr int kVoiceProtocolVersion = 1;

/** The daemon executable's base name ("verzeta-voice"; ".exe" is appended
 *  on Windows by the detection code). */
inline constexpr char kVoiceBinaryBaseName[] = "verzeta-voice";

// Shared transport codec (see infer-protocol.h for the byte layout).
/// One decoded voice frame (the inference sidecar's frame type).
using VoiceFrame = Verzeta::Infer::InferFrame;
/// Incremental frame reader for the daemon's stdout stream.
using VoiceFrameReader = Verzeta::Infer::InferFrameReader;
using Verzeta::Infer::decodeInferPayload;
using Verzeta::Infer::encodeInferFrame;

// --- Host → daemon ops ------------------------------------------------
inline constexpr char kOpGreet[] = "greet";  ///< Handshake; carries the host's protocol version.
inline constexpr char kOpCallStart[] =
    "call.start";                                   ///< Opens a call with its voices and STT model.
inline constexpr char kOpCallEnd[] = "call.end";    ///< Ends a call; safe to repeat.
inline constexpr char kOpMicSet[] = "mic.set";      ///< Mutes or unmutes the microphone.
inline constexpr char kOpPttSet[] = "ptt.set";      ///< Push-to-talk pressed or released.
inline constexpr char kOpTtsSpeak[] = "tts.speak";  ///< One text chunk of a reply to speak.
inline constexpr char kOpTtsCancel[] = "tts.cancel";  ///< Stops one reply's speech, or all of it.
inline constexpr char kOpShutdown[] = "shutdown";     ///< Asks the daemon to exit.

// --- Daemon → host events ---------------------------------------------
inline constexpr char kEvGreeted[] =
    "greeted";  ///< Handshake reply with version, voices and STT models.
inline constexpr char kEvCallStarted[] = "call.started";  ///< The call is live.
inline constexpr char kEvCallEnded[] = "call.ended";      ///< The call has ended.
inline constexpr char kEvCallError[] = "call.error";      ///< The call failed; carries a reason.
/// Voice activity started or stopped. The host does not act on it.
inline constexpr char kEvVadSpeech[] = "vad.speech";
/// Interim transcript. The host shows it as a live caption; the
/// current daemon does not send it.
inline constexpr char kEvSttPartial[] = "stt.partial";
inline constexpr char kEvSttFinal[] = "stt.final";       ///< Final transcript of one utterance.
inline constexpr char kEvTtsStarted[] = "tts.started";   ///< Speech for a reply began playing.
inline constexpr char kEvTtsMsgDone[] = "tts.msg_done";  ///< Speech for a reply finished playing.
inline constexpr char kEvTtsCancelled[] = "tts.cancelled";  ///< Speech for a reply was cancelled.
inline constexpr char kEvLevels[] = "levels";               ///< Microphone level for the meter.
/// Model download progress. Reserved: the current daemon does not send
/// it and the host does not act on it.
inline constexpr char kEvModelProgress[] = "model.download.progress";

}  // namespace Verzeta::Voice
