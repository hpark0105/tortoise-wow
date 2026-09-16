#include "PlayerBotAI.h"
#include "Player.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "Log.h"
#include "SocialMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "MoveSpline.h"
#include "PlayerBotMgr.h"
#include "ObjectAccessor.h"
#include "Timer.h"
#include "Group.h"
#include "WorldPacket.h"
#include "Map.h"
#include "Maps/GridSearchers.h"
#include "QuestDef.h"
#include "SpellMgr.h"
#include "Database/DBCStructure.h"
#include "Database/DatabaseEnv.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cmath>
#include <memory>
#include <functional>
#include <vector>


namespace
{
// TW-014 (KAP-557): the companion holds this range around its owner.
const float kFollowRange = 2.0f;

// PORT-007 (KAP-558): a corpse loot attempt is bounded by this window (ms) so
// a denied or unreachable corpse cannot trap the companion; on expiry the
// prior order (follow) resumes. Phase-1 named constant; per-entry config
// is future work.
uint32 const kLootWindowMs = 20000;
// PORT-008 (KAP-558): bounded pursuit. While the companion chases a valid
// target it cannot land a melee hit on, the pursuit is budgeted to this
// many ms; on expiry the pursuit is abandoned and the prior order resumes.
// A target within melee reach disarms the budget, so an actual fight has
// unbounded kill time. Phase-1 named constant; per-entry config is
// future work.
// PORT-009 (KAP-558): while a dead companion cannot yet reclaim its
// corpse (no corpse, reclaim delay not over, or out of range), the
// recovery state is reported at most this often (ms).
uint32 const kRecoveryReportMs = 30000;
// PORT-009 (KAP-558): while out of range of its own corpse, the
// companion re-issues the path to the corpse only when its motion is
// empty and at least this many ms have passed since the last issue.
uint32 const kRecoveryWalkRetryMs = 5000;
// PORT-008 (KAP-558): while the companion keeps walking toward the owner,
// the follow path is re-issued at most this often (ms) unless the owner
// moved further than 2.0 yd (2D) from the last issued target.
uint32 const kFollowPathRefreshMs = 5000;

// MVP-006: nearest alive creature offering the declared quest within a
// bounded radius; the search range shrinks as closer matches are found.
class NearestQuestGiverCheck
{
public:
    NearestQuestGiverCheck(Player const* obj, uint32 questId, float maxRange)
        : i_obj(obj), i_questId(questId), i_range(maxRange) {}
    WorldObject const& GetFocusObject() const { return *i_obj; }
    bool operator()(Creature const* u)
    {
        if (!u->IsAlive() || !u->HasQuest(i_questId))
            return false;
        if (!i_obj->IsWithinDistInMap(u, i_range))
            return false;
        i_range = i_obj->GetDistance(u);
        return true;
    }
    float GetLastRange() const { return i_range; }
private:
    Player const* const i_obj;
    uint32 i_questId;
    float i_range;
    NearestQuestGiverCheck(NearestQuestGiverCheck const&);
};

// PORT-006 (KAP-558): defend candidate scan. A creature qualifies only
// while it is actually attacking - its current victim is the follow
// leader (the owner) or this companion. Neutrals, bystanders and
// unengaged creatures are never pulled. Deterministic selection: an
// owner attacker beats a companion attacker, then nearest to the
// companion, then lowest GUID.
float const kDefendSearchRange = 30.0f;
// PORT-006: a locked defend target survives this long after the last
// confirmed candidate. The owner's melee state flaps between the 2 s
// legacy combat checks, so the attacker's victim can read null on a
// scan tick even mid-fight; the grace keeps the engagement alive until
// the companion's own swing connects (verified by the run4 probe).
uint32 const kDefendTargetGraceMs = 5000;

// PORT-005 (KAP-558): defend legality. The faction masks can read an
// active attacker as neutral (Turtle faction data: template 32 vs
// player template 1), yet the player melee path gates only on
// IsFriendlyTo and Unit::Attack performs no hostility check, so the
// owner can fight such a mob. A creature with real attack evidence on
// the owner or this companion is defendable even when IsHostileTo is
// false; bystanders stay excluded because only creatures that actually
// hit one of ours carry that evidence (victim pointer or
// attacker-set membership), and stale evidence decays when the fight
// ends.
// PORT-012 (KAP-558): the defend legality predicate is the module's
// snapshot predicate; this fill keeps the defend scan and the shared
// executor snapshot on one rule set.
static Companion::Combat::TargetSnapshot DefendSnapshot(Unit const* me, Unit const* owner, Creature const* u)
{
    Companion::Combat::TargetSnapshot s;
    s.exists = true;
    s.alive = u->IsAlive();
    s.inWorld = u->IsInWorld();
    s.friendly = me->IsFriendlyTo(u);
    s.canAttack = me->CanAttack(u);
    s.targetable = u->IsTargetable(true, me->IsCharmerOrOwnerPlayerOrPlayerItself());
    Unit const* const victim = u->GetVictim();
    s.victimIsProtected = victim == me || (owner && victim == owner);
    s.attackingMe = u->GetAttackers().count(const_cast<Unit*>(me)) != 0;
    s.attackingOwner = owner != nullptr && u->GetAttackers().count(const_cast<Unit*>(owner)) != 0;
    return s;
}

static bool DefendTargetLegal(Unit const* me, Unit const* owner, Creature const* u)
{
    return Companion::Combat::DefendTargetLegal(DefendSnapshot(me, owner, u));
}

class BotDefendScan
{
public:
    BotDefendScan(Unit const* source, Unit const* owner, bool verbose = false)
        : me(source), owner(owner), m_best(nullptr), m_verbose(verbose) {}

    bool operator()(Creature* u)
    {
        if (u == me || !u->IsAlive())
            return false;
        Unit const* const victim = u->GetVictim();
        // Candidate evidence, in priority order: the creature's victim is
        // the owner or this companion, or the owner / companion is still in
        // the creature's attacker set while its victim state flaps between
        // the owner's melee swings. Only creatures that have actually hit
        // one of ours ever enter the attacker set, so bystanders stay
        // excluded.
        bool const attacksOwner = owner != nullptr && victim == owner;
        bool const attacksMe = victim == me;
        bool const hitsOwner = owner != nullptr &&
            u->GetAttackers().count(const_cast<Unit*>(owner)) != 0;
        bool const hitsMe = u->GetAttackers().count(const_cast<Unit*>(me)) != 0;
        if (!attacksOwner && !attacksMe && !hitsOwner && !hitsMe)
            return false;
        if (!u->IsWithinDistInMap(me, kDefendSearchRange, false, SizeFactor::None))
            return false;
        if (!DefendTargetLegal(me, owner, u))
        {
            // Diagnostic (verbose only): a candidate with real attack
            // evidence that fails the legality gate is otherwise silent.
            if (m_verbose)
                sLog.outString("[PlayerBot][Defend] reject GUID:%u target:%u friendly:%u canatk:%u tflags:%u tfaction:%u bfaction:%u dist:%.2f",
                                me->GetGUIDLow(), u->GetGUIDLow(),
                                (uint32)me->IsFriendlyTo(u), (uint32)me->CanAttack(u),
                                u->GetUInt32Value(UNIT_FIELD_FLAGS),
                                u->GetFactionTemplateId(), me->GetFactionTemplateId(),
                                me->GetDistance(u));
            return false;
        }
        if (!m_best)
        {
            m_best = u;
            return true;
        }
        int const cls = attacksOwner ? 0 : attacksMe ? 1 : hitsOwner ? 2 : 3;
        Unit const* const bestVictim = m_best->GetVictim();
        int const clsBest =
            (owner && bestVictim == owner) ? 0 :
            bestVictim == me ? 1 :
            (owner && m_best->GetAttackers().count(const_cast<Unit*>(owner)) != 0) ? 2 : 3;
        float const d = me->GetDistance(u);
        float const dBest = me->GetDistance(m_best);
        if (cls < clsBest ||
            (cls == clsBest && (d < dBest ||
             (d == dBest && u->GetGUIDLow() < m_best->GetGUIDLow()))))
            m_best = u;
        return true;
    }

    Creature* Best() const { return m_best; }

private:
    BotDefendScan(BotDefendScan const&);
    Unit const* me;
    Unit const* owner;
    Creature* m_best;
    bool m_verbose;
};

class BotDefendProbe
{
public:
    BotDefendProbe(Unit const* source)
        : me(source), m_best(nullptr), m_dist(0.0f) {}

    bool operator()(Creature* u)
    {
        if (u == me || !u->IsAlive())
            return false;
        float const d = me->GetDistance(u);
        if (d > kDefendSearchRange)
            return false;
        if (!m_best || d < m_dist ||
            (d == m_dist && u->GetGUIDLow() < m_best->GetGUIDLow()))
        {
            m_best = u;
            m_dist = d;
        }
        return true;
    }

    Creature* Best() const { return m_best; }

private:
    BotDefendProbe(BotDefendProbe const&);
    Unit const* me;
    Creature* m_best;
    float m_dist;
};
}

bool PlayerBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    sess->LoginPlayer(entry->playerGUID);
    return true;
}

void PlayerBotAI::UpdateAI(const uint32 diff)
{
    // TW-008 (AC1): a newly constructed or detached controller has me == nullptr;
    // check before any dereference. The teleport-ack blocks below stay reachable
    // for a valid player mid-teleport (AC2).
    if (!me)
        return;

    if (me->IsBeingTeleportedNear())
    {
        WorldPacket data(MSG_MOVE_TELEPORT_ACK, 10);
        data << me->GetObjectGuid();
        data << uint32(0) << uint32(0);
        me->GetSession()->HandleMoveTeleportAckOpcode(data);
    }
    if (me->IsBeingTeleportedFar())
        me->GetSession()->HandleMoveWorldportAckOpcode();

    if (!me->IsInWorld())
        return;

    // PORT-009 (KAP-558): a socketless bot session can never answer the
    // first-logon racial cinematic (no client sends CMSG_CINEMATIC_DONE);
    // while watching, IsTargetable() is false for NPC attackers, so hostile
    // mobs would never retaliate. Dismiss it on the first tick.
    if (me->watching_cinematic_entry != 0)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] cinematic dismissed entry:%u GUID:%u",
                           me->watching_cinematic_entry, me->GetGUIDLow());
        me->CinematicEnd();
    }

    // Detect manual level changes in case GiveLevel hook missed
    if (_lastLevel != me->GetLevel())
    {
        _lastLevel = me->GetLevel();
        AutoLearnSpellsForLevel();
        AutoEquipForLevel();
    }

    // Bounded lab observability (MVP-005): alive-state transitions plus a
    // 10 s heartbeat, gated on PlayerBot debug logging.
    if (sPlayerBotMgr.IsDebugEnabled())
    {
        bool const alive = me->IsAlive();
        if (alive != _obsAlive)
        {
            _obsAlive = alive;
            sLog.outString("[PlayerBot] bot %s GUID:%u hp:%u/%u",
                           alive ? "alive" : "dead", me->GetGUIDLow(),
                           me->GetHealth(), me->GetMaxHealth());
        }
        if (_obsTimer <= diff)
        {
            _obsTimer = 10000;
            uint32 vguid = 0, vhp = 0, vmax = 0;
            if (Unit* victim = me->GetVictim())
            {
                vguid = victim->GetGUIDLow();
                vhp = victim->GetHealth();
                vmax = victim->GetMaxHealth();
            }
            sLog.outString("[PlayerBot] state GUID:%u map:%u pos:%.1f/%.1f/%.1f combat:%u victim:%u vhp:%u/%u mhp:%u/%u",
                           me->GetGUIDLow(), me->GetMapId(),
                           me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                           me->IsInCombat() ? 1 : 0, vguid, vhp, vmax,
                           me->GetHealth(), me->GetMaxHealth());
        }
        else
            _obsTimer -= diff;
    }

    // PORT-009 (KAP-558): while alive, UpdateRecovery only resets its
    // state (so the next death re-arms the death-ack); while dead it
    // runs the one normal recovery path (corpse reclaim) and everything else
    // stays dead-idle.
    if (UpdateRecovery(diff) || !me->IsAlive())
        return;

    if (UpdateCompanion(diff))
        return;

    if (TryLootDefeatedTarget())
        return;

    // MVP-006: declared quest state machine; while true the bot is
    // moving for the quest (accept or turn-in) and normal behavior is
    // skipped for this tick.
    if (UpdateQuestPhases(diff))
        return;

    // Ability usage timer
    if (_abilityTimer > diff)
        _abilityTimer -= diff;
    else
        _abilityTimer = 0;

    // Combat: hold the current target across ticks. A target can drop out
    // of SelectNearestTarget detection after an opening swing (reaction or
    // attackable-state change), so re-selecting it every tick makes the bot
    // give up mid-fight. Instead the bot pursues the remembered target
    // directly until it dies (looted via TryLootDefeatedTarget) or runs out
    // of hold range.
    if (_combatCheckTimer <= diff)
    {
        _combatCheckTimer = 2000;

        if (me->IsInCombat() && me->GetVictim())
        {
            // The core paired us with a victim (the mob hit back); fight it
            // and keep it as the loot candidate.
            Unit* victim = me->GetVictim();
            if (victim->GetTypeId() == TYPEID_UNIT)
                RememberCombatTarget(victim);
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] fighting GUID:%u victim:%u dist:%.2f",
                               me->GetGUIDLow(), victim->GetGUIDLow(), me->GetDistance(victim));
            if (!me->CanReachWithMeleeAutoAttack(victim))
                me->GetMotionMaster()->MoveChase(victim);
            else
                me->SetFacingToObject(victim);

            if (_abilityTimer == 0)
                TryOffensiveCastOrAttack(victim);
            else
                me->Attack(victim, true);
        }
        else if (Creature* held = GetAliveHeldTarget())
        {
            // A target we picked that stopped being returned by
            // SelectNearestTarget; keep pursuing it directly.
            if (me->GetDistance(held) > 35.0f)
            {
                ClearTarget();
            }
            else if (me->CanReachWithMeleeAutoAttack(held))
            {
                me->SetFacingToObject(held);
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] fighting GUID:%u victim:%u dist:%.2f hp:%u/%u",
                                   me->GetGUIDLow(), held->GetGUIDLow(), me->GetDistance(held),
                                   held->GetHealth(), held->GetMaxHealth());
                if (_abilityTimer == 0)
                    TryOffensiveCastOrAttack(held);
                else
                    me->Attack(held, true);
            }
            else
            {
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] pursuing GUID:%u target:%u dist:%.2f hp:%u/%u",
                                   me->GetGUIDLow(), held->GetGUIDLow(), me->GetDistance(held),
                                   held->GetHealth(), held->GetMaxHealth());
                me->GetMotionMaster()->MoveChase(held);
                me->Attack(held, true);
            }
        }
        else
        {
            // KAP-558 hardening: an owned companion without an order never
            // autonomously acquires targets; the legacy auto-hunt
            // acquisition stays for ambient bots (ownerAccountId == 0).
            // It idles near its owner (owner-follow below) and fights
            // only via self-defense or explicit assist/defend orders.
            if (IsOwnedCompanion())
            {
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] no-order idle GUID:%u", me->GetGUIDLow());
            }
            else if (Unit* target = me->SelectNearestTarget(30.0f))
            {
                // Autonomous companions never initiate PvP. A hostile player
                // may still be the current victim while defending; this guard
                // applies only to idle target acquisition.
                if (!target->IsPlayer())
                {
                    if (target->GetTypeId() == TYPEID_UNIT)
                        RememberCombatTarget(target);
                    me->Attack(target, true);
                    me->GetMotionMaster()->MoveChase(target);
                    if (sPlayerBotMgr.IsDebugEnabled())
                        sLog.outString("[PlayerBot] engage GUID:%u target:%u dist:%.2f",
                                       me->GetGUIDLow(), target->GetGUIDLow(), me->GetDistance(target));
                }
                else if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] autonomous player target skipped GUID:%u target:%u",
                                   me->GetGUIDLow(), target->GetGUIDLow());
            }
            else if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] no-target GUID:%u", me->GetGUIDLow());
        }
    }
    else
        _combatCheckTimer -= diff;

    // Random wandering while idle.
    if (!me->IsInCombat())
    {
        if (_wanderTimer <= diff)
        {
            // A held or detected hostile is handled by the combat check
            // above; wandering here would overwrite the chase and lose the
            // target.
            if (IsOwnedCompanion())
            {
                // KAP-558 hardening: default owner-follow replaces the
                // auto-hunt wander. Close distance when the owner is far
                // on this map; otherwise hold position. Stays within
                // loot distance of its kills so the owner shares party
                // XP, and never roams into solo fights.
                Player* owner = FindOwnerByAccount();
                if (owner && owner->GetMapId() == me->GetMapId() &&
                    me->GetDistance(owner) > kOwnerFollowChaseDist)
                {
                    _wanderTimer = urand(1500, 3000);
                    me->GetMotionMaster()->MovePoint(0, owner->GetPositionX(),
                                                     owner->GetPositionY(), owner->GetPositionZ(),
                                                     MOVE_PATHFINDING);
                    if (sPlayerBotMgr.IsDebugEnabled())
                        sLog.outString("[PlayerBot] owner-follow GUID:%u owner:%u dist:%.2f",
                                       me->GetGUIDLow(), owner->GetGUIDLow(),
                                       me->GetDistance(owner));
                }
                else
                    _wanderTimer = urand(2000, 5000);
            }
            else if (GetAliveHeldTarget() || me->SelectNearestTarget(30.0f))
            {
                _wanderTimer = urand(2000, 4000);
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] wander-hold GUID:%u", me->GetGUIDLow());
            }
            else
            {
                _wanderTimer = urand(8000, 15000);

                float x = me->GetPositionX();
                float y = me->GetPositionY();
                float z = me->GetPositionZ();
                // PORT-007 (KAP-558): PlayerBot.WanderRadius clamps the
                // idle wander for lab fixtures (0 = legacy frand(8,20)).
                float const maxRadius = sPlayerBotMgr.GetWanderRadius();
                float radius = (maxRadius > 0.0f) ? frand(0.0f, maxRadius)
                                                  : frand(8.0f, 20.0f);

                if (Map* map = me->GetMap())
                {
                    if (map->GetWalkRandomPosition(nullptr, x, y, z, radius))
                    {
                        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
                        if (sPlayerBotMgr.IsDebugEnabled())
                            sLog.outString("[PlayerBot] wander GUID:%u to:%.1f/%.1f", me->GetGUIDLow(), x, y);
                    }
                }
            }
        }
        else
            _wanderTimer -= diff;
    }
}

