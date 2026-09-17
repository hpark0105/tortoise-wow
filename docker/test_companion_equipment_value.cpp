// PORT-024 (KAP-558): value-level test of the companion equipment
// policy. Pure: no engine, no I/O. Compiles standalone against
// Companion/Equipment.h.
#include "Companion/Equipment.h"
#include <cstdio>

using namespace Companion::Equipment;

static int failures = 0;

#define CHECK(cond, label) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", label); ++failures; } \
    else { std::printf("ok: %s\n", label); } \
} while (0)

static ItemStats mk(uint32_t req, uint32_t ilvl, uint32_t q, uint32_t player)
{
    ItemStats s;
    s.requiredLevel = req;
    s.itemLevel = ilvl;
    s.quality = q;
    s.playerLevel = player;
    return s;
}

int main()
{
    CHECK(kEquipmentObservationVersion == 1, "observation version is 1");

    // Required level dominates the score.
    CHECK(Score(mk(10, 14, 2, 10)) == 10 * 1000 + 2 * 10 + 14, "required level score");

    // No required level: itemLevel*2/3 with a floor of 1.
    CHECK(Score(mk(0, 14, 2, 10)) == 9 * 1000 + 2 * 10 + 14, "14 ilvl maps to effective 9");
    CHECK(Score(mk(0, 5, 1, 10)) == 3 * 1000 + 1 * 10 + 5, "5 ilvl maps to effective 3");
    CHECK(Score(mk(0, 3, 0, 10)) == 2 * 1000 + 0 + 3, "3 ilvl maps to effective 2");
    CHECK(Score(mk(0, 1, 1, 10)) == 1 * 1000 + 1 * 10 + 1, "1 ilvl clamps to effective 1");

    // Neither: the player level is the fallback.
    CHECK(Score(mk(0, 0, 1, 10)) == 10 * 1000 + 1 * 10 + 0, "no item data falls back to player level");

    // Tie-breaks at equal effective level: quality, then item level.
    CHECK(Score(mk(10, 12, 1, 10)) < Score(mk(10, 13, 2, 10)), "quality outranks item level at equal effective level");
    CHECK(Score(mk(10, 12, 2, 10)) < Score(mk(10, 13, 2, 10)), "item level breaks equal quality");

    // Verdicts: strict upgrades only.
    CHECK(Compare(9034, 6020) == Verdict::Equip, "strict upgrade equips");
    CHECK(Compare(6020, 6020) == Verdict::Keep, "sidegrade (equal score) keeps");
    CHECK(Compare(3015, 6020) == Verdict::Keep, "downgrade keeps");
    CHECK(Compare(1, 0) == Verdict::Equip, "any positive score beats an empty slot");

    // The exact lab fixture values (templates 1008/15335/3267 at level 10).
    CHECK(Score(mk(0, 10, 1, 10)) == 6020, "lab baseline 1008 scores 6020");
    CHECK(Score(mk(0, 14, 2, 10)) == 9034, "lab upgrade 15335 scores 9034");
    CHECK(Score(mk(0, 5, 1, 10)) == 3015, "lab downgrade 3267 scores 3015");
    CHECK(Compare(Score(mk(0, 14, 2, 10)), Score(mk(0, 10, 1, 10))) == Verdict::Equip, "lab upgrade equips over baseline");
    CHECK(Compare(Score(mk(0, 5, 1, 10)), Score(mk(0, 10, 1, 10))) == Verdict::Keep, "lab downgrade keeps over baseline");

    if (failures == 0)
    {
        std::printf("value tests: ALL OK\n");
        return 0;
    }
    std::printf("value tests: %d FAILURES\n", failures);
    return 1;
}
