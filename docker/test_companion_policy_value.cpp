// PORT-013 (KAP-558): value-level coverage for the companion policy
// contract (Companion/Observation.h, Companion/Intent.h,
// Companion/Policy.h). Standalone: no engine, no Docker. Compiled and run
// by docker/test_companion_policy_value.py.
#include "Companion/Policy.h"

#include <cstdio>
#include <string>
#include <string>
#include <type_traits>

namespace CC = Companion;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// Compile-time pins: the contract is bounded value types only.
static_assert(std::is_trivially_copyable<CC::Observation>::value &&
              std::is_standard_layout<CC::Observation>::value, "observation must be a value type");
static_assert(std::is_trivially_copyable<CC::Intent>::value &&
              std::is_standard_layout<CC::Intent>::value, "directive must be a value type");

static void SetCandidate(CC::Observation& o, CC::Action a)
{
    switch (a)
    {
        case CC::Action::Hold:
            o.held = true;
            break;
        case CC::Action::Follow:
            o.following = true;
            o.ownerAvailable = true;
            break;
        case CC::Action::ContinueCombat:
            o.target = 11;
            break;
        case CC::Action::Assist:
            o.assistTarget = 22;
            break;
        case CC::Action::Defend:
            o.defendTarget = 33;
            break;
        case CC::Action::Loot:
            o.lootTarget = 44;
            break;
        default:
            break;
    }
}

static void TestSingleCandidates()
{
    struct { CC::Action action; uint64_t target; } const singles[] = {
        {CC::Action::Hold, 0},
        {CC::Action::Follow, 0},
        {CC::Action::ContinueCombat, 11},
        {CC::Action::Assist, 22},
        {CC::Action::Defend, 33},
        {CC::Action::Loot, 44},
    };
    for (auto const& s : singles)
    {
        CC::Observation o;
        o.generation = 9;
        SetCandidate(o, s.action);
        CC::Intent i = CC::Select(o);
        CHECK(i.action == s.action);
        CHECK(i.target == s.target);
        CHECK(i.generation == 9);
        CHECK(CC::IsCurrent(i, 9));
    }
    // Nothing set and no follow goal: no behavior.
    CC::Observation empty;
    CHECK(CC::Select(empty).action == CC::Action::None);
    // A follow goal with an unavailable owner is a Hold (target 0).
    CC::Observation noOwner;
    noOwner.following = true;
    noOwner.ownerAvailable = false;
    CC::Intent hold = CC::Select(noOwner);
    CHECK(hold.action == CC::Action::Hold);
    CHECK(hold.target == 0);
}

static void TestPriorityEdges()
{
    // The complete documented priority, all 15 pairwise edges:
    // Hold > Assist > ContinueCombat > Defend > Loot > Follow.
    struct { CC::Action higher; CC::Action lower; } const edges[] = {
        {CC::Action::Hold, CC::Action::Assist},
        {CC::Action::Hold, CC::Action::ContinueCombat},
        {CC::Action::Hold, CC::Action::Defend},
        {CC::Action::Hold, CC::Action::Loot},
        {CC::Action::Hold, CC::Action::Follow},
        {CC::Action::Assist, CC::Action::ContinueCombat},
        {CC::Action::Assist, CC::Action::Defend},
        {CC::Action::Assist, CC::Action::Loot},
        {CC::Action::Assist, CC::Action::Follow},
        {CC::Action::ContinueCombat, CC::Action::Defend},
        {CC::Action::ContinueCombat, CC::Action::Loot},
        {CC::Action::ContinueCombat, CC::Action::Follow},
        {CC::Action::Defend, CC::Action::Loot},
        {CC::Action::Defend, CC::Action::Follow},
        {CC::Action::Loot, CC::Action::Follow},
    };
    // indexed by enum order: None, Hold, Follow, ContinueCombat, Assist, Loot, Defend
    uint64_t const targetFor[7] = {0, 0, 0, 11, 22, 44, 33};
    for (auto const& e : edges)
    {
        CC::Observation o;
        o.generation = 4;
        SetCandidate(o, e.higher);
        SetCandidate(o, e.lower);
        CC::Intent i = CC::Select(o);
        CHECK(i.action == e.higher);
        CHECK(i.target == targetFor[(int)e.higher]);
    }
}

static void TestStaleGeneration()
{
    CC::Observation o;
    o.generation = 3;
    o.assistTarget = 22;
    CC::Intent i = CC::Select(o);
    CHECK(CC::IsCurrent(i, 3));
    // A newer owner order bumps the generation; the old directive is
    // stale and must be rejected at execution.
    CHECK(!CC::IsCurrent(i, 4));
    CHECK(!CC::IsCurrent(i, 2));
    CC::Intent plain{CC::Action::Follow, 7, 0};
    CHECK(CC::IsCurrent(plain, 7));
    CHECK(!CC::IsCurrent(plain, 8));
}

static void TestVersioning()
{
    CC::Observation o;
    CHECK(o.version == CC::kObservationVersion);
    CHECK(CC::kObservationVersion == 1);
    CC::Intent i{CC::Action::Follow, 1, 0};
    CHECK(i.version == CC::kDirectiveVersion);
    CHECK(CC::kDirectiveVersion == 1);
}

int main()
{
    TestSingleCandidates();
    TestPriorityEdges();
    TestStaleGeneration();
    TestVersioning();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