Creature* PlayerBotAI::GetAliveHeldTarget() const
{
    // Hardening item 4 / PORT-012 (KAP-558): the held target is the
    // dedicated live slot; the corpse side lives in the corpse slot.
    if (_targets.live == 0 || !me || !me->GetMap())
        return nullptr;
    Creature* creature = me->GetMap()->GetCreature(ObjectGuid(_targets.live));
    if (creature && creature->IsAlive())
        return creature;
    return nullptr;
}

void PlayerBotAI::ClearTarget()
{
    // Full cleanup (owner orders: hold/stop/assist replacement, legacy
    // range drop): both slots and the loot attempt state. Combat-only
    // and loot-only releases use the explicit slot transitions instead
    // (PORT-012: neither may touch the other slot).
    _targets.ClearAll();
    _lootRetryCount = 0;
    _lootWindowMs = 0;
}

bool PlayerBotAI::TryLootDefeatedTarget()
{
    if (!me->GetMap())
        return false;
    // Hardening item 4 / PORT-012 (KAP-558): the live and loot-corpse
    // sides are explicit slots with named transitions: a held combat
    // target that died in between promotes to the pending loot corpse,
    // and a re-embodied corpse hands the target back to the combat side.
    if (_targets.live)
    {
        Creature* held = me->GetMap()->GetCreature(ObjectGuid(_targets.live));
        if (_targets.OnLiveResolved(held != nullptr, held != nullptr && held->IsAlive())
            == Companion::Combat::TargetSlots::Transition::LiveToCorpse)
            _lootRetryCount = 0;
    }
    if (!_targets.corpse)
        return false;
    Creature* creature = me->GetMap()->GetCreature(ObjectGuid(_targets.corpse));
    if (!creature)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] loot target missing GUID:%u target:%u",
                           me->GetGUIDLow(), (uint32)_targets.corpse);
        _targets.ReleaseCorpse();
        _lootRetryCount = 0;
        _lootWindowMs = 0;
        return false;
    }
    if (creature->IsAlive())
    {
        // Re-embodied: it is the held combat target again; the combat loop
        // keeps pursuing it. Do not clear it here.
        _targets.OnCorpseResolved(true, true);
        _lootWindowMs = 0;
        return false;
    }
    return CorpseLootStep(creature);
}
// PORT-007 (KAP-558): one bounded corpse-loot step against a known dead,
// in-world creature. Shared by the legacy fallback (TryLootDefeatedTarget)
// and the companion Loot intent (ExecuteLoot): walk into range, tap the
// corpse, auto-store the eligible loot, then release. A silent denial is
// retried a bounded number of times before giving up so the companion is
// never trapped. Every clear path resets the retry count and the loot window.
bool PlayerBotAI::CorpseLootStep(Creature* creature)
{
    if (!creature || !me->GetMap() || creature->IsAlive())
        return false;
    float const maxLootDist = me->GetMaxLootDistance(creature);
    if (!creature->IsWithinDistInMap(me, maxLootDist, true, SizeFactor::None))
    {
        me->GetMotionMaster()->MovePoint(0, creature->GetPositionX(), creature->GetPositionY(),
                                         creature->GetPositionZ(), MOVE_PATHFINDING);
        return true;
    }

    ObjectGuid const guid = creature->GetObjectGuid();
    me->SendLoot(guid, LOOT_CORPSE);
    if (me->GetLootGuid() == guid)
    {
        std::vector<std::pair<uint32, uint32>> itemCounts;
        uint32 const maxSlot = creature->loot.GetMaxSlotInLootFor(me->GetGUIDLow());
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] corpse loot slots GUID:%u target:%u slots:%u lootid:%u empty:%d",
                           me->GetGUIDLow(), guid.GetCounter(), maxSlot,
                           creature->GetLootId(), creature->loot.empty() ? 1 : 0);
        for (uint32 slot = 0; slot < maxSlot; ++slot)
            if (LootItem* item = creature->loot.LootItemInSlot(slot, me->GetGUIDLow()))
                itemCounts.push_back(std::make_pair(item->itemid, me->GetItemCount(item->itemid)));
        me->AutoStoreLoot(creature->loot);
        if (sPlayerBotMgr.IsDebugEnabled())
            for (std::vector<std::pair<uint32, uint32>>::const_iterator itr = itemCounts.begin(); itr != itemCounts.end(); ++itr)
                sLog.outString("[PlayerBot] corpse loot stored GUID:%u item:%u before:%u after:%u",
                               me->GetGUIDLow(), itr->first, itr->second, me->GetItemCount(itr->first));
        me->GetSession()->DoLootRelease(guid);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] corpse loot processed GUID:%u target:%u",
                           me->GetGUIDLow(), guid.GetCounter());
        _targets.ReleaseCorpse();
        _lootRetryCount = 0;
        _lootWindowMs = 0;
        return true;
    }

    // The core refuses corpse looting silently; log bounded diagnostics and
    // retry a few times so a transient denial can self-heal.
    uint32 const attempt = ++_lootRetryCount;
    if (sPlayerBotMgr.IsDebugEnabled() && (attempt <= 3 || attempt % 5 == 0))
        sLog.outString("[PlayerBot] corpse loot denied GUID:%u target:%u attempt:%u eligible:%d tapped:%u recipient:%u recipfound:%u slots:%u dist:%.2f maxd:%.2f",
                       me->GetGUIDLow(), guid.GetCounter(), attempt,
                       creature->IsLootAllowedDueToDamageOrigin() ? 1 : 0,
                       creature->IsTappedBy(me) ? 1 : 0,
                       creature->GetLootRecipientGuid().GetCounter(),
                       creature->GetLootRecipient() != nullptr ? 1 : 0,
                       creature->loot.GetMaxSlotInLootFor(me->GetGUIDLow()),
                       me->GetDistance(creature), maxLootDist);
    if (attempt >= 5)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] corpse loot giving up GUID:%u target:%u",
                           me->GetGUIDLow(), guid.GetCounter());
        _targets.ReleaseCorpse();
        _lootRetryCount = 0;
        _lootWindowMs = 0;
    }
    return true;
}

void PlayerBotAI::RememberCombatTarget(Unit* unit)
{
    // Hardening item 4 / PORT-012 (KAP-558): remembers the live combat
    // target in the live slot (the corpse side is a separate slot).
    if (!unit)
        return;
    uint64_t const guid = unit->GetObjectGuid().GetRawValue();
    if (_targets.AcquireLive(guid) != Companion::Combat::TargetSlots::Transition::LiveAcquired)
        return;
    _lootRetryCount = 0;
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot] loot target set GUID:%u target:%u entry:%u hp:%u/%u",
                       me->GetGUIDLow(), unit->GetGUIDLow(), unit->GetEntry(),
                       unit->GetHealth(), unit->GetMaxHealth());
}

void PlayerBotAI::InitQuestState()
{
    _questId = sPlayerBotMgr.GetQuestId();
    _questPhase = 0;
    _questGiverGuid = ObjectGuid();
    _questObjectiveGuid = ObjectGuid();
    _questScanTimer = 0;
    _questDenyCount = 0;
    if (!_questId || !me || !sObjectMgr.GetQuestTemplate(_questId))
        return;

    // Resume from the saved quest log so a restart mid-quest continues
    // instead of stalling (MVP-007 relies on this).
    QuestStatus status = me->GetQuestStatus(_questId);
    if (status == QUEST_STATUS_INCOMPLETE)
        _questPhase = 2;
    else if (status == QUEST_STATUS_COMPLETE)
        _questPhase = 3;
    else
        _questPhase = 1;
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot] quest init GUID:%u quest:%u phase:%u",
                       me->GetGUIDLow(), _questId, _questPhase);
}

Creature* PlayerBotAI::FindQuestGiver() const
{
    Creature* found = nullptr;
    NearestQuestGiverCheck check(me, _questId, 50.0f);
    MaNGOS::CreatureLastSearcher<NearestQuestGiverCheck> searcher(found, check);
    Cell::VisitGridObjects(me, searcher, 50.0f);
    if (!found && sPlayerBotMgr.IsDebugEnabled())
    {
        // MVP-006 debug: distinguish "no giver in grid" from "giver in
        // grid but not offering the quest" for the declared quest's giver
        // entry (2079, Conservator Ilthalaine).
        class GiverProbe
        {
        public:
            GiverProbe(Player const* obj, uint32 questId) : i_obj(obj), i_questId(questId) {}
            WorldObject const& GetFocusObject() const { return *i_obj; }
            bool operator()(Creature const* u)
            {
                if (u->GetEntry() == 2079)
                {
                    if (u->IsAlive() && u->HasQuest(i_questId))
                        ++i_withQuest;
                    if (u->IsAlive())
                        ++i_alive;
                    else
                        ++i_dead;
                }
                return false;
            }
            uint32 i_alive = 0;
            uint32 i_dead = 0;
            uint32 i_withQuest = 0;
            GiverProbe(GiverProbe const&);
        private:
            Player const* const i_obj;
            uint32 const i_questId;
        };
        GiverProbe probe(me, _questId);
        Creature* unused = nullptr;
        MaNGOS::CreatureLastSearcher<GiverProbe> probeSearcher(unused, probe);
        Cell::VisitGridObjects(me, probeSearcher, 50.0f);
        sLog.outString("[PlayerBot] quest giver search empty GUID:%u quest:%u alive2079:%u dead2079:%u withQuest:%u",
                       me->GetGUIDLow(), _questId, probe.i_alive, probe.i_dead, probe.i_withQuest);
    }
    return found;
}

Creature* PlayerBotAI::FindQuestObjectiveTarget() const
{
    if (_questPhase != 2)
        return nullptr;
    Quest const* qInfo = sObjectMgr.GetQuestTemplate(_questId);
    if (!qInfo)
        return nullptr;
    QuestStatusData const* qStatus = me->GetQuestStatusData(_questId);
    if (!qStatus || qStatus->m_status != QUEST_STATUS_INCOMPLETE)
        return nullptr;

    // Nearest alive creature matching an incomplete kill objective.
    class NearestQuestKillCheck
    {
    public:
        NearestQuestKillCheck(Player const* obj, Quest const* q, QuestStatusData const* status, float maxRange)
            : i_obj(obj), i_q(q), i_status(status), i_range(maxRange) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(Creature const* u)
        {
            if (!u->IsAlive())
                return false;
            int32 const entry = (int32)u->GetEntry();
            for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (i_q->ReqCreatureOrGOId[i] != entry)
                    continue;
                if (i_status->m_creatureOrGOcount[i] >= i_q->ReqCreatureOrGOCount[i])
                    continue;
                if (!i_obj->IsWithinDistInMap(u, i_range))
                    return false;
                i_range = i_obj->GetDistance(u);
                return true;
            }
            return false;
        }
        float GetLastRange() const { return i_range; }
    private:
        Player const* const i_obj;
        Quest const* const i_q;
        QuestStatusData const* const i_status;
        float i_range;
        NearestQuestKillCheck(NearestQuestKillCheck const&);
    };

    Creature* found = nullptr;
    NearestQuestKillCheck check(me, qInfo, qStatus, 40.0f);
    MaNGOS::CreatureLastSearcher<NearestQuestKillCheck> searcher(found, check);
    Cell::VisitGridObjects(me, searcher, 40.0f);
    return found;
}

