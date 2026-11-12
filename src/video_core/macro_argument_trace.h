// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include "video_core/macro_argument_lifetime.h"

namespace Tegra::MacroArgumentTrace {
using Word = std::uint32_t;
using Wide = std::uint64_t;
constexpr std::size_t MaxWords = 32;
constexpr std::size_t MaxSegments = 8;
inline bool Enabled() {
    static const bool enabled = [] {
        const char *trace = std::getenv("EDEN_NX2_MME_ARGUMENT_TRACE");
        const char *capture = std::getenv("EDEN_NX2_MME_FAILURE_CAPTURE");
        return trace && trace[0] == '1' && capture && *capture;
    }();
    return enabled;
}
inline bool Selected(Word entry) { return entry == 0x1d2 || entry == 0x1e7; }
enum class Source : Word {
    Unknown,
    SubmittedSnapshot,
    SafeRead,
    GpuReadback,
    UnsafeRead,
    Prefetched
};
struct Origin {
    Source source{};
    Wide channel{}, submission{}, snapshot_revision{}, snapshot_suspensions{},
        current_suspensions{};
    Wide entry_index{}, gp_header{}, entry_gpuva{}, entry_words{}, resume_word{};
    bool gpuva_known{}, gpu_written{}, safe_read{}, snapshot_available{};
    Wide lifetime_snapshot_id{};
};
struct Packet {
    Origin origin{};
    Wide header_word{};
    Word raw{};
    bool known{}, immediate{};
};
// Observed before SafeRead performs its existing flush. These are not executed bytes.
struct PreSafeReadObservation {
    bool available{}, device_known{};
    Wide entry_gpuva{}, entry_device{};
    Word count{};
    std::array<Word, MaxWords> words{};
};
struct Parse {
    Origin origin{};
    std::span<const Word> retained_snapshot{};
    PreSafeReadObservation pre_safe_read{};
};
inline thread_local const Parse *current_parse{};
class ParseScope {
  public:
    explicit ParseScope(Parse value) : context{value} {
        if (Enabled()) {
            previous = current_parse;
            current_parse = &context;
            installed = true;
        }
    }
    ~ParseScope() {
        if (installed)
            current_parse = previous;
    }
    ParseScope(const ParseScope &) = delete;

  private:
    Parse context;
    const Parse *previous{};
    bool installed{};
};
inline void NotePacket(Packet &packet, Word raw, Wide word_offset, bool immediate) {
    if (!Enabled())
        return;
    packet = {};
    if (current_parse)
        packet = {current_parse->origin, word_offset, raw, true, immediate};
}
struct Dispatch {
    const void *engine{};
    Origin origin{};
    Packet packet{};
    Wide payload_word{};
    Word method{}, pending{}, original_count{}, parser_count{}, snapshot_count{};
    bool non_incrementing{}, increment_once{}, last{}, context_known{}, payload_gpuva_known{};
    Wide payload_gpuva{}, payload_device{};
    bool payload_device_known{};
    std::array<Word, MaxWords> parser_words{}, snapshot_words{};
    PreSafeReadObservation pre_safe_read{};
    std::shared_ptr<const MacroArgumentLifetime::Observation> lifetime;
};
inline thread_local const Dispatch *current_dispatch{};
class DispatchScope {
  public:
    DispatchScope(const void *engine, const Packet &packet, Wide byte_offset, Word method,
                  Word pending, bool non_incrementing, bool increment_once, bool last,
                  std::span<const Word> words, bool unsplit) {
        if (!Enabled())
            return;
        previous = current_dispatch;
        current_dispatch = nullptr;
        installed = true;
        if (!unsplit || method < 0xe00)
            return;
        try {
            context = std::make_unique<Dispatch>();
            auto &d = *context;
            d.engine = engine;
            d.packet = packet;
            d.method = method;
            d.pending = pending;
            d.non_incrementing = non_incrementing;
            d.increment_once = increment_once;
            d.last = last;
            d.original_count = static_cast<Word>(words.size());
            d.parser_count = static_cast<Word>(std::min(words.size(), MaxWords));
            std::copy_n(words.begin(), d.parser_count, d.parser_words.begin());
            if (current_parse) {
                d.context_known = true;
                d.origin = current_parse->origin;
                d.pre_safe_read = current_parse->pre_safe_read;
                d.payload_word = byte_offset / sizeof(Word);
                d.payload_gpuva_known = d.origin.gpuva_known && !packet.immediate &&
                                        d.payload_word <= d.origin.entry_words &&
                                        words.size() <= d.origin.entry_words - d.payload_word;
                if (d.payload_gpuva_known)
                    d.payload_gpuva = d.origin.entry_gpuva + byte_offset;
                const auto snapshot = current_parse->retained_snapshot;
                if (!packet.immediate && d.payload_word <= snapshot.size() &&
                    words.size() <= snapshot.size() - d.payload_word) {
                    d.snapshot_count = d.parser_count;
                    std::copy_n(snapshot.begin() + d.payload_word, d.snapshot_count,
                                d.snapshot_words.begin());
                }
            }
            current_dispatch = context.get();
        } catch (...) {
            current_dispatch = nullptr;
        }
    }
    std::optional<Wide> PayloadGpuva() const {
        if (context && context->payload_gpuva_known)
            return context->payload_gpuva;
        return {};
    }
    void SetPayloadDevice(std::optional<Wide> device) {
        if (context && device) {
            context->payload_device = *device;
            context->payload_device_known = true;
        }
        if (MacroArgumentLifetime::Enabled() && context) {
            try {
                // Capture the entire short entry at its independently mapped base. A segment
                // offset cannot establish a contiguous base mapping; leave that case unknown.
                const auto base_device = context->payload_word == 0 ? device : std::nullopt;
                context->lifetime = MacroArgumentLifetime::Capture(
                    context->origin.lifetime_snapshot_id, context->origin.entry_gpuva, base_device,
                    context->origin.entry_words * sizeof(Word));
            } catch (...) {
                context->lifetime.reset();
            }
        }
    }
    ~DispatchScope() {
        if (installed)
            current_dispatch = previous;
    }
    DispatchScope(const DispatchScope &) = delete;

