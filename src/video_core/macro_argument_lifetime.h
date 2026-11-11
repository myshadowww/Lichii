// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace Tegra::MacroArgumentLifetime {
using U64 = std::uint64_t;
using U32 = std::uint32_t;
using Byte = std::uint8_t;
constexpr std::size_t MaxArena = 65536, MaxWords = 32, MaxEvents = 1024, MaxSnapshots = 4096,
                      MaxResultEvents = 256, MaxEntryIds = 4096;
struct Config {
    U64 begin{}, size{};
    bool enabled{};
};
inline const Config &Configuration() {
    static const Config config = [] {
        Config result;
        const char *arena = std::getenv("EDEN_NX2_MME_LIFETIME_ARENA");
        const char *trace = std::getenv("EDEN_NX2_MME_ARGUMENT_TRACE");
        const char *capture = std::getenv("EDEN_NX2_MME_FAILURE_CAPTURE");
        if (!arena || *arena == '-' || !trace || trace[0] != '1' || !capture || !*capture)
            return result;
        char *end{};
        errno = 0;
        result.begin = std::strtoull(arena, &end, 0);
        if (errno || end == arena || *end != ':')
            return Config{};
        const char *size = end + 1;
        if (*size == '-')
            return Config{};
        result.size = std::strtoull(size, &end, 0);
        if (errno || end == size || *end || !result.size || result.size > MaxArena ||
            result.begin > std::numeric_limits<U64>::max() - result.size)
            return Config{};
        result.enabled = true;
        return result;
    }();
    return config;
}
inline bool Enabled() { return Configuration().enabled; }
inline bool Overlaps(U64 a, U64 size, U64 b, U64 other_size) {
    return size && other_size && (a <= b ? b - a < size : a - b < other_size);
}
inline bool WithinOneMappingPage(U64 address, U64 size) {
    return size != 0 && size <= 0x1000 - (address & 0xfff);
}
inline bool InArena(U64 address, U64 size) {
    const auto &c = Configuration();
    return c.enabled && Overlaps(address, size, c.begin, c.size);
}
enum class Kind : U32 {
    CpuPreStore,
    DownloadStore,
    DownloadExcluded,
    GpuDeclared,
    CacheInvalidation,
    UploadStaged
};
// Identity is the current host Buffer object address, not a persistent allocation generation.
struct Source {
    U64 buffer_identity{}, buffer_base{}, buffer_size{};
    U32 route{}, producer{};
};
inline thread_local Source current_source;
class SourceScope {
  public:
    explicit SourceScope(Source value) : previous{current_source} { current_source = value; }
    ~SourceScope() { current_source = previous; }

  private:
    Source previous;
};
// Current DispatchCompute context only; this does not prove per-byte GPU authorship.
struct ComputeContext {
    U64 native_key{}, shader_hash{};
    bool known{};
};
inline thread_local ComputeContext current_compute;
class ComputeScope {
  public:
    ComputeScope(U64 native_key, U64 shader_hash) : previous{current_compute} {
        current_compute = {native_key, shader_hash, true};
    }
    ~ComputeScope() { current_compute = previous; }
    ComputeScope(const ComputeScope &) = delete;
    ComputeScope &operator=(const ComputeScope &) = delete;