bool PlayerBotAI::UpdateQuestPhases(uint32 diff)
{
    if (_questPhase == 0 || _questPhase == 4)
        return false;

    // Awaiting the objective: pursue incomplete quest kills through normal
    // combat (this also covers neutral objective creatures, e.g. quest 456's
    // Thistle Boars) and watch for completion so the bot turns in through
    // the normal quest APIs.
    if (_questPhase == 2)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
        {
            if (_questDebugTimer <= diff)
            {
                _questDebugTimer = 5000;
                if (QuestStatusData const* qStatus = me->GetQuestStatusData(_questId))
                {
                    Quest const* qInfo = sObjectMgr.GetQuestTemplate(_questId);
                    sLog.outString("[PlayerBot] quest state GUID:%u quest:%u status:%u c0:%u c1:%u c2:%u c3:%u cancomplete:%u incombat:%u flags:%u",
                                   me->GetGUIDLow(), _questId, qStatus->m_status,
                                   qStatus->m_creatureOrGOcount[0], qStatus->m_creatureOrGOcount[1],
                                   qStatus->m_creatureOrGOcount[2], qStatus->m_creatureOrGOcount[3],
                                   (uint32)me->CanCompleteQuest(_questId), (uint32)me->IsInCombat(),
                                   qInfo ? qInfo->GetSpecialFlags() : 0xFFFFFFFF);
                    if (qInfo)
                        sLog.outString("[PlayerBot] quest req GUID:%u quest:%u r0:%u/%u r1:%u/%u r2:%u/%u r3:%u/%u",
                                       me->GetGUIDLow(), _questId,
                                       qInfo->ReqCreatureOrGOId[0], qInfo->ReqCreatureOrGOCount[0],
                                       qInfo->ReqCreatureOrGOId[1], qInfo->ReqCreatureOrGOCount[1],
                                       qInfo->ReqCreatureOrGOId[2], qInfo->ReqCreatureOrGOCount[2],
                                       qInfo->ReqCreatureOrGOId[3], qInfo->ReqCreatureOrGOCount[3]);
                }
                else
                    sLog.outString("[PlayerBot] quest state GUID:%u quest:%u status:missing cancomplete:%u incombat:%u",
                                   me->GetGUIDLow(), _questId,
                                   (uint32)me->CanCompleteQuest(_questId), (uint32)me->IsInCombat());
            }
            else
                _questDebugTimer -= diff;
        }
        if (me->CanCompleteQuest(_questId) || me->GetQuestStatus(_questId) == QUEST_STATUS_COMPLETE)
        {
            _questPhase = 3;
            _questObjectiveGuid = ObjectGuid();
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] quest objective complete GUID:%u quest:%u",
                               me->GetGUIDLow(), _questId);
            return true;
        }
        if (!me->IsInCombat())
        {
            if (Creature* objective = FindQuestObjectiveTarget())
            {
                if (_questObjectiveGuid != objective->GetObjectGuid())
                {
                    _questObjectiveGuid = objective->GetObjectGuid();
                    if (sPlayerBotMgr.IsDebugEnabled())
                        sLog.outString("[PlayerBot] quest objective target GUID:%u quest:%u target:%u entry:%u",
                                       me->GetGUIDLow(), _questId,
                                       objective->GetGUIDLow(), objective->GetEntry());
                }
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] quest attack GUID:%u quest:%u target:%u dist:%.2f",
                                   me->GetGUIDLow(), _questId, objective->GetGUIDLow(),
                                   me->GetDistance(objective));
                me->Attack(objective, true);
                me->GetMotionMaster()->MoveChase(objective);
                return true;
            }
        }
        return false;
    }

    // Phases 1 (accept) and 3 (turn-in) need the giver object.
    Creature* giver = nullptr;
    if (_questGiverGuid && me->GetMap())
        giver = me->GetMap()->GetCreature(_questGiverGuid);
    if (!giver || !giver->IsAlive())
    {
        if (sPlayerBotMgr.IsDebugEnabled() && _questGiverGuid)
            sLog.outString("[PlayerBot] quest giver lost GUID:%u quest:%u guid:%u null:%u dead:%u",
                           me->GetGUIDLow(), _questId, _questGiverGuid.GetCounter(),
                           giver ? 0 : 1, (giver && !giver->IsAlive()) ? 1 : 0);
        _questGiverGuid = ObjectGuid();
        if (_questScanTimer <= diff)
        {
            _questScanTimer = 2000;
            if (Creature* found = FindQuestGiver())
            {
                _questGiverGuid = found->GetObjectGuid();
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] quest giver found GUID:%u quest:%u giver:%u entry:%u",
                                   me->GetGUIDLow(), _questId,
                                   found->GetGUIDLow(), found->GetEntry());
            }
        }
        else
            _questScanTimer -= diff;
        return false;
    }

    if (!me->IsWithinDistInMap(giver, 2.5f, true, SizeFactor::None))
    {
        // Player chase is a no-op unless the player has the chased
        // unit as victim (ChaseMovementGenerator::_lostTarget), and a
        // bot never attacks its quest giver. Path-find to the giver
        // position instead, the same pattern idle wander uses.
        me->GetMotionMaster()->MovePoint(0, giver->GetPositionX(), giver->GetPositionY(),
                                        giver->GetPositionZ(), MOVE_PATHFINDING);
        return true;
    }

    Quest const* qInfo = sObjectMgr.GetQuestTemplate(_questId);
    if (!qInfo)
    {
        _questPhase = 0;
        return false;
    }

    if (_questPhase == 1)
    {
        if (me->GetQuestStatus(_questId) == QUEST_STATUS_INCOMPLETE)
        {
            _questPhase = 2;
            return false;
        }
        if (me->GetQuestStatus(_questId) == QUEST_STATUS_COMPLETE)
        {
            _questPhase = 3;
            return false;
        }
        if (me->CanInteractWithQuestGiver(giver) && me->CanTakeQuest(qInfo, false) && me->CanAddQuest(qInfo, false))
        {
            me->AddQuest(qInfo, giver);
            // AddQuest returns void; only log acceptance if the quest
            // actually entered the log.
            if (me->GetQuestStatus(_questId) != QUEST_STATUS_NONE)
            {
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] quest accepted GUID:%u quest:%u",
                                   me->GetGUIDLow(), _questId);
                _questPhase = me->CanCompleteQuest(_questId) ? 3 : 2;
                return true;
            }
        }
        if (++_questDenyCount >= 5)
        {
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] quest accept denied GUID:%u quest:%u; disabling",
                               me->GetGUIDLow(), _questId);
            _questPhase = 0;
        }
        else if (sPlayerBotMgr.IsDebugEnabled() && _questDenyCount == 1)
            sLog.outString("[PlayerBot] quest accept denied GUID:%u quest:%u interact:%u take:%u add:%u",
                           me->GetGUIDLow(), _questId,
                           me->CanInteractWithQuestGiver(giver) ? 1 : 0,
                           me->CanTakeQuest(qInfo, false) ? 1 : 0,
                           me->CanAddQuest(qInfo, false) ? 1 : 0);
        return true;
    }

    // Phase 3: turn in.
    if (me->GetQuestStatus(_questId) != QUEST_STATUS_COMPLETE)
    {
        _questPhase = 2;
        return false;
    }
    if (me->CanInteractWithQuestGiver(giver))
    {
        me->CompleteQuest(_questId);
        if (me->CanRewardQuest(qInfo, false))
        {
            uint32 xpBefore = me->GetUInt32Value(PLAYER_XP);
            me->RewardQuest(qInfo, 0, giver, true);
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] quest rewarded GUID:%u quest:%u xpBefore:%u xpAfter:%u",
                               me->GetGUIDLow(), _questId, xpBefore,
                               me->GetUInt32Value(PLAYER_XP));
            _questPhase = 4;
        }
        else
        {
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] quest reward denied GUID:%u quest:%u",
                               me->GetGUIDLow(), _questId);
            _questPhase = 4;
        }
        return true;
    }
    return true;
}

void PlayerBotAI::OnPlayerLogin()
{
    _lastLevel = me ? me->GetLevel() : 0;
    AutoLearnSpellsForLevel();
    AutoEquipForLevel();
    InitQuestState();
}

void PlayerBotAI::OnLevelUp()
{
    _lastLevel = me ? me->GetLevel() : _lastLevel;
    AutoLearnSpellsForLevel();
    AutoEquipForLevel();
}

void PlayerBotAI::AutoLearnSpellsForLevel()
{
    if (!me)
        return;

    uint8 playerClass = me->GetClass();
    uint8 playerRace = me->GetRace();
    uint8 level = me->GetLevel();

    uint32 classMask = 1 << (playerClass - 1);
    uint32 raceMask = 1 << (playerRace - 1);

    uint32 maxSkillId = sObjectMgr.GetMaxSkillLineAbilityId();
    for (uint32 i = 0; i < maxSkillId; ++i)
    {
        SkillLineAbilityEntry const* ability = sObjectMgr.GetSkillLineAbility(i);
        if (!ability)
            continue;

        // Only class spells with matching masks
        if (!ability->classmask || !(ability->classmask & classMask))
            continue;
        if (ability->racemask && !(ability->racemask & raceMask))
            continue;

        // Skip tradeskills / profession gated spells
        if (ability->req_skill_value != 0)
            continue;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(ability->spellId);
        if (!spellInfo)
            continue;

        // Respect required level
        if (spellInfo->spellLevel && spellInfo->spellLevel > level)
            continue;

        if (me->HasSpell(ability->spellId))
            continue;

        me->LearnSpell(ability->spellId, false);
    }
}

// PORT-011/012 (KAP-558): the raw 1.12 SpellCastResult is mapped to
// the module's bounded rejection class; this is the only place that
// sees the engine enum (the vocabulary and its names live in
// Companion/Combat.h). An attempted cast is success only when the core
// reports SPELL_CAST_OK; every other value is a rejection in its
// category (or Other, with the raw value still logged).
static Companion::Combat::CastReject MapCastReject(uint32 res)
{
    switch (res)
    {
        case (uint32)SPELL_FAILED_NO_POWER:          return Companion::Combat::CastReject::InsufficientPower;
        case (uint32)SPELL_FAILED_OUT_OF_RANGE:
        case (uint32)SPELL_FAILED_LINE_OF_SIGHT:     return Companion::Combat::CastReject::RangeLos;
        case (uint32)SPELL_FAILED_NOT_READY:         return Companion::Combat::CastReject::Cooldown;
        case (uint32)SPELL_FAILED_NOT_SHAPESHIFT:
        case (uint32)SPELL_FAILED_ONLY_SHAPESHIFT:   return Companion::Combat::CastReject::StanceForm;
        case (uint32)SPELL_FAILED_NOT_INFRONT:
        case (uint32)SPELL_FAILED_NOT_BEHIND:
        case (uint32)SPELL_FAILED_UNIT_NOT_INFRONT:
        case (uint32)SPELL_FAILED_UNIT_NOT_BEHIND:   return Companion::Combat::CastReject::Facing;
        case (uint32)SPELL_FAILED_BAD_TARGETS:
        case (uint32)SPELL_FAILED_TARGETS_DEAD:
        case (uint32)SPELL_FAILED_TARGET_ENEMY:
        case (uint32)SPELL_FAILED_TARGET_FRIENDLY:
        case (uint32)SPELL_FAILED_TARGET_IS_PLAYER:
        case (uint32)SPELL_FAILED_TARGET_NOT_PLAYER:
        case (uint32)SPELL_FAILED_TARGET_NOT_DEAD:
        case (uint32)SPELL_FAILED_TARGET_IN_COMBAT:
        case (uint32)SPELL_FAILED_TARGET_FREEFORALL: return Companion::Combat::CastReject::InvalidTarget;
        default:                                     return Companion::Combat::CastReject::Other;
    }
}

// Hardening (KAP-558): one offensive evaluation step shared by the legacy
// continue-combat path and the companion assist/defend executors. Only a
// successful cast arms _abilityTimer; a failed cast (mana, cooldown, bad
// target) or no usable spell falls back to melee in the same evaluation,
// so a failed cast never leaves the companion idling unengaged.
// PORT-011 (KAP-558): diagnostics record three distinct bounded outcomes -
// no-eligible-ability, cast-accepted, cast-rejected (with category) - plus
// the ordinary-attack fallback. NoEligibleAbility is never a rejected
// cast: spell:0 plus an unknown-failure value never crosses the boundary as
// one. Behavior is unchanged by this card.
bool PlayerBotAI::TryOffensiveCastOrAttack(Unit* target)
{
    uint32 const spellId = SelectOffensiveSpell(target);
    if (!spellId)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Offense] no-eligible-ability GUID:%u t:%u fallback:ordinary-attack",
                           me->GetGUIDLow(), target->GetGUIDLow());
        me->Attack(target, true);
        return false;
    }
    SpellCastResult const castRes = me->CastSpell(target, spellId, false);
    Companion::Combat::CastReport const report =
        Companion::Combat::ReportCast(spellId, (uint32)castRes, MapCastReject((uint32)castRes));
    if (report.outcome == Companion::Combat::CastOutcome::Accepted)
    {
        _abilityTimer = urand(2000, 4000);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Offense] cast-accepted GUID:%u t:%u spell:%u",
                           me->GetGUIDLow(), target->GetGUIDLow(), spellId);
        return true;
    }
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Offense] cast-rejected GUID:%u t:%u spell:%u res:%u category:%s fallback:ordinary-attack",
                       me->GetGUIDLow(), target->GetGUIDLow(), spellId, (uint32)castRes,
                       Companion::Combat::CastRejectName(report.reject));
    me->Attack(target, true);
    return false;
}

// ---------------------------------------------------------------------------
// PORT-014 (KAP-558): one legal tank threat policy for the single declared
// build (the declared matrix is Companion/Tank.h). The policy runs only on
// the assist source (owner-selected pulls: the tank never autonomously
// acquires targets) and only for the declared build; every other companion
// keeps the ordinary offense path untouched. The taunt is the single
// declared ability; it is reported with the PORT-012 cast vocabulary and a
// rejected cast falls back to the ordinary attack in the same evaluation -
// never a delayed one.
// ---------------------------------------------------------------------------
bool PlayerBotAI::IsDeclaredTank() const
{
    if (!me)
        return false;
    return me->GetClass() == Companion::Tank::kDeclaredTankClass &&
           me->GetLevel() >= Companion::Tank::kDeclaredTankMinLevel &&
           me->HasSpell(Companion::Tank::kDeclaredTankTaunt);
}

Companion::Tank::Observation PlayerBotAI::FillTankObservation(Unit* target) const
{
    Companion::Tank::Observation o;
    if (!me || !target)
        return o;
    o.tankGuid = me->GetGUIDLow();
    o.ownerGuid = _followLeaderGuid;
    o.targetGuid = target->GetGUIDLow();
    if (Creature* c = target->ToCreature())
    {
        o.targetHasThreatList = c->CanHaveThreatList();
        if (o.targetHasThreatList)
        {
            o.tankThreat = (uint32)c->GetThreatManager().getThreat(me, false);
            if (Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, o.ownerGuid)))
                o.ownerThreat = (uint32)c->GetThreatManager().getThreat(owner, false);
            if (Unit* victim = c->GetVictim())
                o.victimGuid = victim->GetGUIDLow();
        }
    }
    o.tauntUsable = me->HasSpell(Companion::Tank::kDeclaredTankTaunt) &&
                    !me->HasSpellCooldown(Companion::Tank::kDeclaredTankTaunt) &&
                    me->IsWithinLOSInMap(target) &&
                    me->CanReachWithMeleeAutoAttack(target);
    return o;
}

void PlayerBotAI::TankTauntStep(Unit* target)
{
    uint32 const spellId = Companion::Tank::kDeclaredTankTaunt;
    SpellCastResult const res = me->CastSpell(target, spellId, false);
    Companion::Combat::CastReport const report =
        Companion::Combat::ReportCast(spellId, (uint32)res, MapCastReject((uint32)res));
    if (report.outcome == Companion::Combat::CastOutcome::Accepted)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Tank] taunt cast-accepted GUID:%u t:%u spell:%u",
                           me->GetGUIDLow(), target->GetGUIDLow(), spellId);
        return;
    }
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[Tank] taunt cast-rejected GUID:%u t:%u spell:%u res:%u category:%s fallback:ordinary-attack",
                       me->GetGUIDLow(), target->GetGUIDLow(), spellId, (uint32)res,
                       Companion::Combat::CastRejectName(report.reject));
    me->Attack(target, true);
}

// ---------------------------------------------------------------------------
// PORT-015 (KAP-558): one legal healer triage policy for the single
// declared build (the declared matrix is Companion/Healer.h). The
// policy runs on the companion tick while the behavior gate is not a
// hold: the card's "Hold" and "owner loss" failure cases suppress
// healing exactly like they suppress every other behavior. Every slot
// fact is re-resolved from the live world each tick, the decision is
// revalidated against live state immediately before the cast, and
// every attempt is reported through the PORT-012 cast outcome
// vocabulary. A rejected or blocked heal creates no false success and
// no delay: the next legal triage/follow outcome is the same
// evaluation.
// ---------------------------------------------------------------------------
bool PlayerBotAI::IsDeclaredHealer() const
{
    if (!me)
        return false;
    return me->GetClass() == Companion::Healer::kDeclaredHealerClass &&
           me->GetLevel() >= Companion::Healer::kDeclaredHealerMinLevel &&
           me->HasSpell(Companion::Healer::kDeclaredHealerHeal);
}

Companion::Healer::Observation PlayerBotAI::FillHealerObservation() const
{
    Companion::Healer::Observation o;
    if (!me || !me->GetMap())
        return o;
    o.mana = me->GetPower(POWER_MANA);
    o.maxMana = me->GetMaxPower(POWER_MANA);
    if (SpellEntry const* se = sSpellMgr.GetSpellEntry(Companion::Healer::kDeclaredHealerHeal))
        o.healRangeYd = (uint32)Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(se->rangeIndex));
    // withDelayed=true: an in-flight timed heal sits in
    // SPELL_STATE_DELAYED and this core's default form treats that as
    // not-cast; the delayed form is the authoritative busy fact.
    o.canCast = me->HasSpell(Companion::Healer::kDeclaredHealerHeal) &&
                !me->HasSpellCooldown(Companion::Healer::kDeclaredHealerHeal) &&
                !me->IsNonMeleeSpellCasted(true);
    // Slot 0 is self: trivially alive (UpdateAI gated the dead and
    // recovery paths), in range, and the declared priority fallback.
    // The remaining slots are the live party members re-resolved from
    // the group slots by GUID; no engine pointer is retained.
    o.slots[0].guid = me->GetGUIDLow();
    o.slots[0].hp = me->GetHealth();
    o.slots[0].maxHp = me->GetMaxHealth();
    o.slots[0].alive = me->IsAlive();
    o.slots[0].inLos = true;
    o.slots[0].distance = 0.0f;
    o.slots[0].isSelf = true;
    uint32 n = 1;
    if (Group* group = me->GetGroup())
    {
        for (auto const& slot : group->GetMemberSlots())
        {
            if (n >= Companion::Healer::kMaxSlots)
                break;
            Player* member = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, slot.guid.GetCounter()));
            if (!member || member == me)
                continue;
            o.slots[n].guid = member->GetGUIDLow();
            o.slots[n].hp = member->GetHealth();
            o.slots[n].maxHp = member->GetMaxHealth();
            o.slots[n].alive = member->IsAlive() && member->IsInWorld();
            // A member on another map (or out of line of sight) is
            // never castable through triage: the follow goal, not a
            // teleport, is what regains range.
            if (member->GetMapId() != me->GetMapId())
            {
                o.slots[n].inLos = false;
                o.slots[n].distance = 10000.0f;
            }
            else
            {
                o.slots[n].inLos = me->IsWithinLOSInMap(member);
                o.slots[n].distance = me->GetDistance(member);
            }
            o.slots[n].isOwner = (slot.guid.GetCounter() == _followLeaderGuid);
            ++n;
        }
    }
    return o;
}