  private:
    std::unique_ptr<Dispatch> context;
    const Dispatch *previous{};
    bool installed{};
};
struct Segment {
    Dispatch dispatch{};
    Wide engine_gpuva{}, fifo_offset{}, original_count{};
    Word copied_count{};
    bool context_matches{}, parser_equals_fifo{};
    std::array<Word, MaxWords> copied_words{};
};
struct Invocation {
    Word entry{}, method{}, segments{};
    Wide code_generation{}, total_words{};
    bool active{}, truncated{}, fifo_matches{};
    std::array<Segment, MaxSegments> parts{};
    void Reset(Word new_entry, Word new_method, Wide generation) {
        entry = new_entry;
        method = new_method;
        code_generation = generation;
        segments = 0;
        total_words = 0;
        truncated = false;
        fifo_matches = false;
        active = Selected(entry);
    }
    void Append(const void *engine, Wide gpuva, std::span<const Word> copied) {
        if (!active)
            return;
        const Wide offset = total_words;
        total_words += copied.size();
        if (segments == MaxSegments) {
            truncated = true;
            return;
        }
        auto &p = parts[segments++];
        p = {};
        p.engine_gpuva = gpuva;
        p.fifo_offset = offset;
        p.original_count = copied.size();
        const auto remaining = offset < MaxWords ? MaxWords - static_cast<std::size_t>(offset) : 0;
        p.copied_count = static_cast<Word>(std::min(remaining, copied.size()));
        std::copy_n(copied.begin(), p.copied_count, p.copied_words.begin());
        truncated |= p.copied_count != copied.size();
        if (current_dispatch && current_dispatch->engine == engine) {
            p.context_matches = true;
            p.dispatch = *current_dispatch;
            p.parser_equals_fifo =
                p.copied_count == copied.size() && p.dispatch.original_count == copied.size() &&
                p.dispatch.parser_count == copied.size() &&
                std::equal(p.copied_words.begin(), p.copied_words.begin() + p.copied_count,
                           p.dispatch.parser_words.begin());
        }
    }
    void CheckFifo(std::span<const Word> fifo) {
        fifo_matches = !truncated && total_words == fifo.size();
        for (Word i = 0; fifo_matches && i < segments; ++i) {
            const auto &p = parts[i];
            fifo_matches =
                p.fifo_offset <= fifo.size() && p.copied_count <= fifo.size() - p.fifo_offset &&
                std::equal(p.copied_words.begin(), p.copied_words.begin() + p.copied_count,
                           fifo.begin() + p.fifo_offset);
        }
    }
};
static_assert(sizeof(Invocation) < 8192);
inline thread_local const Invocation *current_invocation{};
class InvocationScope {
  public:
    InvocationScope(Invocation *value, std::span<const Word> fifo) {
        if (!Enabled())
            return;
        previous = current_invocation;
        current_invocation = nullptr;
        installed = true;
        if (value && value->active) {
            value->CheckFifo(fifo);
            current_invocation = value;
        }
    }
    ~InvocationScope() {
        if (installed)
            current_invocation = previous;
    }

