// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/syncpoint_manager.h"

using Tegra::Host1x::SyncpointManager;

TEST_CASE("Already reached syncpoints run actions immediately with a zero handle",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    u32 guest_calls{};
    u32 host_calls{};
    for (u32 value = 0; value < 3; ++value) {
        const auto guest = manager.RegisterGuestAction(0, value, [&] { ++guest_calls; });
        const auto host = manager.RegisterHostAction(0, value, [&] { ++host_calls; });
        REQUIRE(guest == 0);
        REQUIRE(host == 0);
        REQUIRE(guest_calls == value + 1);
        REQUIRE(host_calls == value + 1);
        manager.DeregisterGuestAction(0, guest);
        manager.DeregisterHostAction(0, host);
        manager.IncrementGuest(0);
        manager.IncrementHost(0);
        REQUIRE(guest_calls == value + 1);
        REQUIRE(host_calls == value + 1);
    }
}

TEST_CASE("Cancelling pending syncpoint actions prevents their callbacks",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    u32 calls{};
    const auto guest = manager.RegisterGuestAction(0, 1, [&] { ++calls; });
    const auto host = manager.RegisterHostAction(0, 1, [&] { ++calls; });
    REQUIRE(guest != 0);
    REQUIRE(host != 0);
    REQUIRE(guest != host);
    manager.DeregisterGuestAction(0, guest);
    manager.DeregisterHostAction(0, host);
    manager.DeregisterGuestAction(0, guest);
    manager.DeregisterHostAction(0, host);
    manager.IncrementGuest(0);
    manager.IncrementHost(0);
    REQUIRE(calls == 0);
    REQUIRE(manager.GetGuestSyncpointValue(0) == 1);
    REQUIRE(manager.GetHostSyncpointValue(0) == 1);
}

TEST_CASE("Cancelling completed syncpoint actions is harmless",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    u32 guest_calls{};
    u32 host_calls{};
    const auto guest = manager.RegisterGuestAction(0, 1, [&] { ++guest_calls; });
    const auto host = manager.RegisterHostAction(0, 1, [&] { ++host_calls; });
    manager.IncrementGuest(0);
    manager.IncrementHost(0);
    REQUIRE(guest_calls == 1);
    REQUIRE(host_calls == 1);
    for (u32 repeat = 0; repeat < 3; ++repeat) {
        manager.DeregisterGuestAction(0, guest);
        manager.DeregisterHostAction(0, host);
        manager.IncrementGuest(0);
        manager.IncrementHost(0);
    }
    REQUIRE(guest_calls == 1);
    REQUIRE(host_calls == 1);
}

TEST_CASE("Stale syncpoint handles cannot cancel replacement actions",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    u32 guest_calls{};
    u32 host_calls{};
    SyncpointManager::ActionHandle previous_guest{};
    SyncpointManager::ActionHandle previous_host{};
    for (u32 target = 1; target <= 256; ++target) {
        const auto guest = manager.RegisterGuestAction(0, target, [&] { ++guest_calls; });
        const auto host = manager.RegisterHostAction(0, target, [&] { ++host_calls; });
        REQUIRE(guest > previous_guest);
        REQUIRE(host > previous_host);
        REQUIRE(guest != host);
        manager.DeregisterGuestAction(0, previous_guest);
        manager.DeregisterHostAction(0, previous_host);
        manager.IncrementGuest(0);
        manager.IncrementHost(0);
        REQUIRE(guest_calls == target);
        REQUIRE(host_calls == target);
        previous_guest = guest;
        previous_host = host;
    }
}

TEST_CASE("Syncpoint action IDs stay distinct across counters and host guest banks",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    const auto stale = manager.RegisterGuestAction(0, 1, [] {});
    manager.IncrementGuest(0);
    u32 guest_calls{};
    u32 host_calls{};
    const auto guest = manager.RegisterGuestAction(1, 1, [&] { ++guest_calls; });
    const auto host = manager.RegisterHostAction(1, 1, [&] { ++host_calls; });
    REQUIRE(stale != guest);
    REQUIRE(stale != host);
    REQUIRE(guest != host);
    manager.DeregisterGuestAction(1, stale);
    manager.DeregisterHostAction(1, stale);
    manager.DeregisterGuestAction(1, host);
    manager.DeregisterHostAction(1, guest);
    manager.DeregisterGuestAction(0, guest);
    manager.DeregisterHostAction(0, host);
    manager.IncrementGuest(1);
    manager.IncrementHost(1);
    REQUIRE(guest_calls == 1);
    REQUIRE(host_calls == 1);
}

TEST_CASE("The guest mirror sees each increment before it is published and its actions run",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    std::vector<u32> mirrored;
    bool published_early{};
    const auto mirror = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        if (id == 3) {
            published_early |= value != 0 && manager.IsReadyGuest(id, value);
            mirrored.push_back(value);
        }
    });
    u32 mirrored_at_action{};
    manager.RegisterGuestAction(3, 2, [&] { mirrored_at_action = mirrored.back(); });
    manager.IncrementGuest(3);
    manager.IncrementGuest(3);
    manager.IncrementHost(3);
    REQUIRE(mirrored == std::vector<u32>{0, 1, 2});
    REQUIRE_FALSE(published_early);
    REQUIRE(mirrored_at_action == 2);

    manager.DeregisterGuestMirror(mirror);
    manager.IncrementGuest(3);
    REQUIRE(mirrored.size() == 3);
    REQUIRE(manager.GetGuestSyncpointValue(3) == 3);
}