void PlayerBotAI::HealerTriageStep(Companion::Healer::Observation const& obs,
                                   Companion::Healer::Decision const& decision)
{
    uint32 const spellId = Companion::Healer::kDeclaredHealerHeal;
    Unit* target = (decision.action == Companion::Healer::Action::SelfHeal)
                   ? me
                   : (me->GetMap() ? me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, decision.target)) : nullptr);
    // Revalidate the whole decision against live state immediately
    // before the cast: a stale slot, a map flap, a range/LOS change,
    // or a cooldown/mana flap is a bounded skip and the next tick
    // re-triages (no false success, no delayed retry of the same
    // decision).
    if (!target || !target->IsAlive() || !target->IsInWorld() ||
        target->GetMapId() != me->GetMapId() ||
        !me->IsWithinLOSInMap(target) ||
        me->GetDistance(target) >= Companion::Healer::kDeclaredHealRangeYd)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Healer] heal skipped target-stale GUID:%u t:%u",
                           me->GetGUIDLow(), decision.target);
        return;
    }
    uint32 const mana = me->GetPower(POWER_MANA);
    // The withDelayed=true form is the authoritative busy fact: an
    // in-flight timed heal sits in SPELL_STATE_DELAYED, and the default
    // form treats that as not-cast - an unguarded re-cast would replace
    // the in-flight cast (the re-accept storm the fixture pins against).
    if (me->IsNonMeleeSpellCasted(true))
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Healer] heal skipped reason:busy GUID:%u t:%u",
                           me->GetGUIDLow(), decision.target);
        return;
    }
    if (me->HasSpellCooldown(spellId))
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Healer] heal skipped reason:cooldown GUID:%u t:%u",
                           me->GetGUIDLow(), decision.target);
        return;
    }
    if (mana < obs.spellCost + obs.manaReserve)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Healer] heal skipped reason:mana-floor GUID:%u t:%u "
                           "mana:%u cost:%u reserve:%u",
                           me->GetGUIDLow(), decision.target, mana,
                           obs.spellCost, obs.manaReserve);
        return;
    }
    uint32 const manaBefore = me->GetPower(POWER_MANA);
    SpellCastResult const res = me->CastSpell(target, spellId, false);
    Companion::Combat::CastReport const report =
        Companion::Combat::ReportCast(spellId, (uint32)res, MapCastReject((uint32)res));
    if (report.outcome == Companion::Combat::CastOutcome::Accepted)
    {
        // The declared heal is cast-interruptible by movement
        // (interruptFlags includes SPELL_INTERRUPT_FLAG_MOVEMENT): stop the
        // walk in progress immediately so the spell's movement check sees a
        // stationary caster for the whole cast window; the follow goal
        // (gated on the in-flight cast) resumes the approach after it ends.
        me->GetMotionMaster()->Clear(false);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Healer] heal cast-accepted GUID:%u t:%u spell:%u mana-before:%u mana:%u/%u",
                           me->GetGUIDLow(), target->GetGUIDLow(), spellId, manaBefore,
                           me->GetPower(POWER_MANA), me->GetMaxPower(POWER_MANA));
        return;
    }
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[Healer] heal cast-rejected GUID:%u t:%u spell:%u res:%u category:%s",
                       me->GetGUIDLow(), target->GetGUIDLow(), spellId, (uint32)res,
                       Companion::Combat::CastRejectName(report.reject));
}

// ---------------------------------------------------------------------------
// PORT-016 (KAP-558): one deterministic damage policy for the single
// declared melee build (the declared matrix is Companion/Damage.h).
// The damage companion engages only the declared tank's established
// target, and only after the pull-ownership (tank threat) gate; while
// the target holds a controlling aura it neither engages nor keeps
// it (crowd-control preservation). The engagement runs through the
// shared combat executor (Source::Damage), the offense step is the
// common cast-or-attack path (the per-class table is the declared
// damage matrix), and every cast is reported through the PORT-012
// cast outcome vocabulary. Failure cases (missing tank, insufficient
// threat, CC, owner loss, unreachable, unsupported class) are
// bounded wait/follow; the policy never selects the nearest hostile.
// ---------------------------------------------------------------------------
bool PlayerBotAI::IsDeclaredDamage() const
{
    if (!me)
        return false;
    return me->GetClass() == Companion::Damage::kDeclaredDamageClass &&
           me->GetLevel() >= Companion::Damage::kDeclaredDamageMinLevel;
}

Unit* PlayerBotAI::FindDeclaredTank() const
{
    if (!me || !me->GetMap() || !me->GetGroup())
        return nullptr;
    Group* group = me->GetGroup();
    for (auto const& slot : group->GetMemberSlots())
    {
        Player* member = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, slot.guid.GetCounter()));
        if (!member || member == me || !member->IsAlive() || !member->IsInWorld())
            continue;
        if (member->GetClass() == Companion::Tank::kDeclaredTankClass &&
            member->GetLevel() >= Companion::Tank::kDeclaredTankMinLevel &&
            member->HasSpell(Companion::Tank::kDeclaredTankTaunt))
            return member;
    }
    return nullptr;
}

bool PlayerBotAI::TargetUnderCC(Unit* unit) const
{
    // PORT-016 declared preservation set (the pinned core AuraType
    // values listed in Companion/Damage.h): any controlling aura from
    // any source marks the target controlled; the damage source
    // waits/follows instead of dealing damage (the conservative
    // ambiguity resolution).
    if (!unit)
        return true;
    return unit->HasAuraType(SPELL_AURA_MOD_STUN) ||
           unit->HasAuraType(SPELL_AURA_MOD_ROOT) ||
           unit->HasAuraType(SPELL_AURA_MOD_CHARM) ||
           unit->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
           unit->HasAuraType(SPELL_AURA_MOD_FEAR) ||
           unit->HasAuraType(SPELL_AURA_MOD_PACIFY) ||
           unit->HasAuraType(SPELL_AURA_TRANSFORM) ||
           unit->HasAuraType(SPELL_AURA_FEIGN_DEATH);
}

Companion::Damage::Observation PlayerBotAI::FillDamageObservation() const
{
    Companion::Damage::Observation o;
    if (!me || !me->GetMap())
        return o;
    o.damageGuid = me->GetGUIDLow();
    o.held = _held;
    o.ownerAvailable = IsFollowOwnerAvailable();
    if (!o.ownerAvailable)
        return o;
    Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
    if (!owner)
        return o;
    Unit* tank = FindDeclaredTank();
    if (!tank)
        return o; // missing tank: bounded wait/follow
    o.tankGuid = tank->GetGUIDLow();
    Creature* target = tank->GetVictim() ? tank->GetVictim()->ToCreature() : nullptr;
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return o; // the tank holds no established target yet
    o.targetGuid = target->GetGUIDLow();
    o.targetRaw = target->GetObjectGuid().GetRawValue();
    o.distance = me->GetDistance(target);
    o.canAttack = me->CanAttack(target);
    o.inLos = me->IsWithinLOSInMap(target);
    o.targetUnderCC = TargetUnderCC(target);
    o.targetHasThreatList = target->CanHaveThreatList();
    if (o.targetHasThreatList)
    {
        o.tankThreat = (uint32)target->GetThreatManager().getThreat(tank, false);
        o.tankIsVictim = (target->GetVictim() == tank);
        if (!o.tankIsVictim)
        {
            Unit* victim = target->GetVictim();
            if (victim)
                o.victimThreat = (uint32)target->GetThreatManager().getThreat(victim, false);
        }
    }
    return o;
}

bool PlayerBotAI::IsEstablishedTankTarget(Creature* target) const
{
    if (!me || !target)
        return false;
    Unit* tank = FindDeclaredTank();
    if (!tank)
        return false;
    // Reuse the pure decision on a fresh value snapshot: the margin
    // math lives in Companion/Damage.h (no float, no duplication).
    Companion::Damage::Observation o;
    o.tankGuid = tank->GetGUIDLow();
    o.targetGuid = target->GetGUIDLow();
    o.targetRaw = target->GetObjectGuid().GetRawValue();
    o.targetHasThreatList = target->CanHaveThreatList();
    if (!o.targetHasThreatList)
        return false;
    o.tankIsVictim = (target->GetVictim() == tank);
    o.tankThreat = (uint32)target->GetThreatManager().getThreat(tank, false);
    if (!o.tankIsVictim)
    {
        Unit* victim = target->GetVictim();
        if (victim)
            o.victimThreat = (uint32)target->GetThreatManager().getThreat(victim, false);
    }
    return Companion::Damage::PullEstablished(o);
}

uint32 PlayerBotAI::SelectOffensiveSpell(Unit* target) const
{
    if (!target)
        return 0;

    struct Action
    {
        uint32 baseSpell;
        float minRange;
        float maxRange;
        bool meleeOnly;
        bool requiresMissingAura;
    };

    std::vector<Action> actions;
    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
            actions = {
                {100, 8.0f, 25.0f, false, false},   // Charge
                {7372, 0.f, 5.0f, true, true},      // Hamstring
                {772, 0.f, 5.0f, true, true},       // Rend
                {78, 0.f, 5.0f, true, false},       // Heroic Strike
            };
            break;
        case CLASS_PALADIN:
            actions = {
                {20271, 0.f, 10.0f, false, false},  // Judgement
                {879, 0.f, 10.0f, false, false},    // Exorcism
                {35395, 0.f, 5.0f, true, false},    // Crusader Strike (if present)
                {20467, 0.f, 10.0f, false, false},  // Judgement of Righteousness
            };
            break;
        case CLASS_HUNTER:
            actions = {
                {75, 5.0f, 35.0f, false, false},    // Auto Shot trigger
                {142, 5.0f, 35.0f, false, false},   // Arcane Shot
                {1978, 5.0f, 35.0f, false, true},   // Serpent Sting
                {2973, 0.f, 5.0f, true, false},     // Raptor Strike
            };
            break;
        case CLASS_ROGUE:
            actions = {
                {2098, 0.f, 5.0f, true, false},     // Eviscerate
                {1752, 0.f, 5.0f, true, false},     // Sinister Strike
                {703, 0.f, 5.0f, true, true},       // Garrote
            };
            break;
        case CLASS_PRIEST:
            actions = {
                {589, 0.f, 30.0f, false, true},     // Shadow Word: Pain
                {8092, 0.f, 30.0f, false, false},   // Mind Blast
                {585, 0.f, 30.0f, false, false},    // Smite
            };
            break;
        case CLASS_SHAMAN:
            actions = {
                {8050, 0.f, 20.0f, false, true},    // Flame Shock
                {403, 0.f, 30.0f, false, false},    // Lightning Bolt
                {8042, 0.f, 20.0f, false, false},   // Earth Shock
                {421, 0.f, 20.0f, false, false},    // Chain Lightning
            };
            break;
        case CLASS_MAGE:
            actions = {
                {116, 0.f, 30.0f, false, false},    // Frostbolt
                {133, 0.f, 30.0f, false, false},    // Fireball
                {2136, 0.f, 20.0f, false, false},   // Fire Blast
                {122, 0.f, 10.0f, false, true},     // Frost Nova
                {1449, 0.f, 10.0f, false, false},   // Arcane Explosion
            };
            break;
        case CLASS_WARLOCK:
            actions = {
                {172, 0.f, 30.0f, false, true},     // Corruption
                {348, 0.f, 30.0f, false, true},     // Immolate
                {980, 0.f, 30.0f, false, true},     // Curse of Agony
                {689, 0.f, 20.0f, false, false},    // Drain Life
                {686, 0.f, 30.0f, false, false},    // Shadow Bolt
            };
            break;
        case CLASS_DRUID:
            actions = {
                {8921, 0.f, 30.0f, false, true},    // Moonfire
                {5176, 0.f, 30.0f, false, false},   // Wrath
                {339, 0.f, 30.0f, false, true},     // Entangling Roots
                {1822, 0.f, 5.0f, true, true},      // Rake
                {5221, 0.f, 5.0f, true, false},     // Shred
            };
            break;
        default:
            break;
    }

    // PORT-012 (KAP-558): the known/usable split. The adapter fills
    // each profile with current engine facts; the pure selector
    // (Companion/Combat.h) decides what is currently executable and
    // why a known spell is not.
    Companion::Combat::AbilityQuery query;
    for (int i = 0; i < Companion::Combat::AbilityQuery::kPowerSlots; ++i)
        query.power[i] = me->GetPower(static_cast<Powers>(i));
    query.distance = me->GetCombatDistance(target);
    query.meleeReach = me->CanReachWithMeleeAutoAttack(target);

    std::vector<Companion::Combat::AbilityProfile> profiles;
    profiles.reserve(actions.size());
    for (auto const& act : actions)
    {
        Companion::Combat::AbilityProfile p;
        p.minRange = act.minRange;
        p.maxRange = act.maxRange;
        p.meleeOnly = act.meleeOnly;
        uint32 const spellId = GetHighestKnownSpell(act.baseSpell);
        if (!spellId || !me->HasSpell(spellId))
        {
            profiles.push_back(p); // not known: not scanned
            continue;
        }
        SpellEntry const* const info = sSpellMgr.GetSpellEntry(spellId);
        if (!info)
        {
            profiles.push_back(p); // no entry: not scanned
            continue;
        }
        p.known = true;
        p.id = spellId;
        p.passive = info->IsPassiveSpell();
        // obsoleteRank is structurally false here: GetHighestKnownSpell
        // already resolves the highest known rank of the chain; the
        // block stays in the model for fixed-rank future rotations.
        // Hardening/PORT-012 (KAP-558): on-next-swing spells occupy the
        // melee spell slot; after their trigger swing, every later
        // auto-attack is consumed by the swing-spell path and no white
        // damage lands for the rest of the engagement (evidence: assist
        // lab runs 2026-09-15). They are explicitly unsupported (see the
        // audit in Companion/Combat.h); the companion keeps pure white
        // damage instead of queueing them.
        p.onNextSwing = info->HasAttribute(SPELL_ATTR_ON_NEXT_SWING_1) ||
                        info->HasAttribute(SPELL_ATTR_ON_NEXT_SWING_2);
        p.powerType = static_cast<uint8_t>(info->powerType);
        p.powerCost = info->manaCost;
        p.onCooldown = me->HasSpellCooldown(spellId);
        // 1.12 has no player stance state and the core does not enforce
        // the spell Stances masks for players; the block stays in the
        // model for future trees.
        p.stanceOk = true;
        p.reagentOk = true;
        for (int i = 0; i < MAX_SPELL_REAGENTS && p.reagentOk; ++i)
            if (info->ReagentCount[i] && me->GetItemCount(info->Reagent[i]) < info->ReagentCount[i])
                p.reagentOk = false;
        p.targetOk = !(act.requiresMissingAura && TargetHasAuraFromChain(target, spellId));
        profiles.push_back(p);
    }

    return Companion::Combat::SelectExecutable(profiles, query).selected;
}

uint32 PlayerBotAI::GetHighestKnownSpell(uint32 spellId) const
{
    uint32 first = sSpellMgr.GetFirstSpellInChain(spellId);
    uint32 best = 0;
    SpellChainMapNext const& nextMap = sSpellMgr.GetSpellChainNext();

    std::function<void(uint32)> dfs = [&](uint32 id)
    {
        if (me->HasSpell(id))
            best = id;
        auto range = nextMap.equal_range(id);
        for (auto itr = range.first; itr != range.second; ++itr)
            dfs(itr->second);
    };

    dfs(first);
    return best;
}