  private:
    const Invocation *previous{};
    bool installed{};
};
inline std::unique_ptr<Invocation> CopyCurrent(Word entry, Wide generation) {
    if (!Enabled() || !current_invocation || current_invocation->entry != entry ||
        current_invocation->code_generation != generation)
        return {};
    return std::make_unique<Invocation>(*current_invocation);
}
inline void Save(const Invocation &invocation, const std::filesystem::path &path) {
    if (std::filesystem::exists(path))
        return;
    const auto temporary = std::filesystem::path{path.string() + ".tmp"};
    std::ofstream out{temporary};
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::boolalpha;
    const auto origin = [&](const Origin &o) {
        out << "{\"source_kind\":" << static_cast<Word>(o.source) << ",\"channel\":" << o.channel
            << ",\"submission\":" << o.submission
            << ",\"snapshot_revision\":" << o.snapshot_revision
            << ",\"snapshot_suspensions\":" << o.snapshot_suspensions
            << ",\"current_suspensions\":" << o.current_suspensions
            << ",\"entry_index\":" << o.entry_index << ",\"gp_header\":" << o.gp_header
            << ",\"entry_gpuva\":" << o.entry_gpuva << ",\"entry_words\":" << o.entry_words
            << ",\"resume_word\":" << o.resume_word << ",\"gpuva_known\":" << o.gpuva_known
            << ",\"gpu_written\":" << o.gpu_written << ",\"safe_read\":" << o.safe_read
            << ",\"snapshot_available\":" << o.snapshot_available
            << ",\"lifetime_snapshot_id\":" << o.lifetime_snapshot_id << '}';
    };
    const auto words = [&](const auto &array, Word count) {
        out << '[';
        for (Word i = 0; i < count; ++i) {
            if (i)
                out << ',';
            out << array[i];
        }
        out << ']';
    };
    out << "{\"format\":1,\"entry\":" << invocation.entry << ",\"method\":" << invocation.method
        << ",\"code_generation\":" << invocation.code_generation
        << ",\"total_original_words\":" << invocation.total_words
        << ",\"truncated\":" << invocation.truncated
        << ",\"fifo_matches_initial_arguments\":" << invocation.fifo_matches
        << ",\"snapshot_label\":\"retained_snapshot_at_parse_not_original_submit\",\"segments\":[";
    for (Word i = 0; i < invocation.segments; ++i) {
        if (i)
            out << ',';
        const auto &p = invocation.parts[i];
        const auto &d = p.dispatch;
        out << "{\"fifo_offset\":" << p.fifo_offset << ",\"original_count\":" << p.original_count
            << ",\"engine_gpuva\":" << p.engine_gpuva
            << ",\"context_matches\":" << p.context_matches
            << ",\"parser_equals_fifo\":" << p.parser_equals_fifo << ",\"fifo_copied_words\":";
        words(p.copied_words, p.copied_count);
        out << ",\"parser_dispatch_words\":";
        words(d.parser_words, d.parser_count);
        out << ",\"retained_snapshot_words\":";
        words(d.snapshot_words, d.snapshot_count);
        out << ",\"pre_safe_read_observation\":{\"available\":" << d.pre_safe_read.available
            << ",\"label\":\"observed_before_existing_safe_read_flush_not_executed_or_submit_bytes\""
            << ",\"entry_gpuva\":" << d.pre_safe_read.entry_gpuva
            << ",\"entry_device_known\":" << d.pre_safe_read.device_known
            << ",\"entry_device_at_observation\":" << d.pre_safe_read.entry_device
            << ",\"entry_words\":";
        words(d.pre_safe_read.words, d.pre_safe_read.count);
        out << '}';
        out << ",\"lifetime_observation\":";
        MacroArgumentLifetime::WriteJson(out, d.lifetime.get());
        out << ",\"method\":" << d.method << ",\"pending\":" << d.pending
            << ",\"non_incrementing\":" << d.non_incrementing
            << ",\"increment_once\":" << d.increment_once << ",\"last\":" << d.last
            << ",\"payload_word\":" << d.payload_word
            << ",\"payload_gpuva_known\":" << d.payload_gpuva_known
            << ",\"payload_gpuva\":" << d.payload_gpuva
            << ",\"payload_device_known\":" << d.payload_device_known
            << ",\"payload_device_at_dispatch\":" << d.payload_device
            << ",\"context_known\":" << d.context_known << ",\"origin\":";
        origin(d.origin);
        out << ",\"packet_header\":{\"known\":" << d.packet.known << ",\"raw\":" << d.packet.raw
            << ",\"immediate\":" << d.packet.immediate << ",\"word\":" << d.packet.header_word
            << ",\"origin\":";
        origin(d.packet.origin);
        out << "}}";
    }
    out << "]}\n";
    out.close();
    std::filesystem::rename(temporary, path);
}
} // namespace Tegra::MacroArgumentTrace