TEST_CASE("Guest increments wait for a stable copy of the guest values",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    std::thread increment;
    u32 value_while_stable{};
    manager.WithGuestValuesStable([&] {
        increment = std::thread([&] { manager.IncrementGuest(0); });
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        value_while_stable = manager.GetGuestSyncpointValue(0);
    });
    increment.join();
    REQUIRE(value_while_stable == 0);
    REQUIRE(manager.GetGuestSyncpointValue(0) == 1);
}

TEST_CASE("Guest mirror registration backfills values before already-ready actions",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    manager.IncrementGuest(0);
    manager.IncrementGuest(191);
    manager.IncrementGuest(191);
    std::array<u32, SyncpointManager::NUM_MAX_SYNCPOINTS> page{};
    u32 copied{};
    const auto mirror = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        page[id] = value;
        ++copied;
    });
    REQUIRE(mirror != 0);
    REQUIRE(copied == page.size());
    REQUIRE(page[0] == 1);
    REQUIRE(page[191] == 2);
    bool ready_action_saw_page{};
    REQUIRE(manager.RegisterGuestAction(191, 2, [&] {
        ready_action_saw_page = page[191] == manager.GetGuestSyncpointValue(191);
    }) == 0);
    REQUIRE(ready_action_saw_page);
    manager.DeregisterGuestMirror(mirror);
}

TEST_CASE("Guest mirrors belong to their sessions across detach and reattach",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    auto first_page = std::make_shared<u32>();
    std::weak_ptr<u32> first_lifetime = first_page;
    const auto first = manager.RegisterGuestMirror([page = first_page](u32 id, u32 value) {
        if (id == 0) {
            *page = value;
        }
    });
    u32 second_page{};
    const auto second = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        if (id == 0) {
            second_page = value;
        }
    });
    REQUIRE(first != second);
    bool both_ready{};
    manager.RegisterGuestAction(0, 1, [&] {
        both_ready = *first_page == 1 && second_page == 1;
    });
    manager.IncrementGuest(0);
    REQUIRE(both_ready);
    first_page.reset();
    manager.DeregisterGuestMirror(first);
    REQUIRE(first_lifetime.expired());
    manager.IncrementGuest(0);
    REQUIRE(second_page == 2);

    u32 replacement_page{};
    const auto replacement = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        if (id == 0) {
            replacement_page = value;
        }
    });
    REQUIRE(replacement != first);
    REQUIRE(replacement_page == 2);
    manager.DeregisterGuestMirror(first); // stale session cannot detach its replacement
    manager.DeregisterGuestMirror(second);
    manager.DeregisterGuestMirror(0);
    manager.IncrementGuest(0);
    REQUIRE(second_page == 2);
    REQUIRE(replacement_page == 3);
    manager.DeregisterGuestMirror(replacement);
}

TEST_CASE("An increment during mirror registration follows the complete initial copy",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    std::latch initial_copy_started{1};
    std::latch increment_started{1};
    std::vector<std::pair<u32, u32>> copies;
    bool published_early{};
    std::thread increment([&] {
        initial_copy_started.wait();
        increment_started.count_down();
        manager.IncrementGuest(0);
    });
    const auto mirror = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        if (id == 0 && value == 0) {
            initial_copy_started.count_down();
            increment_started.wait();
        }
        copies.emplace_back(id, value);
        published_early |= value == 1 && manager.IsReadyGuest(id, value);
    });
    increment.join();
    REQUIRE(copies.size() == SyncpointManager::NUM_MAX_SYNCPOINTS + 1);
    for (u32 id = 0; id < SyncpointManager::NUM_MAX_SYNCPOINTS; ++id) {
        REQUIRE(copies[id] == std::pair<u32, u32>{id, 0});
    }
    REQUIRE(copies.back() == std::pair<u32, u32>{0, 1});
    REQUIRE_FALSE(published_early);
    manager.DeregisterGuestMirror(mirror);
}

TEST_CASE("Detaching a guest mirror finishes its callback before releasing the session",
          "[video_core][syncpoint_actions]") {
    SyncpointManager manager;
    auto page = std::make_shared<u32>();
    std::weak_ptr<u32> lifetime = page;
    std::latch callback_started{1};
    std::latch detach_started{1};
    std::latch finish_callback{1};
    std::atomic<bool> detached{};
    u32 calls{};
    const auto mirror = manager.RegisterGuestMirror([&, page](u32 id, u32 value) {
        if (id != 0 || value == 0) {
            return;
        }
        callback_started.count_down();
        finish_callback.wait();
        *page = value;
        ++calls;
    });
    page.reset();
    std::thread increment([&] { manager.IncrementGuest(0); });
    callback_started.wait();
    std::thread detach([&] {
        detach_started.count_down();
        manager.DeregisterGuestMirror(mirror);
        detached.store(true);
    });
    detach_started.wait();
    const bool detached_while_running = detached.load();
    finish_callback.count_down();
    increment.join();
    detach.join();
    REQUIRE_FALSE(detached_while_running);
    REQUIRE(lifetime.expired());
    manager.IncrementGuest(0);
    REQUIRE(calls == 1);
    REQUIRE(manager.GetGuestSyncpointValue(0) == 2);
}
