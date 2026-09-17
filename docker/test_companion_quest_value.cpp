// PORT-023 (KAP-558): value-level test of the cooperative quest policy.
// Pure: no engine, no I/O. Compiles standalone against Companion/Quest.h.
#include "Companion/Quest.h"
#include <cstdio>

using namespace Companion::Quest;

static int failures = 0;
#define CHECK(cond, label) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", label); ++failures; } \
    else { std::printf("ok: %s\n", label); } \
} while (0)

static Observation base()
{
    Observation o;
    o.ownerAvailable = true;
    o.ownerInParty = true;
    o.held = false;
    o.inCombat = false;
    o.rewarded = false;
    return o;
}

int main()
{
    // Version contract.
    CHECK(kQuestObservationVersion == 1, "observation version is 1");

    // Status mapping: raw QuestStatus enum -> policy status.
    CHECK(MapQuestStatus(0) == kStatusNone, "raw NONE maps to None");
    CHECK(MapQuestStatus(1) == kStatusComplete, "raw COMPLETE maps to Complete");
    CHECK(MapQuestStatus(2) == kStatusNone, "raw UNAVAILABLE maps to None");
    CHECK(MapQuestStatus(3) == kStatusInProgress, "raw INCOMPLETE maps to InProgress");
    CHECK(MapQuestStatus(4) == kStatusNone, "raw AVAILABLE maps to None");
    CHECK(MapQuestStatus(5) == kStatusNone, "raw FAILED maps to None");

    // Accept: owner started, companion none, giver available.
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = true;
        CHECK(Select(o).action == Action::Accept, "accept when owner in-progress and giver near");
    }
    {
        Observation o = base();
        o.ownerStatus = kStatusComplete;
        o.giverAvailable = true;
        CHECK(Select(o).action == Action::Accept, "accept when owner complete and giver near");
    }
    // No accept before the owner (the companion never leads).
    {
        Observation o = base();
        o.ownerStatus = kStatusNone;
        o.giverAvailable = true;
        CHECK(Select(o).action == Action::None, "no accept before the owner");
    }
    // No accept without a giver.
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = false;
        CHECK(Select(o).action == Action::None, "no accept without giver");
    }
    // No re-accept once in the log.
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.myStatus = kStatusInProgress;
        o.giverAvailable = true;
        CHECK(Select(o).action == Action::None, "no re-accept while in progress");
    }

    // Turn-in: companion complete + finisher available.
    {
        Observation o = base();
        o.myStatus = kStatusComplete;
        o.finisherAvailable = true;
        CHECK(Select(o).action == Action::TurnIn, "turn in when complete and finisher near");
    }
    {
        Observation o = base();
        o.myStatus = kStatusComplete;
        o.finisherAvailable = false;
        CHECK(Select(o).action == Action::None, "no turn-in without finisher");
    }
    {
        Observation o = base();
        o.myStatus = kStatusInProgress;
        o.finisherAvailable = true;
        CHECK(Select(o).action == Action::None, "no turn-in before objectives complete");
    }
    // Already rewarded: the persisted COMPLETE row must not re-fire.
    {
        Observation o = base();
        o.myStatus = kStatusComplete;
        o.rewarded = true;
        o.finisherAvailable = true;
        CHECK(Select(o).action == Action::None, "no re-turn-in after reward");
    }

    // Suppressions: hold, combat, party loss, owner loss, rewarded.
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = true;
        o.held = true;
        CHECK(Select(o).action == Action::None, "hold suppresses accept");
    }
    {
        Observation o = base();
        o.myStatus = kStatusComplete;
        o.finisherAvailable = true;
        o.held = true;
        CHECK(Select(o).action == Action::None, "hold suppresses turn-in");
    }
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = true;
        o.inCombat = true;
        CHECK(Select(o).action == Action::None, "combat suppresses accept");
    }
    {
        Observation o = base();
        o.myStatus = kStatusComplete;
        o.finisherAvailable = true;
        o.inCombat = true;
        CHECK(Select(o).action == Action::None, "combat suppresses turn-in");
    }
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = true;
        o.ownerInParty = false;
        CHECK(Select(o).action == Action::None, "party loss suppresses accept");
    }
    {
        Observation o = base();
        o.myStatus = kStatusComplete;
        o.finisherAvailable = true;
        o.ownerInParty = false;
        CHECK(Select(o).action == Action::None, "party loss suppresses turn-in");
    }
    {
        Observation o = base();
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = true;
        o.ownerAvailable = false;
        CHECK(Select(o).action == Action::None, "owner loss suppresses accept");
    }

    // Accept has priority over turn-in when both anchors are available
    // (myStatus none + complete cannot coexist; but owner-complete +
    // companion-in-progress + both anchors: no action, neither fires).
    {
        Observation o = base();
        o.ownerStatus = kStatusComplete;
        o.myStatus = kStatusInProgress;
        o.giverAvailable = true;
        o.finisherAvailable = true;
        CHECK(Select(o).action == Action::None, "in-progress is inert at both anchors");
    }

    // Generation is carried on every non-none intent.
    {
        Observation o = base();
        o.generation = 7;
        o.ownerStatus = kStatusInProgress;
        o.giverAvailable = true;
        Intent i = Select(o);
        CHECK(i.action == Action::Accept && i.generation == 7, "accept carries generation");
        o = base();
        o.generation = 9;
        o.myStatus = kStatusComplete;
        o.finisherAvailable = true;
        i = Select(o);
        CHECK(i.action == Action::TurnIn && i.generation == 9, "turn-in carries generation");
    }

    if (failures == 0)
    {
        std::printf("value tests: ALL OK\n");
        return 0;
    }
    std::printf("value tests: %d FAILURES\n", failures);
    return 1;
}