  private:
    ComputeContext previous;
};
struct Event {
    U64 sequence{}, address{}, size{};
    Kind kind{};
    Source source{};
    ComputeContext compute{};
};
struct ByteState {
    U64 cpu_pre_store{}, download_store{}, download_excluded{}, gpu_declared{};
    U64 cache_invalidation{}; // WriteMemory notification, not an actual store or caller identity.
    U64 upload_staged{};      // Owned staging-source observation, not GPU completion.
    Byte stored_value{}, excluded_value{}, upload_value{};
};
struct Snapshot {
    U64 id{}, begin_sequence{}, end_sequence{}, gpuva{}, before{}, after{};
    bool before_known{}, after_known{}, mapping_changed{}, in_scope{};
    U32 count{};
    std::array<U32, MaxWords> words{};
};
struct Token {
    U64 begin_sequence{}, gpuva{}, before{};
    U32 count{};
    bool before_known{};
};
struct State {
    std::mutex mutex;
    U64 sequence{}, event_count{}, snapshot_count{};
    std::array<ByteState, MaxArena> bytes{};
    std::array<Event, MaxEvents> events{};
    std::array<Snapshot, MaxSnapshots> snapshots{};
};
inline State &Get() {
    static State state;
    return state;
}
// Sequence records host observation order, never hardware completion or guest store order.
inline std::optional<Token> BeginSnapshot(U64 gpuva, std::optional<U64> before, U64 count) {
    if (!Enabled() || !count || count > MaxWords ||
        !WithinOneMappingPage(gpuva, count * sizeof(U32)) ||
        (before && !WithinOneMappingPage(*before, count * sizeof(U32))))
        return {};
    auto &state = Get();
    std::scoped_lock lock{state.mutex};
    return Token{++state.sequence, gpuva, before.value_or(0), static_cast<U32>(count),
                 before.has_value()};
}
inline U64 FinishSnapshot(const Token &token, std::optional<U64> after,
                          std::span<const U32> existing_copied_words) {
    if (!Enabled() || existing_copied_words.size() != token.count || token.count > MaxWords)
        return 0;
    const U64 size = token.count * sizeof(U32);
    if (!WithinOneMappingPage(token.gpuva, size) ||
        (token.before_known && !WithinOneMappingPage(token.before, size)) ||
        (after && !WithinOneMappingPage(*after, size)))
        return 0;
    if (!(token.before_known && InArena(token.before, size)) && !(after && InArena(*after, size)))
        return 0;
    auto &state = Get();
    std::scoped_lock lock{state.mutex};
    Snapshot snapshot;
    snapshot.id = ++state.snapshot_count;
    snapshot.begin_sequence = token.begin_sequence;
    snapshot.end_sequence = ++state.sequence;
    snapshot.gpuva = token.gpuva;
    snapshot.before = token.before;
    snapshot.after = after.value_or(0);
    snapshot.before_known = token.before_known;
    snapshot.after_known = after.has_value();
    snapshot.mapping_changed =
        token.before_known != after.has_value() || (token.before_known && token.before != *after);
    snapshot.in_scope = true;
    snapshot.count = token.count;
    std::copy(existing_copied_words.begin(), existing_copied_words.end(), snapshot.words.begin());
    state.snapshots[snapshot.id % MaxSnapshots] = snapshot;
    return snapshot.id;
}
inline void Note(Kind kind, U64 address, U64 size, const Byte *source = nullptr) {
    if (!Enabled() || !InArena(address, size))
        return;
    auto &state = Get();
    const auto &config = Configuration();
    std::scoped_lock lock{state.mutex};
    const Event event{
        ++state.sequence,
        address,
        size,
        kind,
        (kind == Kind::CpuPreStore || kind == Kind::CacheInvalidation) ? Source{} : current_source,
        kind == Kind::GpuDeclared ? current_compute : ComputeContext{}};
    state.events[state.event_count++ % MaxEvents] = event;
    const U64 begin = std::max(address, config.begin);
    const U64 end = address > std::numeric_limits<U64>::max() - size
                        ? config.begin + config.size
                        : std::min(address + size, config.begin + config.size);
    for (U64 current = begin; current < end; ++current) {
        auto &byte = state.bytes[current - config.begin];
        switch (kind) {
        case Kind::CpuPreStore:
            byte.cpu_pre_store = event.sequence;
            break;
        case Kind::CacheInvalidation:
            byte.cache_invalidation = event.sequence;
            break;
        case Kind::GpuDeclared:
            byte.gpu_declared = event.sequence;
            break;
        case Kind::DownloadStore:
            byte.download_store = event.sequence;
            byte.stored_value = source[current - address];
            break;
        case Kind::DownloadExcluded:
            byte.download_excluded = event.sequence;
            byte.excluded_value = source[current - address];
            break;
        case Kind::UploadStaged:
            byte.upload_staged = event.sequence;
            byte.upload_value = source[current - address];
            break;
        }
    }
}
struct Observation {
    U64 requested_snapshot{}, capture_sequence{}, gpuva{}, device{}, size{}, byte_offset{};
    bool device_known{}, out_of_scope{}, partial_scope{}, snapshot_missing{}, history_dropped{},
        result_events_dropped{}, mapping_matches_snapshot{};
    Snapshot snapshot{};
    std::vector<ByteState> bytes;
    std::vector<Event> events;
};
inline std::shared_ptr<const Observation> Capture(U64 snapshot_id, U64 gpuva,
                                                  std::optional<U64> device, U64 size) {
    if (!Enabled())
        return {};
    auto result = std::make_shared<Observation>();
    result->requested_snapshot = snapshot_id;
    result->gpuva = gpuva;
    result->device = device.value_or(0);
    result->device_known = device.has_value();
    result->size = size;
    result->out_of_scope = !device || !InArena(*device, size) || size > MaxWords * sizeof(U32) ||
                           !WithinOneMappingPage(gpuva, size) ||
                           (device && !WithinOneMappingPage(*device, size));
    auto &state = Get();
    const auto &config = Configuration();
    std::scoped_lock lock{state.mutex};
    result->capture_sequence = ++state.sequence;
    const auto &snapshot = state.snapshots[snapshot_id % MaxSnapshots];
    result->snapshot_missing = !snapshot_id || snapshot.id != snapshot_id;
    if (!result->snapshot_missing) {
        result->snapshot = snapshot;
        // Addresses are bases of this GP entry; caller supplies that base, not a guessed offset.
        result->mapping_matches_snapshot = device && snapshot.before_known &&
                                           snapshot.after_known && !snapshot.mapping_changed &&
                                           snapshot.before == *device && snapshot.gpuva == gpuva;
    }
    if (result->out_of_scope)
        return result;
    const U64 begin = std::max(*device, config.begin);
    const U64 end = *device > std::numeric_limits<U64>::max() - size
                        ? config.begin + config.size
                        : std::min(*device + size, config.begin + config.size);
    result->byte_offset = begin - *device;
    result->partial_scope = result->byte_offset != 0 || end - begin != size;
    result->bytes.assign(state.bytes.begin() + (begin - config.begin),
                         state.bytes.begin() + (end - config.begin));
    const U64 first = state.event_count > MaxEvents ? state.event_count - MaxEvents : 0;
    const U64 floor = result->snapshot_missing ? 0 : snapshot.begin_sequence;
    result->history_dropped =
        state.event_count > MaxEvents && state.events[first % MaxEvents].sequence > floor;
    // Keep the latest overlapping events: broad declarations must not displace the
    // final actual landing. Restore chronological order for the serialized result.
    for (U64 i = state.event_count; i > first;) {
        const auto &event = state.events[--i % MaxEvents];
        if (event.sequence < floor || !Overlaps(event.address, event.size, *device, size))
            continue;
        if (result->events.size() == MaxResultEvents) {
            result->result_events_dropped = true;
            continue;
        }
        result->events.push_back(event);
    }
    std::reverse(result->events.begin(), result->events.end());
    return result;
}
inline void WriteJson(std::ostream &out, const Observation *o) {
    if (!o) {
        out << "null";
        return;
    }
    const auto &c = Configuration();
    out << "{\"label\":\"host_observation_order_not_hardware_completion\",\"latest_bytes_only\":"
           "true"
        << ",\"cpu_notifications_are_pre_store\":true,\"gpu_ranges_are_declarations\":true"
        << ",\"writer_coverage_complete\":false,\"arena_begin\":" << c.begin
        << ",\"arena_size\":" << c.size << ",\"requested_snapshot\":" << o->requested_snapshot
        << ",\"capture_sequence\":" << o->capture_sequence << ",\"gpuva\":" << o->gpuva
        << ",\"device\":" << o->device << ",\"device_known\":" << o->device_known
        << ",\"size\":" << o->size << ",\"byte_offset\":" << o->byte_offset
        << ",\"out_of_scope\":" << o->out_of_scope << ",\"partial_scope\":" << o->partial_scope
        << ",\"snapshot_missing_or_evicted\":" << o->snapshot_missing
        << ",\"history_ring_dropped\":" << o->history_dropped
        << ",\"result_events_dropped\":" << o->result_events_dropped
        << ",\"mapping_matches_snapshot\":" << o->mapping_matches_snapshot;
    const auto &s = o->snapshot;
    out << ",\"snapshot\":{\"id\":" << s.id << ",\"begin_sequence\":" << s.begin_sequence
        << ",\"end_sequence\":" << s.end_sequence << ",\"gpuva\":" << s.gpuva
        << ",\"before_known\":" << s.before_known << ",\"before_device\":" << s.before
        << ",\"after_known\":" << s.after_known << ",\"after_device\":" << s.after
        << ",\"mapping_changed_during_observation\":" << s.mapping_changed << ",\"words\":[";
    for (U32 i = 0; i < s.count; ++i) {
        if (i)
            out << ',';
        out << s.words[i];
    }
    out << "]},\"byte_fields\":[\"cpu_pre_store_seq\",\"download_store_seq\","
           "\"download_excluded_seq\",\"gpu_declared_seq\",\"stored_value\",\"excluded_value\","
           "\"cache_invalidation_seq\",\"upload_staged_seq\",\"upload_staged_value\"],"
           "\"bytes\":[";
    for (std::size_t i = 0; i < o->bytes.size(); ++i) {
        if (i)
            out << ',';
        const auto &b = o->bytes[i];
        out << '[' << b.cpu_pre_store << ',' << b.download_store << ',' << b.download_excluded
            << ',' << b.gpu_declared << ',' << U32{b.stored_value} << ',' << U32{b.excluded_value}
            << ',' << b.cache_invalidation << ',' << b.upload_staged << ',' << U32{b.upload_value}
            << ']';
    }
    out << "],\"events\":[";
    for (std::size_t i = 0; i < o->events.size(); ++i) {
        if (i)
            out << ',';
        const auto &e = o->events[i];
        out << "{\"sequence\":" << e.sequence << ",\"kind\":" << static_cast<U32>(e.kind)
            << ",\"address\":" << e.address << ",\"size\":" << e.size
            << ",\"buffer_identity\":" << e.source.buffer_identity
            << ",\"buffer_base\":" << e.source.buffer_base
            << ",\"buffer_size\":" << e.source.buffer_size << ",\"buffer_offset\":"
            << (e.source.buffer_identity && e.address >= e.source.buffer_base
                    ? e.address - e.source.buffer_base
                    : 0)
            << ",\"download_route\":" << e.source.route
            << ",\"gpu_producer_class\":" << e.source.producer
            << ",\"compute_context_known\":" << e.compute.known
            << ",\"compute_native_key\":" << e.compute.native_key
            << ",\"compute_shader_hash\":" << e.compute.shader_hash << '}';
    }
    out << "]}";
}
} // namespace Tegra::MacroArgumentLifetime
