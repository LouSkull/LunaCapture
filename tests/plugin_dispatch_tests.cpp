#include "PluginSystem.hpp"

#include <iostream>
#include <vector>

namespace {
bool Equal(const std::vector<std::size_t>& left, std::initializer_list<std::size_t> right) {
    return left == std::vector<std::size_t>(right);
}
}

int main() {
    using LunaPluginDispatchDetail::HostEventIndex;
    using LunaPluginDispatchDetail::FairEventDispatchScheduler;
    using LunaPluginDispatchDetail::SelectFairAtomicBatch;

    // Host-event lookup is exact and bounded. In particular, differently cased
    // input must not create an independent fairness cursor.
    const auto savedIndex = HostEventIndex(L"capture_saved");
    if (!savedIndex || HostEventIndex(L"Capture_Saved") || HostEventIndex(L"timer") ||
        HostEventIndex(L"not_a_host_event")) return 10;

    const std::vector<std::size_t> equalHandlers{128, 128, 128, 128};
    const auto first = SelectFairAtomicBatch(equalHandlers, 0, 256);
    if (!Equal(first.handlerIndices, {0, 1}) || first.nextCursor != 2) return 20;
    const auto second = SelectFairAtomicBatch(equalHandlers, first.nextCursor, 256);
    if (!Equal(second.handlerIndices, {2, 3}) || second.nextCursor != 0) return 21;

    // Admission remains atomic: a block that does not fit contributes no prefix,
    // and the next dispatch starts with that skipped block.
    const std::vector<std::size_t> unevenHandlers{100, 100, 100};
    const auto unevenFirst = SelectFairAtomicBatch(unevenHandlers, 0, 256);
    if (!Equal(unevenFirst.handlerIndices, {0, 1}) || unevenFirst.nextCursor != 2) return 30;
    const auto unevenSecond = SelectFairAtomicBatch(unevenHandlers, unevenFirst.nextCursor, 256);
    if (!Equal(unevenSecond.handlerIndices, {2, 0}) || unevenSecond.nextCursor != 1) return 31;
    const auto unevenThird = SelectFairAtomicBatch(unevenHandlers, unevenSecond.nextCursor, 256);
    if (!Equal(unevenThird.handlerIndices, {1, 2}) || unevenThird.nextCursor != 0) return 32;

    // If every block fits, all still run and only their deterministic starting
    // order rotates for the next call.
    const std::vector<std::size_t> smallHandlers{5, 10, 15};
    const auto allFit = SelectFairAtomicBatch(smallHandlers, 1, 256);
    if (!Equal(allFit.handlerIndices, {1, 2, 0}) || allFit.nextCursor != 2) return 40;

    const auto empty = SelectFairAtomicBatch({}, 123, 256);
    if (!empty.handlerIndices.empty() || empty.nextCursor != 0) return 50;

    // The stateful seam proves cursors are independent per exact host event and
    // that reload's Reset operation restores deterministic source-order starts.
    FairEventDispatchScheduler scheduler;
    const auto savedFirst = scheduler.Select(L"capture_saved", equalHandlers, 256);
    const auto savedSecond = scheduler.Select(L"capture_saved", equalHandlers, 256);
    const auto failedFirst = scheduler.Select(L"capture_failed", equalHandlers, 256);
    if (!savedFirst || !savedSecond || !failedFirst ||
        !Equal(savedFirst->handlerIndices, {0, 1}) ||
        !Equal(savedSecond->handlerIndices, {2, 3}) ||
        !Equal(failedFirst->handlerIndices, {0, 1})) return 60;
    if (scheduler.Select(L"CAPTURE_SAVED", equalHandlers, 256)) return 61;
    scheduler.Reset();
    const auto savedAfterReset = scheduler.Select(L"capture_saved", equalHandlers, 256);
    if (!savedAfterReset || !Equal(savedAfterReset->handlerIndices, {0, 1})) return 62;

    std::cout << "Plugin dispatch fairness tests passed\n";
    return 0;
}