bool PlayerBotAI::TargetHasAuraFromChain(Unit* target, uint32 spellId) const
{
    if (!target)
        return false;

    uint32 first = sSpellMgr.GetFirstSpellInChain(spellId);
    SpellChainMapNext const& nextMap = sSpellMgr.GetSpellChainNext();

    std::function<bool(uint32)> dfs = [&](uint32 id) -> bool
    {
        if (target->HasAura(id))
            return true;
        auto range = nextMap.equal_range(id);
        for (auto itr = range.first; itr != range.second; ++itr)
            if (dfs(itr->second))
                return true;
        return false;
    };

    return dfs(first);
}

namespace
{
    std::once_flag g_sourceInitFlag;
    std::unordered_set<uint32> g_vendorItems;
    std::unordered_set<uint32> g_lootItems;
    std::unordered_set<uint32> g_questRewardItems;

    void InitSourceCaches()
    {
        // vendor items
        std::unique_ptr<QueryResult> res(WorldDatabase.Query("SELECT item FROM npc_vendor"));
        if (res)
            do { g_vendorItems.insert(res->Fetch()->GetUInt32()); } while (res->NextRow());

        // loot tables (direct items)
        auto addLoot = [](char const* table)
        {
            std::unique_ptr<QueryResult> r(WorldDatabase.PQuery("SELECT item FROM %s", table));
            if (!r)
                return;
            do { g_lootItems.insert(r->Fetch()->GetUInt32()); } while (r->NextRow());
        };
        addLoot("creature_loot_template");
        addLoot("gameobject_loot_template");
        addLoot("item_loot_template");
        addLoot("pickpocketing_loot_template");
        addLoot("skinning_loot_template");
        addLoot("fishing_loot_template");
        addLoot("disenchant_loot_template");
        addLoot("reference_loot_template");

        // quest rewards
        ObjectMgr::QuestMap const& quests = sObjectMgr.GetQuestTemplates();
        for (auto const& pair : quests)
        {
            Quest const* quest = pair.second.get();
            if (!quest)
                continue;

            for (uint32 i = 0; i < quest->GetRewItemsCount(); ++i)
                if (quest->RewItemId[i])
                    g_questRewardItems.insert(quest->RewItemId[i]);

            for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
                if (quest->RewChoiceItemId[i])
                    g_questRewardItems.insert(quest->RewChoiceItemId[i]);
        }
    }

    bool IsAllowedSource(ItemPrototype const& proto)
    {
        std::call_once(g_sourceInitFlag, InitSourceCaches);

        // crafted (requires skill or spell)
        if (proto.RequiredSkill || proto.RequiredSpell)
            return true;

        if (g_questRewardItems.count(proto.ItemId))
            return true;

        if (g_vendorItems.count(proto.ItemId))
            return true;

        if (g_lootItems.count(proto.ItemId))
            return true;

        return false;
    }
}

void PlayerBotAI::AutoEquipForLevel()
{
    if (!me)
        return;

    uint8 level = me->GetLevel();
    uint32 classMask = 1 << (me->GetClass() - 1);
    uint32 raceMask = 1 << (me->GetRace() - 1);

    auto CanEquipArmorSubclass = [&](ItemPrototype const& proto) -> bool
    {
        switch (me->GetClass())
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
                if (proto.SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                    return true;
                if (level >= 40)
                    return proto.SubClass == ITEM_SUBCLASS_ARMOR_PLATE || proto.InventoryType == INVTYPE_CLOAK;
                return proto.SubClass == ITEM_SUBCLASS_ARMOR_MAIL || proto.InventoryType == INVTYPE_CLOAK;
            case CLASS_HUNTER:
                if (level >= 40)
                    return proto.SubClass == ITEM_SUBCLASS_ARMOR_MAIL || proto.InventoryType == INVTYPE_CLOAK;
                return proto.SubClass == ITEM_SUBCLASS_ARMOR_LEATHER || proto.InventoryType == INVTYPE_CLOAK;
            case CLASS_SHAMAN:
                if (proto.SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                    return true;
                if (level >= 40)
                    return proto.SubClass == ITEM_SUBCLASS_ARMOR_MAIL || proto.InventoryType == INVTYPE_CLOAK;
                return proto.SubClass == ITEM_SUBCLASS_ARMOR_LEATHER || proto.InventoryType == INVTYPE_CLOAK;
            case CLASS_DRUID:
            case CLASS_ROGUE:
                return proto.SubClass == ITEM_SUBCLASS_ARMOR_LEATHER || proto.InventoryType == INVTYPE_CLOAK;
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                return proto.SubClass == ITEM_SUBCLASS_ARMOR_CLOTH || proto.InventoryType == INVTYPE_CLOAK;
            default:
                return true;
        }
    };

    auto CanEquipWeaponSubclass = [&](ItemPrototype const& proto) -> bool
    {
        switch (me->GetClass())
        {
            case CLASS_PRIEST:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_STAFF || proto.SubClass == ITEM_SUBCLASS_WEAPON_WAND || proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE;
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_STAFF || proto.SubClass == ITEM_SUBCLASS_WEAPON_WAND || proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD;
            case CLASS_WARRIOR:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD || proto.SubClass == ITEM_SUBCLASS_WEAPON_GUN || proto.SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_BOW || proto.SubClass == ITEM_SUBCLASS_WEAPON_AXE || proto.SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_THROWN || proto.SubClass == ITEM_SUBCLASS_WEAPON_POLEARM;
            case CLASS_PALADIN:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE || proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD;
            case CLASS_SHAMAN:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE || proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_STAFF ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_AXE || proto.SubClass == ITEM_SUBCLASS_WEAPON_AXE2;
            case CLASS_DRUID:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE || proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_DAGGER ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_STAFF || proto.SubClass == ITEM_SUBCLASS_WEAPON_POLEARM;
            case CLASS_HUNTER:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_AXE2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD2 || proto.SubClass == ITEM_SUBCLASS_WEAPON_GUN ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW || proto.SubClass == ITEM_SUBCLASS_WEAPON_BOW || proto.SubClass == ITEM_SUBCLASS_WEAPON_POLEARM;
            case CLASS_ROGUE:
                return proto.SubClass == ITEM_SUBCLASS_WEAPON_DAGGER || proto.SubClass == ITEM_SUBCLASS_WEAPON_SWORD || proto.SubClass == ITEM_SUBCLASS_WEAPON_MACE ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_GUN || proto.SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW || proto.SubClass == ITEM_SUBCLASS_WEAPON_BOW ||
                       proto.SubClass == ITEM_SUBCLASS_WEAPON_THROWN || proto.SubClass == ITEM_SUBCLASS_WEAPON_AXE;
            default:
                return true;
        }
    };


    struct GearChoice
    {
        ItemPrototype const* proto;
        uint32 score;
    };

    std::unordered_map<uint16, GearChoice> bestBySlot;

    for (auto const& pair : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& proto = pair.second;

        if (proto.Class != ITEM_CLASS_ARMOR && proto.Class != ITEM_CLASS_WEAPON)
            continue;

        if (!IsAllowedSource(proto))
            continue;

        if (proto.RequiredLevel > level)
            continue;

        // avoid items too far below current level if required level known
        uint32 effectiveLevel = proto.RequiredLevel;
        if (!effectiveLevel)
        {
            // crude estimate: item level maps roughly to 2/3 of required level
            effectiveLevel = proto.ItemLevel ? std::max<uint32>(1, (proto.ItemLevel * 2) / 3) : level;
        }
        if (std::abs(int(level) - int(effectiveLevel)) > int(_gearMaxDiff))
            continue;

        if (proto.RequiredSkill || proto.RequiredSpell || proto.RequiredHonorRank || proto.RequiredCityRank || proto.RequiredReputationRank)
            continue;

        if (proto.AllowableClass && !(proto.AllowableClass & classMask))
            continue;
        if (proto.AllowableRace && !(proto.AllowableRace & raceMask))
            continue;

        if (proto.Class == ITEM_CLASS_ARMOR && !CanEquipArmorSubclass(proto))
            continue;
        if (proto.Class == ITEM_CLASS_WEAPON && !CanEquipWeaponSubclass(proto))
            continue;

        uint16 dest = NULL_SLOT;
        InventoryResult res = me->CanEquipItem(NULL_SLOT, dest, &proto, nullptr, true);
        if (res != EQUIP_ERR_OK || dest == NULL_SLOT)
            continue;

        // Prefer items near current level (effective level), then quality, then item level
        uint32 score = effectiveLevel * 1000 + proto.Quality * 10 + proto.ItemLevel;

        auto it = bestBySlot.find(dest);
        if (it != bestBySlot.end() && it->second.score >= score)
            continue;

        bestBySlot[dest] = { &proto, score };
    }

    for (auto const& it : bestBySlot)
    {
        uint16 dest = it.first;
        ItemPrototype const* proto = it.second.proto;
        if (!proto)
            continue;

        uint8 bag = dest >> 8;
        uint8 slot = dest & 255;
        if (Item* existing = me->GetItemByPos(bag, slot))
        {
            if (const ItemPrototype* existingProto = existing->GetProto())
            {
                if (existingProto->ItemLevel >= proto->ItemLevel)
                    continue;
            }
            me->AutoUnequipItemFromSlot(slot, false);
        }

        me->EquipNewItem(dest, proto->ItemId, true);
    }
}

void PlayerBotAI::Remove()
{
    // TW-008 (AC1): tolerate double removal of a detached controller.
    if (!me)
        return;
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot] AI removed GUID:%u", me->GetGUIDLow());
    // PORT-018 (KAP-558): our departure is a party-session change; kill
    // any outstanding round we tracked so a stale offer cannot cross a
    // membership boundary.
    if (_plannerLeaderGuid)
        sPlayerBotMgr.PlannerTransport().InvalidateSession(_plannerLeaderGuid);
    _plannerGroupId = 0;
    _plannerLeaderGuid = 0;
    _plannerLastSubmitMs = 0;
    _plannerOfferValid = false;
    me->setAI(nullptr);
    me = nullptr;
}

void PlayerBotFleeingAI::OnPlayerLogin()
{
    me->GetMotionMaster()->MoveFleeing(me);
    me->SetInvincibilityHpThreshold(1);
}

/// MageOrgrimmarAttackerAI event
enum
{
    SPELL_FROST_NOVA = 122,
    SPELL_FIREBOLT = 133,
    AURA_REGEN_MANA = 430,
};


bool PlayerBotAI::SpawnNewPlayer(WorldSession* sess, uint8 class_, uint32 race_, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
{
    ASSERT(botEntry);
    std::string name = sObjectMgr.GeneratePetName(1863); // Succubus name
    normalizePlayerName(name);
    uint8 gender = urand(0, 1);
    uint8 skin = urand(0, 5);
    uint8 face = urand(0, 5);
    uint8 hairStyle = urand(0, 5);
    uint8 hairColor = urand(0, 5);
    uint8 facialHair = urand(0, 5);
    Player *newChar = new Player(sess);
    uint32 guid = botEntry->playerGUID;
    if (!newChar->Create(guid, name, race_, class_, gender, skin, face, hairStyle, hairColor, facialHair))
    {
        sLog.outError("PlayerBotAI::SpawnNewPlayer: Unable to create a player!");
        delete newChar;
        return false;
    }
    newChar->SetLocationMapId(mapId);
    newChar->SetLocationInstanceId(instanceId);
    newChar->GetMotionMaster()->Initialize();
    newChar->SetCinematic(1);
    // Set instance
    if (instanceId && mapId > 1) // Not a continent
    {
        DungeonPersistentState *state = (DungeonPersistentState*)sMapPersistentStateMgr
                .AddPersistentState(sMapStorage.LookupEntry<MapEntry>(mapId), instanceId, time(nullptr) + 3600, false, true);
        newChar->BindToInstance(state, true, true);
    }
    // Generate position
    Map* map = sMapMgr.FindMap(mapId, instanceId);
    if (!map)
    {
        sLog.outError("PlayerBotAI::SpawnNewPlayer: Map (%u, %u) not found!", mapId, instanceId);
        delete newChar;
        return false;
    }
    newChar->Relocate(x, y, z, o);
    sObjectMgr.InsertPlayerInCache(newChar);
    newChar->SetMap(map);
    newChar->SaveRecallPosition();
    newChar->CreatePacketBroadcaster();
    MasterPlayer* mPlayer = new MasterPlayer(sess);
    mPlayer->LoadPlayer(newChar);
    mPlayer->SetSocial(sSocialMgr.LoadFromDB(nullptr, newChar->GetObjectGuid()));
    if (!newChar->GetMap()->Add(newChar))
    {
        sLog.outError("PlayerBotAI::SpawnNewPlayer: Unable to add player to map!");
        delete newChar;
        return false;
    }
    sess->SetPlayer(newChar);
    sess->SetMasterPlayer(mPlayer);
    sObjectAccessor.AddObject(newChar);
    newChar->SetCanModifyStats(true);
    newChar->UpdateAllStats();
    return true;
}
bool MageOrgrimmarAttackerAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    return SpawnNewPlayer(sess, CLASS_MAGE, RACE_GNOME, 1, 0, 1017.0f, -4450, 12, 0.65f);
}

void MageOrgrimmarAttackerAI::UpdateAI(const uint32 diff)
{
    // TW-008 (AC1): keep the override null-safe for a detached controller.
    if (!me)
        return;
    PlayerBotAI::UpdateAI(diff);
    if (me->GetLevel() != 60)
        me->GiveLevel(60);
    /// DEATH
    if (!me->IsAlive())
    {
        sPlayerBotMgr.DeleteBot(me->GetGUIDLow());
        return;
    }
    /// COMBAT AI
    if (me->IsNonMeleeSpellCasted(false) || (me->HasAura(AURA_REGEN_MANA) && me->GetPower(POWER_MANA) != me->GetMaxPower(POWER_MANA)))
        return;
    float range = me->IsInCombat() ? 30.0f : frand(15, 30);
    Unit* target = me->SelectNearestTarget(range);
    if (target && !me->IsWithinLOSInMap(target))
        target = nullptr;
    // OOM ?
    if (me->GetPower(POWER_MANA) < 40 && target && me->IsInCombat())
    {
        if (me->Attack(target, true))
            me->GetMotionMaster()->MoveChase(target);
        return;
    }
    // Stop chase if has mana
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        me->GetMotionMaster()->MovementExpired();
    bool nearTarget = target && target->CanReachWithMeleeAutoAttack(me);
    if (!me->HasSpellCooldown(SPELL_FROST_NOVA) && me->GetPower(POWER_MANA) > 50)
        if (nearTarget)
            me->CastSpell(me, SPELL_FROST_NOVA, false);
    if (nearTarget && target->HasUnitState(UNIT_STAT_CAN_NOT_MOVE))
    {
        // already runing
        if (!me->movespline->Finalized())
            return;
        // Try to kit
        float x, y, z;
        me->GetPosition(x, y, z);
        float d = me->GetDistance(target);
        d += me->GetObjectBoundingRadius();
        d += target->GetObjectBoundingRadius();
        x += (x - target->GetPositionX()) * 5.0f / d;
        y += (y - target->GetPositionY()) * 5.0f / d;
        me->UpdateGroundPositionZ(x, y, z);
        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
        return;
    }

    if (target && me->GetPower(POWER_MANA) > 50)
    {
        uint32 spellId = SPELL_FIREBOLT;
        me->SetFacingToObject(target);
        if (!me->movespline->Finalized())
            me->StopMoving();

        /*float z = me->GetPositionZ();
        me->UpdateGroundPositionZ(me->GetPositionX(), me->GetPositionY(), z);
        me->Relocate(me->GetPositionX(), me->GetPositionY(), z);
        me->m_movementInfo.moveFlags = 0;
        me->SendHeartBeat();*/

        me->CastSpell(target, spellId, false);
        return;
    }
    /// OUT OF COMBAT REGEN
    if (!me->IsInCombat() && me->GetPower(POWER_MANA) < 150)
    {
        if (!me->movespline->Finalized())
            me->StopMoving();
        me->CastSpell(target, AURA_REGEN_MANA, false);
        return;
    }
    /// MOVEMENT AI
    float x, y, z = 0; // Where to go
    float r = 10;
    if (me->movespline->Finalized())
    {
        if (me->GetPositionX() < 1000.0f)
        {
            x = 1176;
            y = -4404;
        }
        else if (me->GetPositionX() + 10 < 1176.0f)
        {
            x = 1176;
            y = -4404;
        }
        else if (me->GetPositionX() + 10 < 1357.0f)
        {
            switch (urand(0, 1))
            {
                case 0:
                    x = 1357;
                    y = -4376;
                    break;
                case 1:
                    x = 1354;
                    y = -4412;
                    break;
                case 2:
                    x = 1346;
                    y = -4339;
                    break;
            }
        }
        else if (me->GetPositionX() + 10 < 1421.0f)
        {
            // Porte orgri
            x = 1427;
            y = -4362;
            z = 25.0f;
            r = 4;
        }
        else
        {
            switch (urand(0, 2))
            {
                case 0:
                    x = 1516;
                    y = -4410;
                    z = 17.0f;
                    r = 4;
                    break;
                case 1:
                    x = 1538;
                    y = -4347;
                    z = 18;
                    r = 3;
                    break;
                case 2:
                    x = 1617;
                    y = -4426;
                    z = 12;
                    r = 4;
                    break;
            }
        }
        if (!z)
        {
            z = me->GetPositionZ();
            me->UpdateGroundPositionZ(x, y, z);
        }
        r = 20;
        if (!me->GetMap()->GetWalkRandomPosition(nullptr, x, y, z, r))
            return;
    }
    else
    {
        return;
        if (urand(0, 20) == 0) // random move
        {
            me->GetPosition(x, y, z);
            r = frand(0, 2);
            float angle = me->GetOrientation() + frand(-M_PI_F / 2, M_PI_F / 2);
            x += r * cos(angle);
            y += r * sin(angle);
            if (!me->GetMap()->GetWalkHitPosition(nullptr, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), x, y, z))
                return;
        }
        else
            return;
    }
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
}

