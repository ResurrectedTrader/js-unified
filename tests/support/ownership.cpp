#include "support/ownership.h"

#include <vector>

namespace ub_test {
namespace {

/// Deaths per id, indexed by id. A vector rather than a counter because the
/// question a shared-ownership model raises is not "how many died" but "did any
/// one of them die twice".
struct Registry {
    std::vector<int> deaths;
    /// Where each death happened. Only the first is recorded for an id, which
    /// is all that is needed: a second death is already a failure.
    std::vector<std::thread::id> where;
};

Registry& State() {
    static Registry registry;
    return registry;
}

}  // namespace

int Lives::Born() {
    auto& registry = State();
    registry.deaths.push_back(0);
    registry.where.emplace_back();
    return static_cast<int>(registry.deaths.size()) - 1;
}

void Lives::Died(int id) noexcept {
    auto& registry = State();
    if (id >= 0 && static_cast<std::size_t>(id) < registry.deaths.size()) {
        const auto at = static_cast<std::size_t>(id);
        if (registry.deaths[at] == 0) {
            registry.where[at] = std::this_thread::get_id();
        }
        ++registry.deaths[at];
    }
}

int Lives::Deaths(int id) noexcept {
    const auto& deaths = State().deaths;
    if (id < 0 || static_cast<std::size_t>(id) >= deaths.size()) {
        return 0;
    }
    return deaths[static_cast<std::size_t>(id)];
}

int Lives::TotalBorn() noexcept {
    return static_cast<int>(State().deaths.size());
}

int Lives::TotalDeaths() noexcept {
    int total = 0;
    for (const int count : State().deaths) {
        total += count;
    }
    return total;
}

int Lives::Alive() noexcept {
    int alive = 0;
    for (const int count : State().deaths) {
        if (count == 0) {
            ++alive;
        }
    }
    return alive;
}

bool Lives::AnyDestroyedTwice() noexcept {
    for (const int count : State().deaths) {
        if (count > 1) {
            return true;
        }
    }
    return false;
}

bool Lives::EachDestroyedExactlyOnce() noexcept {
    for (const int count : State().deaths) {
        if (count != 1) {
            return false;
        }
    }
    return true;
}

bool Lives::AllDeathsOn(std::thread::id thread) noexcept {
    return DeathsElsewhere(thread) == 0;
}

int Lives::DeathsElsewhere(std::thread::id thread) noexcept {
    const auto& registry = State();
    int elsewhere = 0;
    for (std::size_t at = 0; at < registry.deaths.size(); ++at) {
        if (registry.deaths[at] > 0 && registry.where[at] != thread) {
            ++elsewhere;
        }
    }
    return elsewhere;
}

void Lives::Reset() noexcept {
    State().deaths.clear();
    State().where.clear();
}

}  // namespace ub_test
