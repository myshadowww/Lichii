// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "video_core/macro_argument_trace.h"

namespace Tegra {

// Diagnostic only. Never replays callbacks against the running engine.
class MacroFailureCapture {
public:
    using Word = std::uint32_t;
    static constexpr std::size_t RamWords = 0xc00;
    static constexpr std::size_t MaxFifoWords = 65536;
    static constexpr std::size_t MaxCodeWords = 0x8000;
    static constexpr std::size_t MaxEvents = 65536;
    static constexpr std::size_t MaxPayloadWords = 1024 * 1024;
    enum Flag : Word { Truncated = 1, FifoRewritten = 2, CodeChanged = 4 };

    static bool Enabled(Word start) {
        return Directory() && (start == 0x1d2 || start == 0x1e7) &&
               !(Saved().load(std::memory_order_relaxed) & Bit(start));
    }

    static std::unique_ptr<MacroFailureCapture> Begin(
        Word start, const std::uint64_t& generation, std::span<const Word> code,
        std::vector<Word>& fifo, std::span<Word> ram) noexcept {
        try {
            std::unique_lock lock{Mutex(), std::try_to_lock};
            if (!lock || !Enabled(start) || ram.size() != RamWords ||
                code.size() > MaxCodeWords || fifo.size() > MaxFifoWords) {
                return {};
            }
            return std::unique_ptr<MacroFailureCapture>{new MacroFailureCapture(
                start, generation, code, fifo, ram, std::move(lock))};
        } catch (...) {
            return {}; // Allocation failure must not skip the live call.
        }
    }

    template <typename Read>
    Word ReadCall(Word address, Read& read) {
        Before();
        const Word value = read(address);
        After(0, address, value, 1);
        return value;
    }

    template <typename Emit>
    bool EmitCall(Word address, Word value, Emit& emit) {
        Before();
        const bool ok = emit(address, value);
        After(1, address, value, ok ? 1 : 0);
        return ok;
    }

    template <typename Result>
    void Finish(const Result& result) noexcept {
        if (result.success) return;
        try {
            if (live_generation != generation ||
                !std::equal(code.begin(), code.end(), initial_code.begin())) flags |= CodeChanged;
            if (fifo.size() > MaxFifoWords) flags |= Truncated;
            const auto final_count = std::min(fifo.size(), MaxFifoWords);
            // Version1, little-endian u32 words. Events are seven-word headers followed by
            // appended FIFO words and (index,value) RAM pairs. Event headers:
            // kind, address, value/read-result, callback-ok, FIFO-before, appended, RAM-pairs.
            const std::array<Word, 16> header{
                1, start, Word(generation), Word(generation >> 32), flags,
                Word(result.pc), Word(result.consumed), Word(initial_code.size()),
                Word(initial_fifo.size()), Word(RamWords), Word(final_count), event_count,
                Word(events.size()), Word(result.error.size()), 0, 0};
            static_assert(std::endian::native == std::endian::little);
            const auto directory = std::filesystem::path{Directory()};
            std::filesystem::create_directories(directory);
            const auto name = std::string{"mme-failure-"} + std::to_string(start);
            const auto temporary = directory / (name + ".bin.tmp");
            const auto destination = directory / (name + ".bin");
            // Existing evidence is never overwritten by another run.
            if (std::filesystem::exists(destination)) {
                Saved().fetch_or(Bit(start), std::memory_order_relaxed);
                return;
            }
            std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out.write("MMEFAIL1", 8);
            const auto words = [&](std::span<const Word> data) {
                out.write(reinterpret_cast<const char*>(data.data()), data.size_bytes());
            };
            words(header); words(initial_code); words(initial_fifo); words(initial_ram);
            words(std::span<const Word>{fifo}.first(final_count)); words(ram); words(events);
            out.write(result.error.data(), result.error.size());
            out.close();
            std::filesystem::rename(temporary, destination);
            if (argument_trace) {
                MacroArgumentTrace::Save(*argument_trace, directory / (name + ".arguments.json"));
            }
            Saved().fetch_or(Bit(start), std::memory_order_relaxed);
            std::ofstream info{directory / (name + ".txt")};
            info << "Unsplit macro failure. Entry=" << start << " error=" << result.error
                 << " flags=" << flags << " events=" << event_count
                 << " (JIT result.pc is not maintained by current production code)\n";
        } catch (...) {
            // File-system failures cannot escape through the renderer.
        }
    }

private:
    static const char* Directory() {
        static const char* value = std::getenv("EDEN_NX2_MME_FAILURE_CAPTURE");
        return value && *value ? value : nullptr;
    }
    static Word Bit(Word start) { return start == 0x1d2 ? 1 : 2; }
    static std::atomic<Word>& Saved() { static std::atomic<Word> saved{}; return saved; }
    static std::mutex& Mutex() { static std::mutex mutex; return mutex; }

    MacroFailureCapture(Word entry, const std::uint64_t& gen, std::span<const Word> source,
                        std::vector<Word>& parameters, std::span<Word> dram,
                        std::unique_lock<std::mutex> held)
        : start{entry}, generation{gen}, live_generation{gen}, code{source}, fifo{parameters}, ram{dram},
          lock{std::move(held)}, initial_code{source.begin(), source.end()},
          initial_fifo{parameters.begin(), parameters.end()} {
        std::copy(ram.begin(), ram.end(), initial_ram.begin());
        events.reserve(MaxPayloadWords); // No allocation inside a generated-code callback.
        argument_trace = MacroArgumentTrace::CopyCurrent(start, generation);
    }

    void Before() noexcept {
        if (flags) return;
        if (event_count >= MaxEvents || fifo.size() > MaxFifoWords) {
            flags |= Truncated;
            return;
        }
        before_count = fifo.size();
        std::copy(fifo.begin(), fifo.end(), before_fifo.begin());
        std::copy(ram.begin(), ram.end(), before_ram.begin());
    }

    void After(Word kind, Word address, Word value, Word ok) noexcept {
        if (live_generation != generation) flags |= CodeChanged;
        if (flags) return;
        if (fifo.size() < before_count ||
            !std::equal(before_fifo.begin(), before_fifo.begin() + before_count, fifo.begin())) {
            flags |= FifoRewritten;
            return;
        }
        if (fifo.size() > MaxFifoWords) { flags |= Truncated; return; }
        const auto appended = fifo.size() - before_count;
        Word changes{};
        for (std::size_t i = 0; i < RamWords; ++i) changes += ram[i] != before_ram[i];
        const auto required = 7 + appended + 2 * changes;
        if (required > MaxPayloadWords - events.size()) { flags |= Truncated; return; }
        for (Word item : {kind, address, value, ok, Word(before_count), Word(appended), changes})
            events.push_back(item);
        events.insert(events.end(), fifo.begin() + before_count, fifo.end());
        for (Word i = 0; i < RamWords; ++i) {
            if (ram[i] != before_ram[i]) { events.push_back(i); events.push_back(ram[i]); }
        }
        ++event_count;
    }

    Word start{}, flags{}, event_count{};
    std::uint64_t generation{};
    const std::uint64_t& live_generation;
    std::span<const Word> code;
    std::vector<Word>& fifo;
    std::span<Word> ram;
    std::unique_lock<std::mutex> lock;
    std::vector<Word> initial_code, initial_fifo, events;
    std::unique_ptr<MacroArgumentTrace::Invocation> argument_trace;
    std::array<Word, RamWords> initial_ram{}, before_ram{};
    std::array<Word, MaxFifoWords> before_fifo{};
    std::size_t before_count{};
};
} // namespace Tegra