void PopulateAreaBotAI::BeforeAddToMap(Player* player)
{
    if (player->GetInstanceId() || player->GetTeam() != _team)
        return;
    if (player->GetMapId() != _map || !player->IsWithinDist3d(_x, _y, _z, _radius * 2))
    {
        float x = _x;
        float y = _y;
        float z = _z;
        Map* map = sMapMgr.CreateMap(_map, player);
        while (!map->GetWalkRandomPosition(nullptr, x, y, z, _radius));
        player->Relocate(x, y, z);
        player->SetLocationMapId(_map);
    }
}

void PopulateAreaBotAI::OnPlayerLogin()
{
    if (urand(0, 1))
        me->GetMotionMaster()->MoveConfused();
}

PlayerBotAI* CreatePlayerBotAI(std::string ainame)
{
    if (ainame == "MageOrgrimmarAttackerAI")
        return new MageOrgrimmarAttackerAI();
    if (ainame == "IronforgePopulationAI")
        return new PopulateAreaBotAI(0, -4928.5f, -946.6f, 501.6f, ALLIANCE, 100.0f);
    if (ainame == "StormwindPopulationAI")
        return new PopulateAreaBotAI(0, -8829.5f, 625.6f, 93.9f, ALLIANCE, 50.0f);
    if (ainame == "OrgrimmarPopulationAI")
        return new PopulateAreaBotAI(1, 1568, -4405.87f, 8.13f, HORDE, 150.0f);
    if (ainame == "PlayerBotFleeingAI")
        return new PlayerBotFleeingAI();
    return new PlayerBotAI();
}

// ---------------------------------------------------------------------------
// TW-014 (KAP-557): owner-directed follow/stop. A goal is identified by
// (leader, seq); a seq at or below the current one is stale and must never
// resume following (e.g. a delayed delivery of an already-stopped goal).
// ---------------------------------------------------------------------------
void PlayerBotAI::FollowGoal(uint32 leaderGuid, uint32 seq)
{
    if (!me)
        return;
    if (seq <= _followSeq)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Follow] goal rejected stale seq:%u current:%u GUID:%u",
                           seq, _followSeq, me->GetGUIDLow());
        return;
    }
    _followSeq = seq;
    _followLeaderGuid = leaderGuid;
    _followGroupId = me->GetGroup() ? me->GetGroup()->GetId() : 0;
    _following = true;
    _followReached = false;
    _held = false; // PORT-004: a new follow order cancels the hold
    _assistTargetGuid = 0; // PORT-005: a new follow order cancels an assist
    _pursuitLeash.Disarm(); // PORT-008: a new order starts a fresh pursuit budget
    _followPathAgeMs = 0; // PORT-008: a new order starts a fresh path window
    // PORT-003: do not stop an active engagement; the follow goal takes
    // effect once combat resolves (UpdateAI gate).
    if (!me->IsInCombat())
        me->GetMotionMaster()->Clear(false);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Follow] active GUID:%u leader:%u seq:%u",
                       me->GetGUIDLow(), leaderGuid, seq);
}

void PlayerBotAI::FollowStop()
{
    if (!me)
        return;
    _following = false;
    _followLeaderGuid = 0;
    _followReached = false;
    _pursuitLeash.Disarm(); // PORT-008: the pursuit is over; fresh budget for the next one
    // Invalidate the current goal immediately (TW-014 AC1).
    me->GetMotionMaster()->Clear(true);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Follow] inactive GUID:%u", me->GetGUIDLow());
}

void PlayerBotAI::Hold(uint32 seq)
{
    if (!me || seq <= _followSeq)
        return;
    _followSeq = seq;
    _held = true;
    _following = false;
    _followLeaderGuid = 0;
    _followReached = false;
    _assistTargetGuid = 0; // PORT-005: a hold cancels an active assist
    _pursuitLeash.Disarm(); // PORT-008: a hold starts a fresh pursuit budget
    ClearTarget();
    me->InterruptNonMeleeSpells(false);
    if (me->GetVictim())
        me->CombatStop();
    me->GetMotionMaster()->Clear(true);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Hold] active GUID:%u seq:%u", me->GetGUIDLow(), seq);
}

// PORT-005 (KAP-558): owner-selected assist. The manager validated
// ownership, party membership and the target (name -> legal hostile
// creature in sight) before calling this; the AI re-validates at execution
// time because grids, combat state and the target's life can change between
// order and action. The follow goal is kept: the assist suspends it, and
// Select resumes it once the assisted target is gone (priority:
// Hold > Assist > ContinueCombat > Follow).
void PlayerBotAI::AssistTarget(uint64_t targetGuid, uint32 seq)
{
    if (!me)
        return;
    if (seq <= _followSeq)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Assist] goal rejected stale seq:%u current:%u GUID:%u",
                           seq, _followSeq, me->GetGUIDLow());
        return;
    }
    _followSeq = seq;
    _assistTargetGuid = targetGuid;
    _held = false; // a new authorized order cancels the hold
    _pursuitLeash.Disarm(); // PORT-008: a new assist starts a fresh pursuit budget
    ClearTarget(); // the assist target is the only target; never keep an incidental one
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Assist] active GUID:%u target:%u seq:%u",
                       me->GetGUIDLow(), (uint32)ObjectGuid(targetGuid).GetCounter(), seq);
}

// PORT-006 (KAP-558): resolve the follow leader as the defend owner with
// the same availability rules as follow (alive, in world, same account,
// and the same group when the follow goal is group-bound). A missing or
// unavailable owner yields no candidate; the goal stays a plain follow.
Creature* PlayerBotAI::SelectDefendTarget() const
{
    if (!me || !me->GetMap() || !botEntry || !botEntry->ownerAccountId)
        return nullptr;
    Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
    if (!owner || !owner->IsAlive() || !owner->GetSession() ||
        owner->GetSession()->GetAccountId() != botEntry->ownerAccountId)
        return nullptr;
    if (_followGroupId && (!me->GetGroup() || me->GetGroup()->GetId() != _followGroupId ||
        owner->GetGroup() != me->GetGroup()))
        return nullptr;
    BotDefendScan scan(me, owner);
    Creature* found = nullptr;
    MaNGOS::CreatureLastSearcher<BotDefendScan> searcher(found, scan);
    Cell::VisitGridObjects(me, searcher, kDefendSearchRange);
    return scan.Best();
}

void PlayerBotAI::SetDefendTarget(uint64_t guid)
{
    if (_defendTargetGuid == guid)
        return;
    _defendTargetGuid = guid;
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Defend] active GUID:%u target:%u",
                       me->GetGUIDLow(), (uint32)ObjectGuid(guid).GetCounter());
}

void PlayerBotAI::ClearDefendTarget(const char* reason)
{
    if (!_defendTargetGuid)
        return;
    uint64_t const guid = _defendTargetGuid;
    _defendTargetGuid = 0;
    _defendTargetGrace = 0;
    _pursuitLeash.Disarm(); // PORT-008: the defend pursuit is over; fresh budget
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Defend] cleared GUID:%u target:%u reason:%s",
                       me->GetGUIDLow(), (uint32)ObjectGuid(guid).GetCounter(), reason);
}

bool PlayerBotAI::IsFollowOwnerAvailable() const
{
    if (!me || !me->GetMap() || !botEntry || !botEntry->ownerAccountId)
        return false;
    Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
    if (!owner || !owner->IsAlive() || !owner->GetSession() ||
        owner->GetSession()->GetAccountId() != botEntry->ownerAccountId)
        return false;
    if (_followGroupId && (!me->GetGroup() || me->GetGroup()->GetId() != _followGroupId ||
        owner->GetGroup() != me->GetGroup()))
        return false;
    return true;
}

bool PlayerBotAI::IsOwnedCompanion() const
{
    return botEntry && botEntry->ownerAccountId != 0;
}

Player* PlayerBotAI::FindOwnerByAccount() const
{
    if (!botEntry || !botEntry->ownerAccountId)
        return nullptr;
    HashMapHolder<Player>::MapType const& players = sObjectAccessor.GetPlayers();
    for (auto const& itr : players)
    {
        Player* p = itr.second;
        if (p && p->GetSession() &&
            p->GetSession()->GetAccountId() == botEntry->ownerAccountId && p->IsAlive())
            return p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// PORT-018 (KAP-558): one shared planner round per party.
//
// Eligibility is accepted player-party membership only: an owned
// companion in a group led by a live player (never a bot). Any party
// signature change (group id or leader) invalidates the transport
// session. The lowest-GUID owned companion builds and submits the
// request (queue depth 1, newest wins); every owned companion then
// fetches and records its own step. Transport offers are recorded,
// not executed: the deterministic policies own behavior until
// PORT-019 maps them.
// ---------------------------------------------------------------------------
void PlayerBotAI::PlannerRoundStep(uint32 diff)
{
    (void)diff;
    if (!IsOwnedCompanion() || !me || !me->GetGroup())
        return;
    Group const* group = me->GetGroup();
    uint32 const groupId = group->GetId();
    uint32 const leaderLow = group->GetLeaderGuid().GetCounter();
    if (!leaderLow || sPlayerBotMgr.FindBotByGuid(leaderLow))
        return; // bot-led: no player owner to plan for
    Companion::Planner::PlannerTransport& transport =
        sPlayerBotMgr.PlannerTransport();
    if (groupId != _plannerGroupId || leaderLow != _plannerLeaderGuid)
    {
        if (_plannerLeaderGuid && _plannerLeaderGuid != leaderLow)
            transport.InvalidateSession(_plannerLeaderGuid);
        _plannerGroupId = groupId;
        _plannerLeaderGuid = leaderLow;
        _plannerLastSubmitMs = 0; // fresh session: immediate first round
    }
    uint32 const nowMs = WorldTimer::getMSTime();
    // Collect the party's owned companions; the lowest-GUID one submits.
    uint32 submitterLow = 0;
    uint32 botCount = 0;
    uint32 botLows[Companion::Planner::kMaxPartyBots] = {};
    uint8_t botCls[Companion::Planner::kMaxPartyBots] = {};
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        uint32 const slotLow = slot.guid.GetCounter();
        Player* member = sObjectAccessor.FindPlayer(ObjectGuid(slotLow));
        PlayerBotEntry* be = member ? sPlayerBotMgr.FindBotByGuid(slotLow) : nullptr;
        if (!be || !be->ownerAccountId)
            continue;
        if (!submitterLow || slotLow < submitterLow)
            submitterLow = slotLow;
        if (botCount < Companion::Planner::kMaxPartyBots)
        {
            botLows[botCount] = slotLow;
            botCls[botCount] = member->GetClass();
            ++botCount;
        }
    }
    if (submitterLow == me->GetGUIDLow() && botCount > 0 &&
        (_plannerLastSubmitMs == 0 ||
         nowMs - _plannerLastSubmitMs >= Companion::Planner::kPlannerPaceMs))
    {
        Companion::Planner::Envelope env;
        env.requestId = ++_plannerReqId;
        env.ownerGuid = leaderLow;
        env.observationVersion = Companion::kObservationVersion;
        env.captureTimeMs = nowMs;
        env.stepCount = 0;
        env.totalSize = Companion::Planner::kRequestBytes;
        Companion::Planner::RequestBody body;
        body.generation = _followSeq;
        body.flags = (_held ? 0x01 : 0x00) | (_following ? 0x02 : 0x00) |
                     (IsFollowOwnerAvailable() ? 0x04 : 0x00);
        body.botCount = botCount;
        for (uint32 i = 0; i < botCount; ++i)
        {
            body.bots[i].botGuid = botLows[i];
            body.bots[i].cls = botCls[i];
        }
        uint8_t req[Companion::Planner::kRequestBytes];
        Companion::Planner::EncodeEnvelope(req, env);
        Companion::Planner::EncodeRequestBody(req + Companion::Planner::kEnvelopeBytes, body);
        if (transport.SubmitShared(leaderLow, req, Companion::Planner::kRequestBytes, nowMs))
            _plannerLastSubmitMs = nowMs;
    }
    // Record-only consumption (PORT-019 maps offers to behavior): every
    // owned companion fetches its own step from the shared round.
    Companion::Planner::Step offer;
    _plannerOfferValid =
        transport.FetchOffer(leaderLow, me->GetGUIDLow(), nowMs, offer);
    if (_plannerOfferValid)
        _plannerOffer = offer;
}

bool PlayerBotAI::UpdateCompanion(uint32 diff)
{
    // PORT-018 (KAP-558): planner rounds key on party membership, not
    // order state, so the round step runs before the no-order early
    // return.
    PlannerRoundStep(diff);
    if (!_following && !_held && !_assistTargetGuid)
        return false;
    Companion::Observation observation;
    observation.generation = _followSeq;
    observation.following = _following;
    observation.held = _held;
    observation.ownerAvailable = IsFollowOwnerAvailable();
    // PORT-005: re-resolve the assisted target from its GUID every tick; a
    // dead, vanished or unloaded target clears the assist (the companion
    // resumes its previous order) and never substitutes another enemy.
    if (_assistTargetGuid)
    {
        uint64_t const assistGuid = _assistTargetGuid;
        Creature* assist = me->GetMap() ? me->GetMap()->GetCreature(ObjectGuid(assistGuid)) : nullptr;
        if (assist && assist->IsAlive() && assist->IsInWorld())
            observation.assistTarget = assist->GetObjectGuid().GetRawValue();
        else
        {
            _assistTargetGuid = 0;
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot][Assist] target gone GUID:%u target:%u",
                               me->GetGUIDLow(), (uint32)ObjectGuid(assistGuid).GetCounter());
        }
    }
    // Hardening item 4 (KAP-558): a held combat target that died becomes
    // the pending loot corpse; the Loot intent picks it up this tick.
    // PORT-012: the live-slot transition is the module's named one; a
    // LiveToCorpse promotion resets the loot retry budget exactly as
    // before.
    if (_targets.live && me->GetMap())
    {
        Creature* held = me->GetMap()->GetCreature(ObjectGuid(_targets.live));
        if (_targets.OnLiveResolved(held != nullptr, held != nullptr && held->IsAlive())
            == Companion::Combat::TargetSlots::Transition::LiveToCorpse)
            _lootRetryCount = 0;
    }
    Unit* target = me->GetVictim();
    if (!target)
        target = GetAliveHeldTarget();
    if (target && target->GetTypeId() == TYPEID_UNIT && target->IsAlive())
        observation.target = target->GetObjectGuid().GetRawValue();
    // PORT-007 (KAP-558): a dead, in-world corpse the companion is meant to
    // loot becomes a first-class Loot target. The value is read here so the
    // policy stays pure; the executor re-resolves it from the GUID and
    // re-validates the world before acting.
    if (_targets.corpse && me->GetMap())
    {
        Creature* loot = me->GetMap()->GetCreature(ObjectGuid(_targets.corpse));
        if (loot && !loot->IsAlive() && loot->IsInWorld())
            observation.lootTarget = loot->GetObjectGuid().GetRawValue();
    }
    Companion::Intent intent = Companion::Select(observation);
    // PORT-006 (KAP-558): reactive defend. Fires only when the selection
    // would otherwise be Follow (no hold, no assist, no current target)
    // and the owner enabled it: engage a legal creature that is actually
    // attacking the owner or this companion. Priority stays
    // Hold > Assist > ContinueCombat > Defend > Follow, so a hold always
    // wins and an active fight is never abandoned for a new defender.
    // PORT-007: defend also interrupts a Loot goal (life over loot).
    if ((intent.action == Companion::Action::Follow || intent.action == Companion::Action::Loot) &&
        botEntry && botEntry->defendEnabled)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
        {
            _defendProbeTimer += diff;
            if (_defendProbeTimer >= 2000)
            {
                _defendProbeTimer = 0;
                // PORT-006 probe: what the defend scan sees each tick,
                // victim state or not. near:0 means an empty 30 yd radius.
                BotDefendProbe probe(me);
                Creature* nearest = nullptr;
                MaNGOS::CreatureLastSearcher<BotDefendProbe> searcher(nearest, probe);
                Cell::VisitGridObjects(me, searcher, kDefendSearchRange);
                nearest = probe.Best();
                if (!nearest)
                {
                    sLog.outString("[PlayerBot][Defend] probe GUID:%u near:0",
                                   me->GetGUIDLow());
                }
                else
                {
                    Unit const* const nv = nearest->GetVictim();
                    sLog.outString(
                        "[PlayerBot][Defend] probe GUID:%u near:%u v:%u evade:%u "
                        "combat:%u react:%u atk:%u threat:%u dist:%.2f",
                        me->GetGUIDLow(),
                        (uint32)ObjectGuid(nearest->GetObjectGuid()).GetCounter(),
                        nv ? (uint32)ObjectGuid(nv->GetObjectGuid()).GetCounter() : 0,
                        (uint32)nearest->IsInEvadeMode(),
                        (uint32)nearest->IsInCombat(),
                        (uint32)nearest->GetReactState(),
                        (uint32)nearest->GetAttackers().size(),
                        (uint32)!nearest->GetThreatManager().isThreatListEmpty(),
                        me->GetDistance(nearest));
                }
                // Diagnostic (2 s cadence, debug only): re-run the evidence
                // scan verbosely so a candidate rejected by the legality
                // gate is identified with the exact failing sub-checks.
                if (Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid)))
                {
                    BotDefendScan vscan(me, owner, true);
                    Creature* vfound = nullptr;
                    MaNGOS::CreatureLastSearcher<BotDefendScan> vsearcher(vfound, vscan);
                    Cell::VisitGridObjects(me, vsearcher, kDefendSearchRange);
                }
            }
        }
        // Hardening item 5 (KAP-558): defend is a typed intent. The scan
        // and grace bookkeeping only fill the observation; the policy
        // re-selects and the shared executor drives the engagement (the
        // grace counts down per tick in ExecuteCompanion while the Defend
        // intent is active).
        Creature* defender = SelectDefendTarget();
        if (defender)
        {
            SetDefendTarget(defender->GetObjectGuid().GetRawValue());
            _defendTargetGrace = kDefendTargetGraceMs;
            observation.defendTarget = defender->GetObjectGuid().GetRawValue();
        }
        else if (_defendTargetGuid && _defendTargetGrace > 0)
        {
            // Grace hysteresis: the scan can read the attacker as safe on a
            // flap tick. Keep the locked target through kDefendTargetGraceMs
            // after the last confirmed candidate; the executor re-validates
            // world state each tick, and the companion's first swing hands
            // the fight to the continue-combat path.
            Creature* held = me->GetMap()->GetCreature(ObjectGuid(_defendTargetGuid));
            if (held && held->IsAlive())
                observation.defendTarget = held->GetObjectGuid().GetRawValue();
            else
                ClearDefendTarget("owner safe");
        }
        else if (_defendTargetGuid)
        {
            ClearDefendTarget("owner safe");
        }
        if (observation.defendTarget)
            intent = Companion::Select(observation);
    }
    else if (_defendTargetGuid && !me->GetVictim() && !GetAliveHeldTarget())
    {
        ClearDefendTarget(intent.action == Companion::Action::Hold ? "hold"
                          : intent.action == Companion::Action::Assist ? "assist"
                          : "no goal");
    }
    // PORT-016 (KAP-558): declared damage tank-pull assist. Fires only
    // when the selection would otherwise be Follow or Loot (no hold,
    // assist, live combat or defend), like the defend fill: the only
    // candidate target is the declared tank's established victim and
    // the tank-pull discipline (Companion/Damage.h) decides when the
    // damage companion engages (threat gate, crowd-control
    // preservation, reach). The Damage intent runs through the shared
    // combat executor; once acquired, the fight persists as
    // ContinueCombat through the live slot.
    if ((intent.action == Companion::Action::Follow || intent.action == Companion::Action::Loot) &&
        IsDeclaredDamage())
    {
        Companion::Damage::Observation const dmgObs = FillDamageObservation();
        if (Companion::Damage::Select(dmgObs).action == Companion::Damage::Action::Damage)
        {
            observation.damageTarget = dmgObs.targetGuid;
            intent = Companion::Select(observation);
            // The policy carries the low GUID (value contract, log
            // fixture); the shared executor looks up packed object
            // GUIDs (the assist contract), so the adapter hands it the
            // packed form here, exactly like the assist branch.
            if (dmgObs.targetRaw)
                intent.target = dmgObs.targetRaw;
        }
        else
        {
            // [Damage] wait line (2 s cadence, debug only): why the
            // damage companion is not engaging. established:1 with
            // cc:1 is the crowd-control hold; established:0 is the
            // pull gate (no tank, no established target, or
            // insufficient tank threat).
            _damageWaitTimer += diff;
            if (_damageWaitTimer >= 2000)
            {
                _damageWaitTimer = 0;
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString(
                        "[Damage] wait GUID:%u tank:%u target:%u established:%u cc:%u "
                        "canAttack:%u los:%u dist:%.2f held:%u owner:%u",
                        me->GetGUIDLow(), dmgObs.tankGuid, dmgObs.targetGuid,
                        (uint32)Companion::Damage::PullEstablished(dmgObs),
                        (uint32)dmgObs.targetUnderCC, (uint32)dmgObs.canAttack,
                        (uint32)dmgObs.inLos, dmgObs.distance,
                        (uint32)dmgObs.held, (uint32)dmgObs.ownerAvailable);
            }
        }
    }
    // PORT-015 (KAP-558): declared healer triage runs on this tick's
    // budget, before behavior execution: a hold or an owner loss
    // (the card's failure cases) suppresses healing exactly like it
        // suppresses every other behavior, and an accepted heal does not
        // cancel the selected behavior - the behavior path (follow and
        // the rest) continues on the same tick after the cast step.
    if (IsDeclaredHealer() &&
        !(_held || !IsFollowOwnerAvailable() ||
          intent.action == Companion::Action::Hold))
    {
        Companion::Healer::Observation const healerObs = FillHealerObservation();
        Companion::Healer::Decision const healerDecision = Companion::Healer::Select(healerObs);
        if (healerDecision.action != Companion::Healer::Action::None)
            HealerTriageStep(healerObs, healerDecision);
    }
    ExecuteCompanion(intent, diff);
    return true;
}

// ---------------------------------------------------------------------------
// PORT-009 (KAP-558): one normal companion death and recovery path. While
// dead, Update() skips every offensive, loot and quest path; an owned
// companion (the same gate as UpdateCompanion) reclaims its own corpse
// through the normal CMSG_RECLAIM_CORPSE handler: it waits out the
// standard reclaim delay, walks to the corpse when out of range, and
// issues the reclaim the first tick inside CORPSE_RECLAIM_RADIUS. The
// handler's ResurrectPlayer(0.5f) applies the normal 50% restore - no
// free resurrection, no teleport, no second mechanism. Unavailable
// states (no corpse, delay not over, out of range) hold and report at a
// bounded pace.
// ---------------------------------------------------------------------------
bool PlayerBotAI::UpdateRecovery(uint32 diff)
{
    if (!me || !me->GetMap())
        return false;
    if (me->IsAlive())
    {
        _recoveryDead = false;
        _recoveryReportMs = 0;
        _recoveryWalkMs = 0;
        _recoveryDeathAck = false;
        return false;
    }
    // Owned companions with an active order only; ambient dead bots keep
    // the legacy dead-idle behavior.
    if (!_following && !_held && !_assistTargetGuid)
        return false;
    if (!_recoveryDead)
    {
        _recoveryDead = true;
        _recoveryReportMs = 0;
        _recoveryWalkMs = 0;
        _recoveryDeathAck = false;
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Recovery] entered GUID:%u",
                           me->GetGUIDLow());
    }
    Corpse* corpse = me->GetCorpse();
    if ((!corpse || !corpse->IsInWorld()) && !_recoveryDeathAck)
    {
        // PORT-009: KillPlayer defers corpse creation to the client's
        // CMSG_MOVE_DEADACK, which a socketless session never sends (the
        // fallback is the 6 min repop timer). Build the corpse now, the way the
        // client's dead-ack would, so the standard reclaim path can run.
        _recoveryDeathAck = true;
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Recovery] death-ack issued GUID:%u",
                           me->GetGUIDLow());
        me->BuildPlayerRepop();
        corpse = me->GetCorpse();
    }
    if (!corpse || !corpse->IsInWorld())
    {
        // The corpse is not (yet) available: hold and report at a bounded
        // pace. There is no fallback teleport or free resurrection.
        if (_recoveryReportMs <= diff)
        {
            _recoveryReportMs = kRecoveryReportMs;
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot][Recovery] unavailable GUID:%u reason:no-corpse",
                               me->GetGUIDLow());
        }
        else
            _recoveryReportMs -= diff;
        return true;
    }
    // Mirror the handler's reclaim gate: ghost time plus the standard
    // reclaim delay must have passed before the reclaim is legal.
    if (corpse->GetGhostTime() +
            me->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
        time(nullptr))
    {
        if (_recoveryReportMs <= diff)
        {
            _recoveryReportMs = kRecoveryReportMs;
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot][Recovery] waiting GUID:%u dist:%.1f",
                               me->GetGUIDLow(), me->GetDistance(corpse));
        }
        else
            _recoveryReportMs -= diff;
        return true;
    }
    if (corpse->IsWithinDistInMap(me, CORPSE_RECLAIM_RADIUS, true))
    {
        me->GetMotionMaster()->Clear(true);
        WorldPacket packet(CMSG_RECLAIM_CORPSE);
        packet << me->GetObjectGuid();
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Recovery] reclaim issued GUID:%u",
                           me->GetGUIDLow());
        // The normal handler path: it re-validates death, ghost flag,
        // corpse, delay and range, then resurrects at 50% and spawns the
        // bones (the standard reclaim penalty, no free resurrection).
        me->GetSession()->HandleReclaimCorpseOpcode(packet);
        return true;
    }
    // Out of range: walk to the corpse. The path is re-issued only when
    // the motion is empty and the retry window has elapsed, so a failed
    // path is retried at a bounded pace and the companion is never
    // trapped by an unreachable corpse.
    if (me->GetMotionMaster()->empty() && _recoveryWalkMs <= diff)
    {
        _recoveryWalkMs = kRecoveryWalkRetryMs;
        me->GetMotionMaster()->MovePoint(0, corpse->GetPositionX(),
                                         corpse->GetPositionY(),
                                         corpse->GetPositionZ(), MOVE_PATHFINDING);
    }
    else
        _recoveryWalkMs = (_recoveryWalkMs > diff) ? _recoveryWalkMs - diff : 0;
    if (_recoveryReportMs <= diff)
    {
        _recoveryReportMs = kRecoveryReportMs;
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Recovery] walking GUID:%u dist:%.1f",
                           me->GetGUIDLow(), me->GetDistance(corpse));
    }
    else
        _recoveryReportMs -= diff;
    return true;
}

// ---------------------------------------------------------------------------
// PORT-008 (KAP-558): one tick of the bounded pursuit budget. While the
// companion cannot land a melee hit on the named target it may keep
// chasing for at most Leash::kArmedMs; on expiry the caller abandons the
// pursuit (drops the target, stops combat, clears motion) so the prior
// order resumes. A target within melee reach disarms the budget - an
// actual fight has unbounded kill time.
// ---------------------------------------------------------------------------
bool PlayerBotAI::PursuitLeashTick(Unit* target, uint32 diff)
{
    if (!target)
    {
        _pursuitLeash.Disarm();
        return false;
    }
    Companion::Combat::Leash::Step const step =
        _pursuitLeash.Tick(me && me->CanReachWithMeleeAutoAttack(target), diff);
    if (step == Companion::Combat::Leash::Step::Armed)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] pursuit leash armed GUID:%u target:%u",
                           me->GetGUIDLow(), target->GetGUIDLow());
        return false;
    }
    if (step == Companion::Combat::Leash::Step::Expired)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] pursuit leash expired GUID:%u target:%u",
                           me->GetGUIDLow(), target->GetGUIDLow());
        return true;
    }
    return false;
}
// ---------------------------------------------------------------------------
// PORT-009 diagnostic: the target never retaliated against a
// player bot; log the exact creature-side and player-side
// checks that selectNextVictim/IsTargetable use, each combat
// tick, until the missing condition is identified.
// ---------------------------------------------------------------------------
void PlayerBotAI::LogAssistProbe(Creature* target)
{
    float const assistThreat = target->GetThreatManager().getThreat(me, true);
    sLog.outString(
        "[PlayerBot][Assist] probe GUID:%u t:%u v:%u evade:%u combat:%u "
        "react:%u atk:%u tlist:%u threat:%.1f valid:%u outarea:%u "
        "canatk:%u pacified:%u selfcombat:%u selfgm:%u selfimmnpc:%u "
        "selftgt:%u selfdet:%u rawflags:%x bytes1:%x cin:%u feign:%u "
        "taxi:%u inworld:%u mounted:%u canatkself:%u canauto:%u",
        me->GetGUIDLow(),
        target->GetGUIDLow(),
        target->GetVictim() ? (uint32)target->GetVictim()->GetGUIDLow() : 0,
        (uint32)target->IsInEvadeMode(),
        (uint32)target->IsInCombat(),
        (uint32)target->GetReactState(),
        (uint32)target->GetAttackers().size(),
        (uint32)!target->GetThreatManager().isThreatListEmpty(),
        assistThreat,
        (uint32)target->IsValidAttackTarget(me),
        (uint32)target->IsOutOfThreatArea(me),
        (uint32)target->CanInitiateAttack(),
        (uint32)target->IsTempPacified(),
        (uint32)me->IsInCombat(),
        (uint32)me->IsGameMaster(),
        (uint32)me->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC),
        (uint32)me->IsTargetable(true, false, false, true),
        (uint32)me->CanBeDetected(),
        me->GetUInt32Value(UNIT_FIELD_FLAGS),
        me->GetUInt32Value(UNIT_FIELD_BYTES_1),
        me->watching_cinematic_entry,
        (uint32)me->HasUnitState(UNIT_STAT_FEIGN_DEATH),
        (uint32)me->IsTaxiFlying(),
        (uint32)me->IsInWorld(),
        (uint32)me->IsMounted(),
        (uint32)me->CanAttack(target),
        (uint32)(me->CanAutoAttackTarget(target) == ATTACK_RESULT_OK));
}

// ---------------------------------------------------------------------------
// Hardening item 3 (KAP-558): the single combat executor shared by the
// Assist, ContinueCombat and Defend intents. A Companion::Combat::Request
// carries the target GUID, the source intent and the engagement distance
// limit. The adapter re-resolves the target against the world each tick and
// fills a value-only TargetSnapshot; the module's Verify applies the shared
// legality rules (defend accepts neutral attackers the CanAttack gate
// rejects; a continued fight must still be the current victim or held
// target and has no LOS rule). A failed engagement is dropped with the
// source-specific cleanup (assist clears the assist state and motion, a
// held fight stops combat, defend clears the defend lock) so the prior
// order resumes. The per-source debug lines are the fixture contract and
// are preserved verbatim.
// ---------------------------------------------------------------------------
bool PlayerBotAI::ExecuteCombat(Companion::Combat::Request const& req, uint32 diff)
{
    if (!me || !me->IsAlive() || !me->GetMap() || req.generation != _followSeq)
        return false;
    Creature* target = me->GetMap()->GetCreature(ObjectGuid(req.target));
    Companion::Combat::TargetSnapshot snap;
    if (target)
    {
        if (req.source == Companion::Combat::Source::Defend)
        {
            // One snapshot rule set for the defend scan and the executor.
            Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
            snap = DefendSnapshot(me, owner, target);
        }
        else
        {
            snap.exists = true;
            snap.alive = target->IsAlive();
            snap.inWorld = target->IsInWorld();
            if (snap.alive && snap.inWorld)
            {
                snap.canAttack = me->CanAttack(target);
                snap.friendly = me->IsFriendlyTo(target);
                snap.isVictim = me->GetVictim() == target;
                snap.isHeld = GetAliveHeldTarget() == target;
            }
            if (req.source == Companion::Combat::Source::Damage && snap.alive && snap.inWorld)
            {
                // PORT-016: the pull-discipline facts, re-resolved
                // from the live world immediately before the
                // engagement is honored (the adapter revalidation
                // the shared Verify consumes).
                snap.targetUnderCC = TargetUnderCC(target);
                snap.establishedTarget = IsEstablishedTankTarget(target->ToCreature());
            }
        }
        if (snap.alive && snap.inWorld)
        {
            snap.inLos = me->IsWithinLOSInMap(target);
            snap.distance = me->GetDistance(target);
        }
    }
    Companion::Combat::Verdict const verdict = Companion::Combat::Verify(req, snap);
    if (!verdict.legal)
    {
        switch (req.source)
        {
            case Companion::Combat::Source::Assist:
            {
                // The named target is no longer a legal engagement. Drop it
                // and resume the prior order; never substitute an unrelated
                // enemy.
                _assistTargetGuid = 0;
                _pursuitLeash.Disarm(); // PORT-008: the pursuit is over; fresh budget for the next one
                if (me->GetVictim())
                    me->CombatStop();
                me->GetMotionMaster()->Clear(false);
                _targets.ReleaseLive();
                if (sPlayerBotMgr.IsDebugEnabled())
                {
                    if (target && target->IsAlive())
                        sLog.outString("[PlayerBot][Assist] target out of reach GUID:%u target:%u dist:%.2f",
                                       me->GetGUIDLow(), target->GetGUIDLow(), me->GetDistance(target));
                    else
                        sLog.outString("[PlayerBot][Assist] target invalid GUID:%u target:%u",
                                       me->GetGUIDLow(), (uint32)ObjectGuid(req.target).GetCounter());
                }
                break;
            }
            case Companion::Combat::Source::ContinueCombat:
                _pursuitLeash.Disarm(); // PORT-008: the pursuit is over; fresh budget for the next one
                if (me->GetVictim())
                    me->CombatStop();
                _targets.ReleaseLive();
                break;
            case Companion::Combat::Source::Damage:
                _pursuitLeash.Disarm(); // PORT-008: the pursuit is over; fresh budget for the next one
                if (me->GetVictim())
                    me->CombatStop();
                _targets.ReleaseLive();
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[Damage] target dropped GUID:%u target:%u reject:%s",
                                   me->GetGUIDLow(),
                                   (uint32)ObjectGuid(req.target).GetCounter(),
                                   Companion::Combat::RejectName(verdict.reject));
                break;
            case Companion::Combat::Source::Defend:
                ClearDefendTarget("target invalid");
                break;
        }
        return false;
    }
    // PORT-008 (KAP-558): pursuit reach budget; runs every tick outside the
    // 2 s combat check pacing. On expiry the engagement is abandoned exactly
    // like an invalid target and the prior order resumes.
    if (PursuitLeashTick(target, diff))
    {
        switch (req.source)
        {
            case Companion::Combat::Source::Assist:
                _assistTargetGuid = 0;
                if (me->GetVictim())
                    me->CombatStop();
                me->GetMotionMaster()->Clear(false);
                _targets.ReleaseLive();
                break;
            case Companion::Combat::Source::ContinueCombat:
                if (me->GetVictim())
                    me->CombatStop();
                _targets.ReleaseLive();
                break;
            case Companion::Combat::Source::Damage:
                if (me->GetVictim())
                    me->CombatStop();
                _targets.ReleaseLive();
                break;
            case Companion::Combat::Source::Defend:
                if (me->GetVictim() == target)
                    me->CombatStop();
                ClearDefendTarget("leash");
                break;
        }
        return false;
    }
    if (_abilityTimer > diff)
        _abilityTimer -= diff;
    else
        _abilityTimer = 0;
    if (_combatCheckTimer > diff)
    {
        _combatCheckTimer -= diff;
        return true;
    }
    _combatCheckTimer = 2000;
    RememberCombatTarget(target);
    if (sPlayerBotMgr.IsDebugEnabled())
    {
        if (req.source == Companion::Combat::Source::Assist)
            sLog.outString("[PlayerBot][Assist] fighting GUID:%u target:%u dist:%.2f",
                           me->GetGUIDLow(), target->GetGUIDLow(), me->GetDistance(target));
        else if (req.source == Companion::Combat::Source::Defend)
            sLog.outString("[PlayerBot][Defend] fighting GUID:%u target:%u dist:%.2f",
                           me->GetGUIDLow(), target->GetGUIDLow(), me->GetDistance(target));
        else if (req.source == Companion::Combat::Source::Damage)
            sLog.outString("[Damage] fighting GUID:%u target:%u dist:%.2f",
                           me->GetGUIDLow(), target->GetGUIDLow(), me->GetDistance(target));
        else
            sLog.outString("[PlayerBot] fighting GUID:%u victim:%u dist:%.2f",
                           me->GetGUIDLow(), target->GetGUIDLow(), me->GetDistance(target));
        if (req.source == Companion::Combat::Source::Assist)
            LogAssistProbe(target);
        // Hardening diag (KAP-558): the four swing-gate conditions from
        // UpdateMeleeAttackingState, sampled each offense step, so a
        // stopped swing is identified by its failing predicate.
        if (req.source == Companion::Combat::Source::Assist)
            sLog.outString(
                "[PlayerBot][Assist] swing GUID:%u t:%u victim:%u cast:%u "
                "ready:%u atktimer:%u facing:%u auto:%u los:%u abltimer:%u",
                me->GetGUIDLow(), target->GetGUIDLow(),
                me->GetVictim() ? (uint32)me->GetVictim()->GetGUIDLow() : 0,
                (uint32)me->IsNonMeleeSpellCasted(false),
                (uint32)me->IsAttackReady(BASE_ATTACK),
                me->GetAttackTimer(BASE_ATTACK),
                (uint32)me->HasInArc(target, 2 * M_PI_F / 3),
                (uint32)me->CanAutoAttackTarget(target),
                (uint32)me->IsWithinLOSInMap(target),
                _abilityTimer);
    }
    if (!me->CanReachWithMeleeAutoAttack(target))
        me->GetMotionMaster()->MoveChase(target);
    else
        me->SetFacingToObject(target);
    // PORT-016 (KAP-558): the damage companion's pull evidence line,
    // sampled on the same 2 s combat pacing: the established-target
    // and crowd-control facts at each offense step (the fixture
    // contract, like the [Tank] threat line above).
    if (IsDeclaredDamage() && sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[Damage] pull GUID:%u t:%u established:%u cc:%u victim:%u",
                       me->GetGUIDLow(), target->GetGUIDLow(),
                       (uint32)IsEstablishedTankTarget(target->ToCreature()),
                       (uint32)TargetUnderCC(target),
                       target->GetVictim() ? (uint32)target->GetVictim()->GetGUIDLow() : 0);
    // PORT-014 (KAP-558): the declared tank matrix owns the assist offense
    // step: measured normal threat, a taunt when the protected party member
    // holds it, and the ordinary attack otherwise. The [Tank] threat line
    // is the fixture contract, sampled on the 2 s combat pacing above.
    if (req.source == Companion::Combat::Source::Assist && IsDeclaredTank())
    {
        Companion::Tank::Observation const obs = FillTankObservation(target);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Tank] threat GUID:%u me:%u owner:%u t:%u victim:%u",
                           me->GetGUIDLow(), obs.tankThreat, obs.ownerThreat,
                           target->GetGUIDLow(), obs.victimGuid);
        Companion::Tank::Decision const decision = Companion::Tank::SelectAction(obs);
        if (decision.action == Companion::Tank::Action::Taunt)
            TankTauntStep(target);
        else
            me->Attack(target, true); // normal threat: the ordinary white swing
    }
    else if (!_abilityTimer && me->IsWithinLOSInMap(target))
        TryOffensiveCastOrAttack(target);
    else
        me->Attack(target, true);
    return true;
}
void PlayerBotAI::ExecuteCompanion(Companion::Intent const& intent, uint32 diff)
{
    if (!Companion::IsCurrent(intent, _followSeq) || !me || !me->IsAlive() || !me->GetMap())
        return;
    // PORT-005: the assist runs before the owner-availability gate: helping
    // a party member is valid even while the follow leader is momentarily
    // unavailable (dead, loading, grouped elsewhere).
    if (intent.action == Companion::Action::Assist)
    {
        // Hardening item 3 (KAP-558): the assist engagement runs through the
        // shared executor; a vanished or out-of-reach target drops the assist
        // (state, motion and combat cleared) and the prior order resumes.
        ExecuteCombat(Companion::Combat::Request{intent.target, Companion::Combat::Source::Assist,
                                                                                          intent.generation, 35.0f}, diff);
        return;
    }
    if (intent.action == Companion::Action::Defend)
    {
        // Hardening item 5 (KAP-558): the defend engagement runs through
        // the shared executor; the grace counts down per tick while this
        // intent is active, and a failed engagement drops the defend lock
        // (reason logged) so the prior order resumes.
        _defendTargetGrace = (_defendTargetGrace > diff) ? _defendTargetGrace - diff : 0;
        ExecuteCombat(Companion::Combat::Request{intent.target, Companion::Combat::Source::Defend,
                                                                                          intent.generation, 35.0f}, diff);
        return;
    }
    if (intent.action == Companion::Action::Damage)
    {
        // PORT-016 (KAP-558): the damage engagement runs through the
        // shared executor; a vanished, out-of-reach, controlled or
        // no-longer-established target drops the engagement (combat
        // and the live slot released) and the prior order resumes.
        ExecuteCombat(Companion::Combat::Request{intent.target, Companion::Combat::Source::Damage,
                                                                                          intent.generation,
                                                                                          Companion::Damage::kEngageDistance}, diff);
        return;
    }
    if (_held || !IsFollowOwnerAvailable() || intent.action == Companion::Action::Hold)
    {
        me->InterruptNonMeleeSpells(false);
        if (me->GetVictim())
            me->CombatStop();
        me->GetMotionMaster()->Clear(false);
        ClearTarget();
        return;
    }
    if (intent.action == Companion::Action::Loot)
    {
        Creature* corpse = me->GetMap()->GetCreature(ObjectGuid(intent.target));
        ExecuteLoot(corpse, diff);
        return;
    }
    if (intent.action == Companion::Action::Follow)
    {
        UpdateFollow(diff);
        return;
    }
    if (intent.action != Companion::Action::ContinueCombat)
        return;
    // Hardening item 3 (KAP-558): the held-target engagement runs through
    // the shared executor; a vanished or released target drops the fight
    // and the prior order resumes.
    ExecuteCombat(Companion::Combat::Request{intent.target, Companion::Combat::Source::ContinueCombat,
                                                                                                  intent.generation, 35.0f}, diff);
}

// PORT-007 (KAP-558): one bounded execution tick of the companion Loot
// intent. The named corpse is re-validated against the world; a vanished,
// re-embodied or illegal corpse drops the goal so the prior order resumes.
// The attempt is time-bounded by kLootWindowMs (a diff countdown, no wall
// clock) so a denied or unreachable corpse can never trap the companion:
// on expiry the follow goal resumes (regroup). The walk/tap/store body is
// shared with the legacy fallback through CorpseLootStep.
void PlayerBotAI::ExecuteLoot(Creature* corpse, uint32 diff)
{
    if (!me || !me->IsAlive() || !me->GetMap() || !corpse ||
        !corpse->IsInWorld() || corpse->IsAlive())
    {
        ClearTarget();
        return;
    }
    if (_lootWindowMs == 0)
    {
        _lootWindowMs = kLootWindowMs;
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] loot intent GUID:%u target:%u window:%u",
                           me->GetGUIDLow(), corpse->GetGUIDLow(), _lootWindowMs);
    }
    _lootWindowMs = (_lootWindowMs > diff) ? _lootWindowMs - diff : 0;
    if (_lootWindowMs == 0)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] corpse loot timeout GUID:%u target:%u",
                           me->GetGUIDLow(), corpse->GetGUIDLow());
        ClearTarget();
        return;
    }
    CorpseLootStep(corpse);
}

bool PlayerBotAI::UpdateFollow(uint32 diff)
{
    if (!_following || !me || !me->IsAlive() || !me->IsInWorld() || !me->GetMap())
        return false;

    // Same-map lookup: a leader on another map (or not in world yet) is
    // temporarily unavailable; the goal stays active and the bot holds
    // position instead of dropping it.
    Player* leader = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
    if (!leader || !leader->IsAlive())
    {
        if (sPlayerBotMgr.IsDebugEnabled() && _followDebugTimer <= diff)
        {
            _followDebugTimer = 5000;
            sLog.outString("[PlayerBot][Follow] leader unavailable GUID:%u leader:%u null:%u dead:%u",
                           me->GetGUIDLow(), _followLeaderGuid, leader ? 0 : 1,
                           (leader && !leader->IsAlive()) ? 1 : 0);
        }
        else if (sPlayerBotMgr.IsDebugEnabled())
            _followDebugTimer -= diff;
        return true;
    }

    if (me->GetDistance(leader) > kFollowRange)
    {
        // PORT-015 (KAP-558): the declared heal is cast-interruptible by
        // movement; while a timed cast is in flight the follower holds
        // position (and cancels a walk in progress) so the cast completes.
        // The approach re-issues on the first tick after the cast ends.
        if (me->IsNonMeleeSpellCasted(true))
        {
            if (!me->GetMotionMaster()->empty())
                me->GetMotionMaster()->Clear(false);
            return true;
        }
        // PORT-007: re-arm the reached latch while out of range so the
        // "reached" line marks every out-of-range -> in-range transition
        // (a completed regroup), not just the first approach.
        _followReached = false;
        // The same path-find-to-position pattern idle wander and the quest
        // giver pursuit use (a player chase is a no-op without a victim).
        // PORT-008 (KAP-558): throttle follow path re-issuance. The path is
        // re-issued only when the motion master is empty (the walk finished
        // but the owner is still out of range), the owner moved more than
        // 2.0 yd (2D) from the last issued target, or the issued target is
        // stale (kFollowPathRefreshMs).
        {
            float const lx = leader->GetPositionX();
            float const ly = leader->GetPositionY();
            float const lz = leader->GetPositionZ();
            float const dpx = lx - _followPathX;
            float const dpy = ly - _followPathY;
            bool const moved = (dpx * dpx + dpy * dpy) > (2.0f * 2.0f);
            if (me->GetMotionMaster()->empty() || moved ||
                _followPathAgeMs >= kFollowPathRefreshMs)
            {
                _followPathX = lx;
                _followPathY = ly;
                _followPathZ = lz;
                _followPathAgeMs = 0;
                me->GetMotionMaster()->MovePoint(0, lx, ly, lz, MOVE_PATHFINDING);
            }
            else
                _followPathAgeMs += diff;
        }
        return true;
    }

    if (!me->GetMotionMaster()->empty())
        me->GetMotionMaster()->Clear(false);
    _followPathAgeMs = 0; // PORT-008: fresh path window after a completed approach
    if (!_followReached)
    {
        _followReached = true;
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Follow] reached GUID:%u leader:%u dist:%.2f seq:%u",
                           me->GetGUIDLow(), _followLeaderGuid, me->GetDistance(leader), _followSeq);
    }
    return true;
}
