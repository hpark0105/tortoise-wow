#include "PlayerBotAI.h"
#include "Player.h"
#include "Pet.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "Log.h"
#include "SocialMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "MoveSpline.h"
#include "PlayerBotMgr.h"
#include "Companion/CitizenGearPolicy.h"
#include "ObjectAccessor.h"
#include "Timer.h"
#include "Group.h"
#include "WorldPacket.h"
#include "World.h"
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
#include <map>
#include <algorithm>
#include <cstring>


namespace
{
uint32 const kCitizenNoTargetFirstExpansionMs = 5 * 60 * 1000;
uint32 const kCitizenNoTargetSecondExpansionMs = 15 * 60 * 1000;
uint32 const kCitizenNoTargetRecenterMs = 30 * 60 * 1000;
float const kCitizenPatrolRadiusNormal = 240.0f;
float const kCitizenPatrolRadiusExpanded = 600.0f;
float const kCitizenPatrolRadiusWide = 1200.0f;

class NearestTameableCreatureCheck
{
public:
    NearestTameableCreatureCheck(Player const* player, float range) : i_player(player), i_range(range) {}
    WorldObject const& GetFocusObject() const { return *i_player; }
    bool operator()(Creature const* creature)
    {
        CreatureInfo const* info = creature ? creature->GetCreatureInfo() : nullptr;
        if (!creature || !creature->IsAlive() || !info || !info->isTameable() ||
            creature->GetLevel() > i_player->GetLevel() || creature->IsInCombat() ||
            !i_player->IsWithinDistInMap(creature, i_range))
            return false;
        i_range = i_player->GetDistance(creature);
        return true;
    }
private:
    Player const* const i_player;
    float i_range;
};

// TW-014 (KAP-557): the companion holds this range around its owner.
const float kFollowRange = 2.0f;
const float kFollowSideOffsetYd = 1.5f; // PORT-032: stand beside, not on the leader

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
// A citizen may only begin a destination route it can plausibly traverse
// during normal play. Longer continent-spanning vectors are kept out of the
// autonomous loop until a multi-stage route graph exists.
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

// PORT-025 (KAP-558): nearest legal vendor within the declared
// radius: live, in-world, vendor service flag, not evading. The
// executor re-validates through the same interaction check the
// vendor packet handler uses before any approach or sale.
class BotVendorSearcher
{
public:
    BotVendorSearcher(Unit const* source)
        : me(source), m_best(nullptr), m_dist(0.0f) {}

    bool operator()(Creature* u)
    {
        if (u == me || !u->IsAlive() || !u->IsInWorld())
            return false;
        if (!u->IsVendor() || u->IsInEvadeMode())
            return false;
        float const d = me->GetDistance(u);
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
    BotVendorSearcher(BotVendorSearcher const&);
    Unit const* me;
    Creature* m_best;
    float m_dist;
};

// Persistent bots repair worn gear only when a repair NPC is already within
// normal interaction range. The repair executor re-checks the normal NPC
// interaction guards before charging the bot the standard repair cost.
class BotRepairVendorSearcher
{
public:
    explicit BotRepairVendorSearcher(Unit const* source)
        : me(source), m_best(nullptr), m_dist(0.0f) {}

    bool operator()(Creature* u)
    {
        if (u == me || !u->IsAlive() || !u->IsInWorld() ||
            !(u->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_REPAIR) ||
            u->IsInEvadeMode())
            return false;
        float const d = me->GetDistance(u);
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
    BotRepairVendorSearcher(BotRepairVendorSearcher const&);
    Unit const* me;
    Creature* m_best;
    float m_dist;
};

// Citizens seek ordinary, untapped, level-appropriate creatures only. The
// nearest eligible target wins so a patrol never pulls a distant pack or a
// mob already engaged by a player. This scan is paced by the AI below.
class BotCitizenHuntScan
{
public:
    explicit BotCitizenHuntScan(Player* source, uint32 excluded = 0) : me(source), m_excluded(excluded) {}

    static bool Eligible(Player const* me, Creature* u)
    {
        if (!u || !u->IsAlive() || !u->IsInWorld() || u->IsPet() ||
            u->IsInCombat() || u->HasLootRecipient() || u->IsInEvadeMode() ||
            !u->GetCreatureInfo() ||
            u->GetCreatureInfo()->rank != CREATURE_ELITE_NORMAL ||
            me->IsFriendlyTo(u) || !me->CanAttack(u) ||
            !u->IsTargetable(true, me->IsCharmerOrOwnerPlayerOrPlayerItself()))
            return false;
        uint32 const level = me->GetLevel();
        // Keep independent citizens in a productive XP band. A target more
        // than three levels below the citizen is intentionally skipped, so
        // an outgrown zone leads to progression travel instead of endless
        // farming of trivial creatures.
        // Independent citizens have no player healer or coordinated party
        // behind them. A same-level solo target is the highest safe default;
        // tougher content is reserved for an actual player party.
        if (u->GetLevel() > level ||
            u->GetLevel() + 3 < level)
            return false;
        return true;
    }

    bool operator()(Creature* u)
    {
        if (!Eligible(me, u) || u->GetGUIDLow() == m_excluded ||
            !u->IsWithinDistInMap(me, 25.0f, false, SizeFactor::None))
            return false;
        float const distance = me->GetDistance(u);
        if (!m_best || distance < m_distance ||
            (distance == m_distance && u->GetGUIDLow() < m_best->GetGUIDLow()))
        {
            m_best = u;
            m_distance = distance;
        }
        return true;
    }

    Creature* Best() const { return m_best; }

private:
    Player* me;
    uint32 m_excluded;
    Creature* m_best = nullptr;
    float m_distance = 0.0f;
};

// One bounded local scan per retreat attempt. Overflow fails closed rather
// than treating an incompletely observed crowded area as a safe destination.
class BotCitizenEscapeScan
{
public:
    BotCitizenEscapeScan(Player* source, float radius) : me(source), radius(radius) {}
    bool operator()(Creature* u)
    {
        if (!u || !u->IsAlive() || !u->IsWithinDistInMap(me, radius) ||
            (!u->IsHostileTo(me) && u->GetVictim() != me) || !u->CanAttack(me))
            return false;
        if (hostiles.size() < 32)
            hostiles.push_back(u);
        else
            overflow = true;
        return false;
    }
    Player* me;
    float radius;
    std::vector<Creature*> hostiles;
    bool overflow = false;
};

// A distant eligible creature is a destination, not an immediate attack.
// Stay in the spawn zone and within its home tether; never cross a zone or
// select a mob already claimed by another player merely to look busy.
class BotCitizenHuntingGroundScan
{
public:
    BotCitizenHuntingGroundScan(Player* source, float homeX, float homeY,
                                uint32 zone, uint32 excludedGuid, float homeRadius)
        : me(source), m_homeX(homeX), m_homeY(homeY), m_zone(zone),
          m_excludedGuid(excludedGuid), m_homeRadius(homeRadius) {}

    bool operator()(Creature* u)
    {
        if (!BotCitizenHuntScan::Eligible(me, u) ||
            u->GetGUIDLow() == m_excludedGuid ||
            !u->IsWithinDistInMap(me, 110.0f, false, SizeFactor::None))
            return false;
        float const distance = me->GetDistance(u);
        float const hx = u->GetPositionX() - m_homeX;
        float const hy = u->GetPositionY() - m_homeY;
        if (distance < 30.0f || hx * hx + hy * hy > m_homeRadius * m_homeRadius ||
            me->GetMap()->GetTerrain()->GetZoneId(u->GetPositionX(),
                u->GetPositionY(), u->GetPositionZ()) != m_zone)
            return false;
        if (!m_best || distance < m_distance ||
            (distance == m_distance && u->GetGUIDLow() < m_best->GetGUIDLow()))
        {
            m_best = u;
            m_distance = distance;
        }
        return true;
    }

    Creature* Best() const { return m_best; }

private:
    Player* me;
    float m_homeX, m_homeY;
    uint32 m_zone, m_excludedGuid;
    float m_homeRadius;
    Creature* m_best = nullptr;
    float m_distance = 0.0f;
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

    // A same-map teleport is also a position discontinuity. Its displacement
    // must not be mistaken for a successful walk out of combat.
    if (me->IsBeingTeleportedNear() || me->IsBeingTeleportedFar())
    {
        if (_citizenRetreat.Active() && !MotionIdle())
            ClearCitizenRetreatMotion();
        _citizenRetreat.Reset();
        _citizenRetreatRetry.Reset();
        // AC-10 R5: a teleport is a position discontinuity; the
        // hunting-group episode age does not survive it.
        _citizenGroupLeaderLostMs = 0;
        _citizenGroupFollowProgress.Reset();
    }

    if (me->IsBeingTeleportedNear())
    {
        WorldPacket data(MSG_MOVE_TELEPORT_ACK, 10);
        data << me->GetObjectGuid();
        data << uint32(0) << uint32(0);
        // A null session means the disconnect flow already owns the
        // player; skipping the ack loses only this in-flight teleport.
        if (WorldSession* sess = me->GetSession())
            sess->HandleMoveTeleportAckOpcode(data);
    }
    if (me->IsBeingTeleportedFar())
    {
        if (WorldSession* sess = me->GetSession())
            sess->HandleMoveWorldportAckOpcode();
    }

    if (!me->IsInWorld())
        return;

    _citizenRetreatRetry.Tick(diff);
    if (!me->IsAlive() ||
        (me->GetGroup() && !sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup())) ||
        _following || _held || _assistTargetGuid || me->GetMapId() != _citizenRetreatMap ||
        (botEntry && botEntry->recruiterAccountId))
    {
        // A pending retreat leg must not resurface on the motion stack
        // once party/recovery orders own motion; terminal retreat
        // outcomes clear motion the same way (UpdateCitizenRetreat).
        if (_citizenRetreat.Active() && !MotionIdle())
            ClearCitizenRetreatMotion();
        _citizenRetreat.Reset(); // ordinary recovery/party orders own motion now
        _citizenRetreatRetry.Reset();
        // AC-10 R5: genuine party/recovery authority also clears the
        // hunting-group episode age. The map leg counts only after a
        // retreat set _citizenRetreatMap: with its zero default the
        // block fires for every never-retreated citizen and would
        // reset the episode on every tick.
        if (!me->IsAlive() || _following || _held || _assistTargetGuid ||
            (botEntry && botEntry->recruiterAccountId) ||
            (_citizenRetreatMap != 0 && me->GetMapId() != _citizenRetreatMap))
        {
            _citizenGroupLeaderLostMs = 0;
            _citizenGroupFollowProgress.Reset();
        }
    }
    else if (!_citizenRetreat.Active() && !me->IsInCombat() && !me->GetVictim() &&
             !GetAliveHeldTarget() && me->GetMaxHealth() &&
             me->GetHealth() * 100 >= me->GetMaxHealth() *
                 Companion::CitizenRecovery::kHuntReadyHealthPercent)
        _citizenRetreatRetry.Reset(); // genuine health/combat recovery ends this episode

    if (_socialCooldownMs > diff)
        _socialCooldownMs -= diff;
    else
        _socialCooldownMs = 0;

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
        RefreshCitizenProgression(true);
        AutoLearnSpellsForLevel();
        AutoAssignCitizenTalents();
        AutoEquipForLevel();
        PersistCitizenJournal(_progressionTravelActive ? 1 : 0);
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

    // Repeated deaths have already escalated this citizen to a durable
    // population-manager relocation. Do not let combat, group following,
    // repair, or ordinary retreat delay that safety handoff for another loop.
    // DeleteBot is the existing safe logout path; the manager will queue the
    // replacement placement on its next bounded population pass.
    if (IsZoneCitizen() && botEntry && botEntry->zoneWorldSafetyRelocation &&
        (!me->GetGroup() || sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup())))
    {
        sLog.outString("[ZoneCitizen][Survival] leaving-danger guid:%u map:%u zone:%u reason:relocation-pending",
                       me->GetGUIDLow(), me->GetMapId(), me->GetZoneId());
        if (sPlayerBotMgr.DeleteBot(me->GetGUIDLow()))
            return;
        sLog.outString("[ZoneCitizen][Survival] relocation-logout-failed guid:%u map:%u zone:%u",
                       me->GetGUIDLow(), me->GetMapId(), me->GetZoneId());
    }

    if (UpdateCompanion(diff))
        return;

    if (UpdateCitizenRetreat(diff))
        return;

    if (TryLootDefeatedTarget())
        return;

    // MVP-006: declared quest state machine; while true the bot is
    // moving for the quest (accept or turn-in) and normal behavior is
    // skipped for this tick.
    if (UpdateQuestPhases(diff))
        return;

    if (IsZoneCitizen() && me->GetClass() == CLASS_HUNTER && MaintainCitizenHunterPet(diff))
        return;

    // Persistent bots share one bounded off-duty travel-and-hunt loop.
    // Owned companions wait for their owner to be online; citizens need no
    // owner. Party orders, hold, combat, recovery and loot preempt this job.
    bool const citizenOffDuty = IsZoneCitizen() && botEntry &&
        !botEntry->recruiterAccountId;
    bool const ownedOffDuty = botEntry && botEntry->ownerAccountId &&
        FindOwnerByAccount();
    Group* const citizenGroup = me->GetGroup();
    bool const autonomousGroup = citizenGroup &&
        sPlayerBotMgr.IsAutonomousCitizenGroup(citizenGroup);
    bool const autonomousGroupLeader = autonomousGroup &&
        citizenGroup->GetLeaderGuid() == me->GetObjectGuid();
    if (botEntry && botEntry->persistent && autonomousGroup &&
        !_following && !_held && !_assistTargetGuid &&
        UpdateCitizenGroupActivity(diff))
        return;
    if (BotRepairStep(diff))
        return;
    if (botEntry && botEntry->persistent && (!citizenGroup || autonomousGroupLeader) &&
        !_following && !_held && !_assistTargetGuid &&
        (citizenOffDuty || ownedOffDuty) && UpdateIndependentActivity(diff))
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
            if (TryCitizenDisengage(victim))
                return;
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] fighting GUID:%u victim:%u dist:%.2f",
                               me->GetGUIDLow(), victim->GetGUIDLow(), me->GetDistance(victim));
            UpdateCombatPursuit(victim, diff);
            if (me->CanReachWithMeleeAutoAttack(victim))
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
            if (TryCitizenDisengage(held))
                return;
            if (me->GetDistance(held) > 35.0f)
            {
                ClearTarget();
            }
            else
            {
                UpdateCombatPursuit(held, diff);
                if (me->CanReachWithMeleeAutoAttack(held))
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
                    me->Attack(held, true);
                }
            }
        }
        else
        {
            // All persistent bots use the same citizen acquisition policy
            // once they are not in a party. A party order, follow, defend,
            // combat, recovery or loot state has already returned above.
            if (Unit* target = sPlayerBotMgr.IsAmbientAcquireEnabled()
                                 ? me->SelectNearestTarget(30.0f) : nullptr)
            {
                // Autonomous companions never initiate PvP. A hostile player
                // may still be the current victim while defending; this guard
                // applies only to idle target acquisition.
                if (!target->IsPlayer())
                {
                    if (target->GetTypeId() == TYPEID_UNIT)
                        RememberCombatTarget(target);
                    me->Attack(target, true);
                    UpdateCombatPursuit(target, diff);
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
            if (IsOwnedCompanion() && me->GetGroup())
            {
                // KAP-558 hardening: default owner-follow replaces the
                // auto-hunt wander. Close distance when the owner is far
                // on this map; otherwise hold position. Stays within
                // loot distance of its kills so the owner shares party
                // XP, and never roams into solo fights.
                Player* owner = FindOwnerByAccount();
                if (owner && owner->GetMapId() == me->GetMapId() &&
                    me->GetDistance(owner) > _personalityChaseDist)
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
                _independentHomeSet = false;
            }
            else if (IsOwnedCompanion())
            {
                // Off-duty companions do not trail the player after leaving
                // the party. This first autonomous activity is local and
                // non-aggressive, and pauses while the owner is offline;
                // solo combat needs separate validation.
                if (!FindOwnerByAccount())
                {
                    _wanderTimer = urand(8000, 15000);
                    return;
                }
                if (_worldRestUntilMs > WorldTimer::getMSTime())
                {
                    _wanderTimer = 1000;
                    return;
                }
                if (!_independentHomeSet)
                {
                    _independentHomeX = me->GetPositionX();
                    _independentHomeY = me->GetPositionY();
                    _independentHomeZ = me->GetPositionZ();
                    _independentHomeSet = true;
                }
                _wanderTimer = urand(8000, 15000);
                float x = me->GetPositionX();
                float y = me->GetPositionY();
                float z = me->GetPositionZ();
                if (me->GetDistance(_independentHomeX, _independentHomeY,
                                    _independentHomeZ) > 35.0f)
                {
                    x = _independentHomeX;
                    y = _independentHomeY;
                    z = _independentHomeZ;
                }
                else if (!me->GetMap() ||
                         !me->GetMap()->GetWalkRandomPosition(nullptr, x, y, z, 8.0f))
                    return;
                me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
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
    if (_combatPursuitNudging && me && me->GetMotionMaster() &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        me->GetMotionMaster()->MovementExpired();
    _combatPursuitTargetGuid = 0;
    _combatPursuitStalledMs = 0;
    _combatPursuitNudging = false;
    _combatPursuitNudgeMs = 0;
}

void PlayerBotAI::UpdateCombatPursuit(Unit* target, uint32 diff)
{
    if (!me || !me->GetMap() || !target || !target->IsInWorld() ||
        target->GetMap() != me->GetMap() || !target->IsAlive())
    {
        if (me && me->GetMotionMaster() && _combatPursuitNudging &&
            me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            me->GetMotionMaster()->MovementExpired();
        _combatPursuitTargetGuid = 0;
        _combatPursuitStalledMs = 0;
        _combatPursuitNudging = false;
        _combatPursuitNudgeMs = 0;
        return;
    }

    MotionMaster* motion = me->GetMotionMaster();
    if (me->CanReachWithMeleeAutoAttack(target))
    {
        if (_combatPursuitNudging &&
            motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            motion->MovementExpired();
        else if (motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            motion->MovementExpired();
        _combatPursuitTargetGuid = 0;
        _combatPursuitStalledMs = 0;
        _combatPursuitNudging = false;
        _combatPursuitNudgeMs = 0;
        return;
    }

    uint32 const targetGuid = target->GetGUIDLow();
    float const distance = me->GetDistance(target);
    if (_combatPursuitTargetGuid != targetGuid)
    {
        _combatPursuitTargetGuid = targetGuid;
        _combatPursuitLastDistance = distance;
        _combatPursuitStalledMs = 0;
        _combatPursuitNudging = false;
        _combatPursuitNudgeMs = 0;
        motion->MoveChase(target);
        return;
    }

    if (_combatPursuitNudging)
    {
        float const nudgeDistance = me->GetDistance(
            _combatPursuitNudgeX, _combatPursuitNudgeY, _combatPursuitNudgeZ);
        bool const nudgeExpired = _combatPursuitNudgeMs <= diff ||
            motion->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE;
        _combatPursuitNudgeMs = _combatPursuitNudgeMs > diff
            ? _combatPursuitNudgeMs - diff : 0;
        if (nudgeDistance < 3.0f || nudgeExpired)
        {
            _combatPursuitNudging = false;
            _combatPursuitStalledMs = 0;
            _combatPursuitLastDistance = distance;
            motion->MoveChase(target);
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot][CombatPath] resume GUID:%u target:%u dist:%.1f",
                               me->GetGUIDLow(), targetGuid, distance);
        }
        return;
    }

    if (_combatPursuitLastDistance - distance >= 1.0f)
    {
        _combatPursuitLastDistance = distance;
        _combatPursuitStalledMs = 0;
    }
    else
        _combatPursuitStalledMs = std::min<uint32>(
            _combatPursuitStalledMs + diff, 30000u);

    if (motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
    {
        _combatPursuitStalledMs = 0;
        motion->MoveChase(target);
        return;
    }
    if (_combatPursuitStalledMs < 8000)
        return;

    // A chase that makes no progress may be facing a cliff, steep slope, or
    // awkward navmesh edge. Try a short walkable sidestep, then resume the
    // normal chase so the core recalculates a route from different ground.
    float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
    float bestTargetDistance = distance + 6.0f;
    bool foundNudge = false;
    for (uint32 attempt = 0; attempt < 12; ++attempt)
    {
        float x = me->GetPositionX();
        float y = me->GetPositionY();
        float z = me->GetPositionZ();
        if (!me->GetMap()->GetWalkRandomPosition(nullptr, x, y, z, 8.0f))
            continue;
        float const fromBotX = x - me->GetPositionX();
        float const fromBotY = y - me->GetPositionY();
        if (fromBotX * fromBotX + fromBotY * fromBotY < 3.0f * 3.0f)
            continue;
        float const dx = x - target->GetPositionX();
        float const dy = y - target->GetPositionY();
        float const dz = z - target->GetPositionZ();
        float const candidateDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (candidateDistance >= bestTargetDistance)
            continue;
        bestX = x;
        bestY = y;
        bestZ = z;
        bestTargetDistance = candidateDistance;
        foundNudge = true;
    }

    _combatPursuitStalledMs = 0;
    if (foundNudge)
    {
        _combatPursuitNudging = true;
        _combatPursuitNudgeX = bestX;
        _combatPursuitNudgeY = bestY;
        _combatPursuitNudgeZ = bestZ;
        _combatPursuitNudgeMs = 8000;
        motion->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][CombatPath] nudge GUID:%u target:%u dist:%.1f",
                           me->GetGUIDLow(), targetGuid, distance);
    }
    else
    {
        motion->MoveChase(target);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][CombatPath] repath GUID:%u target:%u dist:%.1f no-nudge",
                           me->GetGUIDLow(), targetGuid, distance);
    }
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
        // PORT-024 (KAP-558): owned companions progress their
        // equipment from the items just received; ambient bots
        // keep the legacy loot-only path.
        if (IsOwnedCompanion())
            EvaluateReceivedEquipment(itemCounts);
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

// ---------------------------------------------------------------------------
// PORT-024 (KAP-558): equipment progression from loot legitimately
// received by owned companions.
//
// EvaluateReceivedEquipment receives the (entry, count before this
// loot window) pairs captured before AutoStoreLoot. An entry whose
// saved count did not grow was eligible loot the bags could not
// accept: it raises the bounded inventory-pressure state for
// PORT-025 and nothing is ever deleted to make room. An entry whose
// count grew was received; up to the delta of its instances in the
// bags is evaluated (bounded, idempotent: re-evaluating a retained
// instance can only repeat its earlier verdict).
//
// EvaluateReceivedInstance asks the authoritative CanEquipItem for
// legality and slot (class, level, skill, proficiency, unique rules,
// live combat state) and Companion::Equipment for the verdict: only
// a strict upgrade moves, via SwapItem, the received instance into
// the equipment slot; the replaced gear returns to the bag slot the
// new item came from (a 1:1 exchange, so full bags can never block
// an upgrade). Illegal or non-upgrade items stay in the bag
// untouched. The model never supplies item IDs, scores or equip
// commands.

static Companion::Equipment::ItemStats EquipmentStatsFromProto(ItemPrototype const* proto, uint32 playerLevel)
{
    Companion::Equipment::ItemStats s;
    s.playerLevel = playerLevel;
    if (proto)
    {
        s.requiredLevel = proto->RequiredLevel;
        s.itemLevel = proto->ItemLevel;
        s.quality = proto->Quality;
    }
    return s;
}

bool PlayerBotAI::ShouldNeedLoot(ItemPrototype const* proto) const
{
    if (!me || !proto || !IsLootEligibleBot() ||
        me->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;

    uint8 const slot = me->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot >= EQUIPMENT_SLOT_END)
        return false;

    uint32 const level = me->GetLevel();
    uint32_t const candidate = Companion::Equipment::Score(EquipmentStatsFromProto(proto, level));
    Item const* equipped = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    uint32_t const current = equipped
        ? Companion::Equipment::Score(EquipmentStatsFromProto(equipped->GetProto(), level))
        : 0;
    return Companion::Equipment::Compare(candidate, current) ==
        Companion::Equipment::Verdict::Equip;
}

void PlayerBotAI::EvaluateReceivedEquipment(std::vector<std::pair<uint32, uint32>> const& itemCounts)
{
    if (!me || !me->GetMap())
        return;

    std::map<uint32, uint32> before;
    for (std::vector<std::pair<uint32, uint32>>::const_iterator itr = itemCounts.begin();
         itr != itemCounts.end(); ++itr)
        before.emplace(itr->first, itr->second);   // first occurrence wins

    for (std::map<uint32, uint32>::const_iterator kv = before.begin(); kv != before.end(); ++kv)
    {
        uint32 const entry = kv->first;
        uint32 const after = me->GetItemCount(entry);
        if (after <= kv->second)
        {
            // Eligible loot the inventory could not accept: raise the
            // bounded pressure state; the item stays where the normal
            // inventory rules left it (on the corpse).
            if (_inventoryPressure < 8)
                ++_inventoryPressure;
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] equipment pressure GUID:%u item:%u stored:0",
                               me->GetGUIDLow(), entry);
            continue;
        }

        uint32 const received = after - kv->second;
        uint32 evaluated = 0;
        // Bounded scan: bag0 item slots, then any sub-bags.
        for (uint8 slot = INVENTORY_SLOT_ITEM_START;
             slot < INVENTORY_SLOT_ITEM_END && evaluated < received; ++slot)
        {
            Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || item->GetEntry() != entry)
                continue;
            ++evaluated;
            EvaluateReceivedInstance(item, INVENTORY_SLOT_BAG_0, slot);
        }
        for (uint8 bag = INVENTORY_SLOT_BAG_START;
             bag < INVENTORY_SLOT_BAG_END && evaluated < received; ++bag)
        {
            for (uint8 slot = 0; slot < 36 && evaluated < received; ++slot)
            {
                Item* item = me->GetItemByPos(bag, slot);
                if (!item || item->GetEntry() != entry)
                    continue;
                ++evaluated;
                EvaluateReceivedInstance(item, bag, slot);
            }
        }
    }
}

void PlayerBotAI::EvaluateReceivedInstance(Item* item, uint8 bag, uint8 slot)
{
    ItemPrototype const* proto = item ? item->GetProto() : nullptr;
    if (!proto)
        return;

    // Authoritative legality + slot. swap=true: the slot may be
    // occupied; the replaced gear returns to the bag slot this item
    // came from. Same checks the packet handlers apply (not_loading
    // defaults to true, as in AutoEquipForLevel). CanEquipItem packs
    // dest as (INVENTORY_SLOT_BAG_0 << 8) | equipmentSlot; the low byte
    // is the equipment slot index.
    uint16 dest = 0;
    InventoryResult const res = me->CanEquipItem(NULL_SLOT, dest, item, true);
    if (res != EQUIP_ERR_OK || dest == NULL_SLOT)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] equipment skip illegal GUID:%u item:%u err:%u",
                           me->GetGUIDLow(), item->GetEntry(), uint32(res));
        return;
    }
    uint8 const eslot = dest & 0xFF;
    if (eslot >= EQUIPMENT_SLOT_END)
    {
        // Not an equipment slot (e.g. a bag position): normal
        // storage already placed the item; the equipment policy does
        // not move bags.
        return;
    }

    uint32 const level = me->GetLevel();
    Item* equipped = me->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot);
    uint32_t const newScore = Companion::Equipment::Score(EquipmentStatsFromProto(proto, level));
    uint32_t const eqScore = equipped
        ? Companion::Equipment::Score(EquipmentStatsFromProto(equipped->GetProto(), level))
        : 0;

    if (Companion::Equipment::Compare(newScore, eqScore) != Companion::Equipment::Verdict::Equip)
    {
        // Sidegrade or downgrade: equipment unchanged, item retained.
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] equipment keep GUID:%u item:%u new:%u eq:%u",
                           me->GetGUIDLow(), item->GetEntry(), uint32_t(newScore), uint32_t(eqScore));
        return;
    }

    me->SwapItem(uint16((bag << 8) | slot), dest);

    Item* now = me->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot);
    if (now && now->GetEntry() == item->GetEntry())
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] equipment upgraded GUID:%u slot:%u item:%u new:%u eq:%u",
                           me->GetGUIDLow(), uint32(eslot), item->GetEntry(), uint32_t(newScore), uint32_t(eqScore));
    }
    else
    {
        // The core refused the move after the pre-checks (transient
        // world state): the item stays in the bag, equipment unchanged.
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] equipment swap denied GUID:%u slot:%u item:%u",
                           me->GetGUIDLow(), uint32(eslot), item->GetEntry());
    }
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

void PlayerBotAI::ClearCitizenGroupState()
{
    _citizenGroupLeaderLostMs = 0;
    _citizenGroupFollowProgress.Reset();
}

bool PlayerBotAI::CitizenCombatReady() const
{
    // Voluntary combat (pulling a solo hunt or joining a hunting group)
    // requires hunt-ready health; below it the citizen rests or recovers.
    // Necessary self-defense is never gated: an already engaged citizen
    // keeps fighting through the ordinary combat path.
    // AC-10 R3: readiness also requires a free recovery commitment. A
    // citizen resting after a retreat, still retreating, deferred by
    // repeated retreat failures, or holding a corpse pending loot is
    // not pulled into voluntary combat until that commitment ends.
    if (!me || !me->IsAlive() || me->GetMaxHealth() == 0)
        return false;
    if (me->GetHealth() * 100u < me->GetMaxHealth() *
        Companion::CitizenRecovery::kHuntReadyHealthPercent)
        return false;
    if (_worldRestUntilMs > WorldTimer::getMSTime())
        return false;
    if (_citizenRetreat.Active())
        return false;
    if (_citizenRetreatRetry.Deferred())
        return false;
    if (_targets.corpse != 0)
        return false;
    return true;
}

bool PlayerBotAI::JoinCitizenCombat(Unit* target)
{
    if (!me || !IsZoneCitizen() || !botEntry || !botEntry->persistent ||
        botEntry->ownerAccountId || botEntry->recruiterAccountId || !me->IsAlive() ||
        !CitizenCombatReady() ||
        (me->GetGroup() && !sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup())) ||
        _following || _held || _assistTargetGuid || !target ||
        !target->IsAlive() || target->GetTypeId() != TYPEID_UNIT ||
        target->GetMap() != me->GetMap() || target->GetZoneId() != me->GetZoneId() ||
        !me->CanAttack(target) ||
        // AC-10 R3: a suppressed failed hunt target is not a group
        // re-engagement target; the suppression window owns it.
        (_failedHuntMs && target->GetGUIDLow() == _failedHuntGuid))
        return false;

    RememberCombatTarget(target);
    return me->Attack(target, true);
}

bool PlayerBotAI::UpdateCitizenGroupActivity(uint32 diff)
{
    Group* group = me ? me->GetGroup() : nullptr;
    if (!me || !group || !sPlayerBotMgr.IsAutonomousCitizenGroup(group))
    {
        // AC-10 R5: a new group identity (joined, left, disbanded)
        // starts a fresh episode; no timer survives the change.
        _citizenGroupLeaderLostMs = 0;
        _citizenGroupFollowProgress.Reset();
        return false;
    }

    Player* leader = sObjectAccessor.FindPlayer(group->GetLeaderGuid());
    PlayerBotAI* leaderAI = leader ? dynamic_cast<PlayerBotAI*>(leader->AI()) : nullptr;
    bool const leaderReachable = leader && leader->IsInWorld() && leader->IsAlive() &&
        leaderAI && leaderAI->IsZoneCitizen();
    // AC-10 R2: reachability is not usability. A living leader on
    // another map cannot be followed from this one, so it enters the
    // same bounded episode; on expiry the follower leaves instead of
    // disbanding the leader's group.
    bool const leaderUsable = leaderReachable && leader->GetMapId() == me->GetMapId();
    if (!leaderUsable)
    {
        // A dead or unavailable leader must not suspend the member's own
        // survival. A threatened member falls through immediately to the
        // ordinary combat/retreat evaluation; an unthreatened member waits
        // for the leader up to the bounded regroup deadline, then disbands
        // and resumes solo citizen activity.
        switch (Companion::CitizenRecovery::EvaluateLeaderLoss(
            _citizenGroupLeaderLostMs, me->IsInCombat() || me->GetVictim() != nullptr))
        {
        case Companion::CitizenRecovery::LeaderLoss::FallThrough:
            // AC-10 R5: self-defense does not cancel the missing-leader
            // episode; the age is preserved and the outcome resolves on
            // the next safe tick.
            return false;
        case Companion::CitizenRecovery::LeaderLoss::Disband:
            if (leaderReachable)
            {
                // AC-10 R2: the leader is fine, only this follower
                // cannot reach it. Leave the group and resume solo
                // activity; the leader keeps its group.
                sLog.outString("[ZoneCitizen][HuntingGroup] leave cross-map group:%u member:%u leader-guid:%u leader-map:%u member-map:%u waited-ms:%u deadline-ms:%u",
                               group->GetId(), me->GetGUIDLow(),
                               group->GetLeaderGuid().GetCounter(),
                               leader->GetMapId(), me->GetMapId(),
                               _citizenGroupLeaderLostMs,
                               Companion::CitizenRecovery::kLeaderLostDeadlineMs);
                me->Say("My hunting party is out of reach. I am resuming my own hunt.", LANG_UNIVERSAL);
                if (!MotionIdle())
                {
                    me->GetMotionMaster()->Clear(false);
                    me->GetMotionMaster()->MoveIdle();
                }
                _citizenGroupLeaderLostMs = 0;
                _citizenGroupFollowProgress.Reset();
                group->RemoveMember(me->GetObjectGuid(), GROUP_LEAVE);
                return true;
            }
            sLog.outString("[ZoneCitizen][HuntingGroup] disband leader-unavailable group:%u member:%u leader-guid:%u waited-ms:%u deadline-ms:%u",
                           group->GetId(), me->GetGUIDLow(),
                           group->GetLeaderGuid().GetCounter(),
                           _citizenGroupLeaderLostMs,
                           Companion::CitizenRecovery::kLeaderLostDeadlineMs);
            me->Say("I am resuming my own hunt without the group.", LANG_UNIVERSAL);
            _citizenGroupLeaderLostMs = 0;
            _citizenGroupFollowProgress.Reset();
            group->Disband(true, me->GetObjectGuid());
            return true;
        case Companion::CitizenRecovery::LeaderLoss::Wait:
            break;
        }
        // Saturate the wait so a huge diff cannot wrap it backwards.
        if (diff >= Companion::CitizenRecovery::kLeaderLostDeadlineMs ||
            _citizenGroupLeaderLostMs >=
                Companion::CitizenRecovery::kLeaderLostDeadlineMs - diff)
            _citizenGroupLeaderLostMs = Companion::CitizenRecovery::kLeaderLostDeadlineMs;
        else
            _citizenGroupLeaderLostMs += diff;
        return true;
    }
    _citizenGroupLeaderLostMs = 0;

    bool levelSpread = false;
    for (auto const& slot : group->GetMemberSlots())
    {
        Player* member = sObjectAccessor.FindPlayer(slot.guid);
        if (member && std::abs(int(member->GetLevel()) - int(leader->GetLevel())) > 3)
        {
            levelSpread = true;
            break;
        }
    }
    if (levelSpread)
    {
        sLog.outString("[ZoneCitizen][HuntingGroup] disband level-spread group:%u member:%u leader:%u level:%u",
                       group->GetId(), me->GetGUIDLow(), leader->GetGUIDLow(), leader->GetLevel());
        me->Say("We are splitting up as our levels pull us toward different challenges.", LANG_UNIVERSAL);
        group->Disband(true, group->GetLeaderGuid());
        return true;
    }

    // Recovery has priority over all voluntary group duty. Do not follow a
    // leader or join its target while resting, retreat-deferred, or waiting
    // on corpse loot. If already under attack, fall through so self-defense
    // and retreat remain available.
    if (Companion::CitizenRecovery::ShouldPauseGroupDuty(
            CitizenCombatReady(), me->IsInCombat() || me->GetVictim() != nullptr))
        return true;

    if (leader != me)
    {
        Unit* target = leaderAI->GetAliveHeldTarget();
        if (target && target->IsAlive())
        {
            if (me->GetVictim() == target || (me->IsInCombat() && me->GetVictim()))
                return false;
            JoinCitizenCombat(target);
            return false;
        }
        if (me->IsInCombat() || me->GetVictim())
            return false;

        _citizenGroupFollowTimer = _citizenGroupFollowTimer > diff
            ? _citizenGroupFollowTimer - diff : 0;
        float const distance = me->GetDistance(leader);
        // AC-10 R2: a same-map follower that cannot close the
        // distance to a usable leader (blocked path, lagging member)
        // gets a bounded progress deadline; on expiry it leaves the
        // group and resumes solo activity instead of orbiting forever.
        bool followStalled = false;
        if (distance <= 6.0f)
            _citizenGroupFollowProgress.Reset();
        else
            followStalled = _citizenGroupFollowProgress.Update(
                diff, me->GetPositionX(), me->GetPositionY());
        if (followStalled)
        {
            sLog.outString("[ZoneCitizen][HuntingGroup] leave follow-stall group:%u member:%u leader-guid:%u distance:%.1f stall-ms:%u deadline-ms:%u",
                           group->GetId(), me->GetGUIDLow(),
                           group->GetLeaderGuid().GetCounter(), distance,
                           _citizenGroupFollowProgress.StalledMs(),
                            Companion::CitizenRecovery::kFollowStallDeadlineMs);
            me->Say("My hunting party is out of reach. I am resuming my own hunt.", LANG_UNIVERSAL);
            _citizenGroupFollowProgress.Reset();
            _citizenGroupLeaderLostMs = 0;
            if (!MotionIdle())
            {
                me->GetMotionMaster()->Clear(false);
                me->GetMotionMaster()->MoveIdle();
            }
            group->RemoveMember(me->GetObjectGuid(), GROUP_LEAVE);
            return true;
        }
        if (distance > 6.0f && !_citizenGroupFollowTimer)
        {
            float const angle = leader->GetOrientation() + M_PI_F;
            float x = leader->GetPositionX() + std::cos(angle) * 3.0f;
            float y = leader->GetPositionY() + std::sin(angle) * 3.0f;
            float z = leader->GetPositionZ();
            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
            _citizenGroupFollowTimer = 2500;
        }
        else if (distance <= 6.0f && !MotionIdle())
        {
            me->GetMotionMaster()->Clear(false);
            me->GetMotionMaster()->MoveIdle();
        }
        return true;
    }
    return false;
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
    _encounter.Reset(); // BL-002: encounters never span a login
    _lastLevel = me ? me->GetLevel() : 0;
    LoadCitizenJournal();
    if (me && IsZoneCitizen())
    {
        // PlayerBotAI is retained by the roster entry across logins. Rebuild
        // this episode from the current position so a safe respawn does not
        // keep a stale relocation request or zone-wide retreat timer.
        _citizenSafetyRelocationRequested = false;
        _citizenDeathsAtSpot = 0;
        _citizenDeathMap = 0;
        _citizenDeathZone = 0;
        _citizenDeathX = 0.0f;
        _citizenDeathY = 0.0f;
        _citizenDangerZoneUntilMs = 0;
        QueryResult* danger = CharacterDatabase.PQuery(
            "SELECT map_id,zone_id,cell_x,cell_y,deaths,"
            "safe_until>UNIX_TIMESTAMP(),"
            "GREATEST(0,safe_until-UNIX_TIMESTAMP()),"
            "GREATEST(0,UNIX_TIMESTAMP()-updated_at) "
            "FROM bot_citizen_danger_memory "
            "WHERE char_guid=%u AND map_id=%u AND zone_id=%u "
            "AND safe_until>UNIX_TIMESTAMP() ORDER BY safe_until DESC",
            me->GetGUIDLow(), me->GetMapId(), me->GetZoneId());
        if (danger)
        {
            // Restore the active danger-cell memory, but only re-arm safety
            // relocation when this login position is still inside an active
            // remembered cell and its configured buffer. A safe anchor
            // elsewhere in the same zone must not recreate the request, and
            // a missing query result must not invent one.
            struct LoginDangerRow
            {
                Companion::CitizenRecovery::RememberedDangerCell cell;
                uint32 mapId;
                uint32 zoneId;
                uint32 remainingSeconds;
            };
            std::vector<LoginDangerRow> rows;
            std::vector<Companion::CitizenRecovery::RememberedDangerCell> cells;
            do
            {
                Field* fields = danger->Fetch();
                rows.push_back({
                    {fields[2].GetInt32(), fields[3].GetInt32(),
                     fields[4].GetUInt8(), fields[5].GetUInt32() != 0,
                     fields[7].GetUInt32()},
                    fields[0].GetUInt32(), fields[1].GetUInt32(),
                    fields[6].GetUInt32()});
                cells.push_back(rows.back().cell);
            } while (danger->NextRow());
            auto const triggered =
                Companion::CitizenRecovery::FirstUnsafeRememberedCell(
                    me->GetPositionX(), me->GetPositionY(), cells);
            if (triggered)
            {
                LoginDangerRow const& row = rows[*triggered];
                // Keep the death memory, retreat timer, and the bot entry
                // exclusion synchronized with the cell that actually
                // triggered the relocation.
                _citizenDeathMap = row.mapId;
                _citizenDeathZone = row.zoneId;
                _citizenDeathsAtSpot = static_cast<uint8>(row.cell.deaths);
                _citizenDangerZoneUntilMs =
                    WorldTimer::getMSTime() + row.remainingSeconds * 1000;
                // A restarted citizen must not forget an active, durable
                // warning and immediately return to hunting the same fatal
                // ground.
                if (!me->GetGroup())
                {
                    _citizenSafetyRelocationRequested = true;
                    if (botEntry)
                    {
                        botEntry->zoneWorldSafetyRelocation = true;
                        botEntry->zoneWorldSafetyExcludeMap = _citizenDeathMap;
                        botEntry->zoneWorldSafetyExcludeZone = _citizenDeathZone;
                    }
                }
            }
            delete danger;
        }
    }
    RefreshCitizenProgression(true);
    AutoLearnSpellsForLevel();
    AutoAssignCitizenTalents();
    AutoEquipForLevel();
    RepairBotEquipmentAtLogin();
    InitQuestState();
    BackfillMirrorQuestMarkers(); // KAP-558 review: legacy in-flight mirrors
    // A login is one authoritative observation of the citizen's current
    // zone; level updates and route-leg retries must not inflate this count.
    PersistCitizenJournal(_progressionTravelActive ? 1 : 0, true);
}

void PlayerBotAI::OnLevelUp()
{
    _lastLevel = me ? me->GetLevel() : _lastLevel;
    RefreshCitizenProgression(true);
    AutoLearnSpellsForLevel();
    AutoAssignCitizenTalents();
    AutoEquipForLevel();
    PersistCitizenJournal(_progressionTravelActive ? 1 : 0);
}

bool PlayerBotAI::IsLootEligibleBot() const
{
    return IsZoneCitizen() || IsOwnedCompanion();
}

void PlayerBotAI::LoadCitizenJournal()
{
    if (!me || !IsZoneCitizen() || !botEntry || !botEntry->persistent)
        return;
    QueryResult* result = CharacterDatabase.PQuery(
        "SELECT intent, current_job, progression_band, last_level, target_map, target_zone, "
        "target_x, target_y, target_z, visited_zones, route_failure_map, route_failure_zone, "
        "route_failure_x, route_failure_y, route_failure_z, route_failure_reason, "
        "GREATEST(0,route_failure_until-UNIX_TIMESTAMP()), route_search_cursor "
        "FROM bot_citizen_journal "
        "WHERE char_guid = %u", me->GetGUIDLow());
    if (!result)
        return;
    Field* fields = result->Fetch();
    uint8 const intent = fields[0].GetUInt8();
    _citizenActivityIntent = fields[1].GetUInt8() <= 4 ? fields[1].GetUInt8() : 0;
    uint8 const band = fields[2].GetUInt8();
    uint8 const recordedLevel = fields[3].GetUInt8();
    uint32 const targetMap = fields[4].GetUInt32();
    uint32 const targetZone = fields[5].GetUInt32();
    _progressionFinalX = fields[6].GetFloat();
    _progressionFinalY = fields[7].GetFloat();
    _progressionFinalZ = fields[8].GetFloat();
    _citizenVisitedZones = fields[9].GetUInt32();
    _progressionBlockedAnchor.mapId = fields[10].GetUInt32();
    _progressionBlockedAnchor.zoneId = fields[11].GetUInt32();
    _progressionBlockedAnchor.x = fields[12].GetFloat();
    _progressionBlockedAnchor.y = fields[13].GetFloat();
    _progressionBlockedAnchor.z = fields[14].GetFloat();
    _progressionBlockedReason = static_cast<Companion::CitizenTravel::RouteFailureReason>(
        fields[15].GetUInt8() <= (uint8)Companion::CitizenTravel::RouteFailureReason::AnchorBlocked
            ? fields[15].GetUInt8() : 0);
    _progressionBlockedRemainingMs =
        std::min<uint32>(Companion::CitizenTravel::kBlockedAnchorCooldownMs / 1000,
                         fields[16].GetUInt32()) * 1000;
    _progressionCandidateCursor = fields[17].GetUInt32();
    _progressionBlockedAnchor.active = _progressionBlockedRemainingMs > 0 &&
        _progressionBlockedAnchor.mapId == me->GetMapId() &&
        _progressionBlockedAnchor.zoneId != 0 &&
        _progressionBlockedReason != Companion::CitizenTravel::RouteFailureReason::None;
    delete result;

    uint8 const currentBand = static_cast<uint8>((std::max<uint32>(1, me->GetLevel()) - 1) / 10);
    uint32 const productiveMinimum = me->GetLevel() > 3 ? me->GetLevel() - 3 : 1;
    uint32 const targetProductiveShare = sPlayerBotMgr.GetCitizenZoneProductiveSharePercent(
        targetMap, targetZone, productiveMinimum, me->GetLevel());
    if (recordedLevel == me->GetLevel() && band == currentBand &&
        Companion::CitizenTravel::CanResumeRoute(intent, targetMap, me->GetMapId(),
            targetZone, me->GetZoneId(), targetProductiveShare,
            PlayerBotMgr::CitizenZoneMinimumProductiveSharePercent))
    {
        _progressionTargetZone = targetZone;
        _progressionMap = me->GetMapId();
        _progressionTravelActive = true;
        sLog.outString("[ZoneCitizen][Journal] resume guid:%u target-zone:%u visits:%u",
                       me->GetGUIDLow(), targetZone, _citizenVisitedZones);
    }
    else if (intent == 1)
        sLog.outString("[ZoneCitizen][Journal] discarded stale route guid:%u target-zone:%u",
                       me->GetGUIDLow(), targetZone);
}

void PlayerBotAI::PersistCitizenJournal(uint8 intent, bool recordVisit)
{
    if (!me || !IsZoneCitizen() || !botEntry || !botEntry->persistent)
        return;
    CharacterDatabase.DirectPExecute(
        "INSERT INTO bot_citizen_journal "
        "(char_guid, intent, current_job, progression_band, last_level, current_map, current_zone, "
        "target_map, target_zone, target_x, target_y, target_z, visited_zones, "
        "route_failure_map, route_failure_zone, route_failure_x, route_failure_y, route_failure_z, "
        "route_failure_until, route_failure_reason, route_search_cursor, updated_at) "
        "VALUES (%u,%u,%u,%u,%u,%u,%u,%u,%u,%f,%f,%f,%u,%u,%u,%f,%f,%f,UNIX_TIMESTAMP()+%u,%u,%u,UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE intent=VALUES(intent), current_job=VALUES(current_job), progression_band=VALUES(progression_band), "
        "last_level=VALUES(last_level), current_map=VALUES(current_map), current_zone=VALUES(current_zone), "
        "target_map=VALUES(target_map), target_zone=VALUES(target_zone), target_x=VALUES(target_x), "
        "target_y=VALUES(target_y), target_z=VALUES(target_z), visited_zones=VALUES(visited_zones), "
        "route_failure_map=VALUES(route_failure_map), route_failure_zone=VALUES(route_failure_zone), "
        "route_failure_x=VALUES(route_failure_x), route_failure_y=VALUES(route_failure_y), "
        "route_failure_z=VALUES(route_failure_z), route_failure_until=VALUES(route_failure_until), "
        "route_failure_reason=VALUES(route_failure_reason), "
        "route_search_cursor=VALUES(route_search_cursor), "
        "updated_at=VALUES(updated_at)",
        me->GetGUIDLow(), intent, _citizenActivityIntent, _progressionBand, me->GetLevel(), me->GetMapId(), me->GetZoneId(),
        intent == 1 ? me->GetMapId() : 0, intent == 1 ? _progressionTargetZone : 0,
        intent == 1 ? _progressionFinalX : 0.0f, intent == 1 ? _progressionFinalY : 0.0f,
        intent == 1 ? _progressionFinalZ : 0.0f, _citizenVisitedZones,
        _progressionBlockedAnchor.active ? _progressionBlockedAnchor.mapId : 0,
        _progressionBlockedAnchor.active ? _progressionBlockedAnchor.zoneId : 0,
        _progressionBlockedAnchor.active ? _progressionBlockedAnchor.x : 0.0f,
        _progressionBlockedAnchor.active ? _progressionBlockedAnchor.y : 0.0f,
        _progressionBlockedAnchor.active ? _progressionBlockedAnchor.z : 0.0f,
        _progressionBlockedAnchor.active ? (_progressionBlockedRemainingMs + 999) / 1000 : 0,
        _progressionBlockedAnchor.active ? (uint8)_progressionBlockedReason : 0,
        _progressionCandidateCursor);
    if (recordVisit)
        CharacterDatabase.DirectPExecute(
            "INSERT INTO bot_citizen_zone_visit (char_guid,map_id,zone_id,first_seen,last_seen,visits) "
            "VALUES (%u,%u,%u,UNIX_TIMESTAMP(),UNIX_TIMESTAMP(),1) "
            "ON DUPLICATE KEY UPDATE last_seen=VALUES(last_seen), visits=visits+1",
            me->GetGUIDLow(), me->GetMapId(), me->GetZoneId());
}

void PlayerBotAI::SetCitizenActivityIntent(uint8 intent, char const* reason)
{
    if (!me || !IsZoneCitizen() || intent > 4 || _citizenActivityIntent == intent)
        return;
    _citizenActivityIntent = intent;
    PersistCitizenJournal(intent == 1 ? 1 : 0);
    sLog.outString("[ZoneCitizen][Job] guid:%u job:%u reason:%s", me->GetGUIDLow(),
                   intent, reason ? reason : "state");
}

void PlayerBotAI::RefreshCitizenProgression(bool forceLog)
{
    if (!me || !IsZoneCitizen())
        return;

    uint32 const level = std::max<uint32>(1, me->GetLevel());
    uint8 const band = static_cast<uint8>((level - 1) / 10);
    uint32 const zone = me->GetZoneId();
    int32 const zoneLevel = sPlayerBotMgr.GetCitizenZoneDifficulty(me->GetMapId(), zone);
    // Use the measured creature-level distribution, not the zone's average,
    // so a mixed-level zone remains viable when it still has enough productive
    // prey. The combat filter accepts [level - 3, level] for solo citizens.
    uint32 const productiveMinimum = level > 3 ? level - 3 : 1;
    uint32 const productiveShare = sPlayerBotMgr.GetCitizenZoneProductiveSharePercent(
        me->GetMapId(), zone, productiveMinimum, level);
    bool const levelCapped = level >= 60;
    bool const overleveled = !levelCapped &&
        productiveShare < PlayerBotMgr::CitizenZoneMinimumProductiveSharePercent;
    bool const changed = band != _progressionBand || zone != _progressionZone ||
                         zoneLevel != _progressionZoneLevel ||
                         overleveled != _progressionOverleveled;

    _progressionBand = band;
    _progressionZone = zone;
    _progressionZoneLevel = zoneLevel;
    _progressionOverleveled = overleveled;
    if (forceLog || changed)
    {
        char const* status = levelCapped ? "level-cap" : (overleveled ? "outgrowing" : "in-band");
        sLog.outString("[ZoneCitizen][Progression] guid:%u level:%u band:%u-%u zone:%u zone-level:%d productive:%u%% status:%s",
                       me->GetGUIDLow(), level, band * 10 + 1,
                       std::min<uint32>(60, (band + 1) * 10), zone, zoneLevel,
                       productiveShare, status);
        _progressionReportMs = 60000;
    }
}

// Restore carried gear at login for persistent citizens and owned
// companions. Use the core repair path so equipped stats and item dirty
// state are updated correctly, while `cost=false` makes this a free
// reconnect service. Run after auto-equipping so the whole carried loadout
// starts at full durability. Preserve the fixture count correction that
// prevents invalid stack counts on owned-companion equipment.
void PlayerBotAI::RepairBotEquipmentAtLogin()
{
    if (!me || (!IsZoneCitizen() && !IsOwnedCompanion()))
        return;

    auto countWornCarriedItems = [this]() -> uint32
    {
        uint32 worn = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) <
                            item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                ++worn;
        }
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            for (uint8 slot = 0; slot < MAX_BAG_SIZE; ++slot)
            {
                Item* item = me->GetItemByPos(bag, slot);
                if (item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) <
                                item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                    ++worn;
            }
        return worn;
    };

    uint32 const wornBefore = countWornCarriedItems();
    me->DurabilityRepairAll(false, 0.0f);
    uint32 const wornAfter = countWornCarriedItems();
    uint32 const restored = wornBefore > wornAfter ? wornBefore - wornAfter : 0;
    sLog.outString("[BotLogin][Durability] GUID:%u restored:%u remaining:%u",
                   me->GetGUIDLow(), restored, wornAfter);

    if (!IsOwnedCompanion())
        return;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        ItemPrototype const* proto = item->GetProto();
        if (!proto)
            continue;
        if (proto->Stackable <= 1 && item->GetCount() != 1)
            item->SetCount(1);
    }
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

        // PORT-028 (KAP-558): category-based filter instead of the old
        // blanket req_skill_value skip. The 1.12 data gates most class
        // abilities on req_skill_value (the "raises this skill line"
        // marker, uniformly 1 in this DBC), so the old skip dropped
        // nearly every combat spell and left the companion auto-attack
        // only. Learn from Weapon Skills (6), Class Skills (7) and
        // Armor Proficiencies (8); skip Professions (11), Secondary
        // Skills (9) and Not Displayed (12). A gated entry first raises
        // the required skill line; UpdateSkill refuses a 0-value line,
        // so a missing line still skips the entry.
        if (ability->skillId)
        {
            SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(ability->skillId);
            int32 const category = skillLine ? skillLine->categoryId : 0;
            if (category != 6 && category != 7 && category != 8)
                continue;
            uint32 const have = me->GetSkillValue((uint16)ability->skillId);
            if (ability->req_skill_value > have)
            {
                if (have == 0 || !me->UpdateSkill(ability->skillId, ability->req_skill_value - have))
                    continue;
            }
        }
        else if (ability->req_skill_value != 0)
        {
            continue; // unknown skill line with a gate: keep the old skip
        }

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

    // Citizens take the standard Hunter taming skill as part of autonomous
    // class progression; pets themselves are still acquired with the core's
    // normal Tame Beast spell and tameable-creature checks.
    if (IsZoneCitizen() && playerClass == CLASS_HUNTER && level >= 10 &&
        !me->HasSpell(1515) && sSpellMgr.GetSpellEntry(1515))
        me->LearnSpell(1515, false);
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

// ---------------------------------------------------------------------------
// BL-002 (KAP-558): observe-only encounter recording. The recorder is a
// pure value module (Companion/Encounter.h); these adapters drive it from
// the shared combat executor, the owner-order handlers and the PlayerAI
// damage observation callbacks. None of this changes combat behavior:
// no decision, timer, target slot, log or fallback is altered by it.
// ---------------------------------------------------------------------------
// The shared cast vocabulary -> the encounter recorder's cast outcome.
static Companion::Encounter::CastOutcome ToEncounterOutcome(Companion::Combat::CastOutcome o)
{
    switch (o)
    {
        case Companion::Combat::CastOutcome::Accepted:
            return Companion::Encounter::CastOutcome::Accepted;
        case Companion::Combat::CastOutcome::Rejected:
            return Companion::Encounter::CastOutcome::Rejected;
        case Companion::Combat::CastOutcome::NoEligibleAbility:
            return Companion::Encounter::CastOutcome::NoEligibleAbility;
        case Companion::Combat::CastOutcome::None:
        default:
            return Companion::Encounter::CastOutcome::None;
    }
}

void PlayerBotAI::EncounterEngage(Companion::Combat::Source source,
                                  Companion::Combat::OffenseRoute route,
                                  uint64_t targetGuid, uint32 diff)
{
    if (_encounter.GetState() == Companion::Encounter::State::Active &&
        _encounter.GetTargetGuid() != targetGuid)
    {
        _encounter.End(Companion::Encounter::EndReason::TargetChanged);
        // BL-003: the prior completion is persisted before the new
        // target begins (exactly once, before reset).
        TryPersistLearningSummary();
    }
    _encounter.Begin(targetGuid, static_cast<uint32_t>(source),
                     static_cast<uint32_t>(route));
    _encounter.Tick(diff);
}

// BL-003 (KAP-558): persist a completed encounter summary to the
// learning store exactly once, before the recorder resets. Persistent
// owned companions and citizens share the per-character evidence path;
// the store still requires an explicitly started profile. It assigns
// the delivery identity
// (process nonce + monotonic sequence); a failed enqueue drops the
// summary by design (the FIFO is bounded and fail-closed).
void PlayerBotAI::TryPersistLearningSummary()
{
    if (_encounter.GetState() != Companion::Encounter::State::Complete)
        return;
    if (me && botEntry && botEntry->persistent &&
        (botEntry->ownerAccountId || IsZoneCitizen()))
    {
        Companion::Encounter::Recorder::Summary const r = _encounter.GetSummary();
        Companion::Learning::SummaryFifo::Summary s;
        s.charGuid = me->GetGUIDLow();
        s.targetGuid = r.targetGuid; // full 64-bit creature GUID
        s.sequence = ++_learningEncounterSeq; // per-session, 1-based
        s.policyVersion = 0; // BL-003: learned behavior stays disabled
        s.playbookVersion = 0;
        s.source = r.source;
        s.route = r.route;
        s.durationMs = r.durationMs;
        s.effectiveDamage = r.effectiveDamage;
        s.periodicDamage = r.periodicDamage;
        s.damageTaken = r.damageTaken;
        s.deaths = r.deaths;
        s.ownerOverrides = r.ownerOverrides;
        s.decisions = r.decisions;
        s.castsAccepted = r.castsAccepted;
        s.castsRejected = r.castsRejected;
        s.castsNoEligible = r.castsNoEligible;
        s.eventCount = r.eventCount;
        s.endReason = static_cast<uint32_t>(r.endReason);
        s.complete = r.complete;
        s.overflow = r.overflow;
        s.efficacyEligible = r.efficacyEligible;
        s.capturedAt = static_cast<uint32_t>(time(nullptr));
        if (!sPlayerBotMgr.LearningStore().Enqueue(s))
            sLog.outError("[PlayerBot][Learning] enqueue dropped GUID:%u reason:%s",
                          me->GetGUIDLow(), Companion::Encounter::EndReasonName(r.endReason));

        if (r.endReason == Companion::Encounter::EndReason::TargetDeath)
            ApplyValidatedCombatLesson(r, WorldTimer::getMSTime());

        // Offer one advisory, personality-colored debrief after a completed
        // party kill. This uses the conversation worker: no model or socket
        // call runs on the world thread, and no reply changes combat policy.
        // Persisted evidence above is independent of model availability.
        if (r.endReason == Companion::Encounter::EndReason::TargetDeath &&
            IsOwnedCompanion() && me->GetGroup() && me->IsAlive())
        {
            Group* group = me->GetGroup();
            Player* leader = sObjectAccessor.FindPlayer(group->GetLeaderGuid());
            uint32 const nowMs = WorldTimer::getMSTime();
            static uint32 lastPartyReviewMs = 0; // one voice per burst of shared kills
            if (leader && leader->GetSession() && !leader->GetSession()->GetBot() &&
                (!_combatReviewLastMs || uint32(nowMs - _combatReviewLastMs) >= 90000) &&
                (!lastPartyReviewMs || uint32(nowMs - lastPartyReviewMs) >= 15000))
            {
                std::string const evidence =
                    "duration_ms=" + std::to_string(r.durationMs) +
                    " damage=" + std::to_string(r.effectiveDamage) +
                    " taken=" + std::to_string(r.damageTaken) +
                    " casts_ok=" + std::to_string(r.castsAccepted) +
                    " casts_rejected=" + std::to_string(r.castsRejected) +
                    " decisions=" + std::to_string(r.decisions) +
                    " memory=" + (_partyMemoryLesson == 1 ? "caution" :
                                    _partyMemoryLesson == 2 ? "timing" :
                                    _partyMemoryLesson == 3 ? "steady" : "none");
                if (sPlayerBotMgr.ConversationTransport().Submit(
                        me->GetGUIDLow(), group->GetId(),
                        group->GetLeaderGuid().GetCounter(),
                        (uint32)botEntry->personalityProfile, evidence, nowMs,
                        Companion::Conversation::ConvKind::CombatReview))
                {
                    _combatReviewLastMs = nowMs;
                    lastPartyReviewMs = nowMs;
                }
            }
        }
    }
    _encounter.Reset(); // BL-003: persist before reset
}

void PlayerBotAI::ApplyValidatedCombatLesson(
    Companion::Encounter::Recorder::Summary const& summary, uint32 nowMs)
{
    if (!me || !botEntry || !IsOwnedCompanion() || !me->GetGroup())
        return;
    Player* leader = sObjectAccessor.FindPlayer(me->GetGroup()->GetLeaderGuid());
    if (!leader || !leader->GetSession() || leader->GetSession()->GetBot())
        return;

    uint32 lessonType = 3; // steady
    uint32 confidence = 1;
    if (summary.damageTaken >= 20 &&
        summary.damageTaken >= summary.effectiveDamage * 2)
    {
        lessonType = 1; // caution
        confidence = summary.decisions >= 3 ? 3 : 2;
        _combatCautionUntilMs = nowMs + 300000;
        _personalityChaseDist = 30.0f;
        _partyMemoryLesson = 1;
    }
    else if (summary.castsRejected > summary.castsAccepted && summary.decisions >= 3)
    {
        lessonType = 2; // timing
        confidence = 2;
        _partyMemoryLesson = 2;
    }
    else
        _partyMemoryLesson = 3;

    if (!sPlayerBotMgr.LearningStore().QueueLesson(
            me->GetGUIDLow(), lessonType, confidence, _learningEncounterSeq,
            summary.durationMs,
            summary.effectiveDamage, summary.damageTaken, summary.decisions))
        sLog.outError("[PlayerBot][Learning] lesson queue dropped GUID:%u type:%u",
                      me->GetGUIDLow(), lessonType);
    else if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Learning] lesson GUID:%u type:%u confidence:%u",
                       me->GetGUIDLow(), lessonType, confidence);
}

void PlayerBotAI::OnDamageDealt(Unit* target, uint32 effectiveDamage, uint32 spellId,
                                bool periodic, bool targetDied)
{
    if (!target)
        return;
    // Target-matching happens in the recorder: damage to any other victim
    // is ignored, and a matching target death completes the encounter.
    _encounter.RecordDamageDealt(target->GetObjectGuid().GetRawValue(), effectiveDamage,
                                 spellId, periodic, targetDied);
    // BL-003: a matching target death completes the encounter; persist
    // the summary before the recorder resets (no-op otherwise).
    TryPersistLearningSummary();
    if (targetDied)
        RecordSocialEncounter(target, true);
}

void PlayerBotAI::RecordSocialEncounter(Unit* target, bool targetDied)
{
    if (!targetDied || !target || !me || !me->IsInWorld() || !me->IsAlive() ||
        !botEntry || !botEntry->persistent || _socialCooldownMs ||
        target->GetTypeId() != TYPEID_UNIT)
        return;

    PlayerBotAI* partnerAI = nullptr;
    Player* partner = nullptr;
    for (auto const& item : sObjectAccessor.GetPlayers())
    {
        Player* candidate = item.second;
        if (!candidate || candidate == me || !candidate->IsInWorld() ||
            candidate->GetMapId() != me->GetMapId() ||
            candidate->GetDistance(me) > 25.0f)
            continue;
        PlayerBotAI* candidateAI = dynamic_cast<PlayerBotAI*>(candidate->AI());
        if (!candidateAI || !candidateAI->botEntry ||
            !candidateAI->botEntry->persistent || !candidate->IsAlive())
            continue;
        if (candidate->GetVictim() != target && !candidate->IsInCombat())
            continue;
        if (!partner || candidate->GetGUIDLow() < partner->GetGUIDLow())
        {
            partner = candidate;
            partnerAI = candidateAI;
        }
    }
    if (!partner || !partnerAI)
        return;

    CharacterDatabase.DirectPExecute(
        "INSERT INTO bot_social_acquaintance "
        "(char_guid, acquaintance_guid, shared_events, first_seen, last_seen) "
        "VALUES (%u, %u, 1, UNIX_TIMESTAMP(), UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE shared_events = shared_events + 1, "
        "last_seen = UNIX_TIMESTAMP()",
        me->GetGUIDLow(), partner->GetGUIDLow());
    CharacterDatabase.DirectPExecute(
        "INSERT INTO bot_social_acquaintance "
        "(char_guid, acquaintance_guid, shared_events, first_seen, last_seen) "
        "VALUES (%u, %u, 1, UNIX_TIMESTAMP(), UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE shared_events = shared_events + 1, "
        "last_seen = UNIX_TIMESTAMP()",
        partner->GetGUIDLow(), me->GetGUIDLow());
    _socialCooldownMs = 30000;
    partnerAI->_socialCooldownMs = 30000;

    // Only the lower GUID speaks, so one shared kill creates one visible,
    // world-local line rather than a two-bot echo.
    if (me->GetGUIDLow() < partner->GetGUIDLow())
    {
        std::string const line = "Good work, " + std::string(partner->GetName()) + ".";
        me->Say(line.c_str(), LANG_UNIVERSAL);
    }
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[Social] acquaintance GUID:%u partner:%u event:shared-kill",
                       me->GetGUIDLow(), partner->GetGUIDLow());
}

void PlayerBotAI::OnDamageTaken(Unit* /*attacker*/, uint32 effectiveDamage, uint32 spellId,
                                bool periodic, bool victimDied)
{
    // Direct damage taken by the companion while the encounter is active;
    // the companion's death completes it as a safety outcome.
    _encounter.RecordDamageTaken(effectiveDamage, spellId, periodic, victimDied);
    if (victimDied && me && IsZoneCitizen())
    {
        float const dx = me->GetPositionX() - _citizenDeathX;
        float const dy = me->GetPositionY() - _citizenDeathY;
        bool const sameSpot = _citizenDeathMap == me->GetMapId() &&
            _citizenDeathZone == me->GetZoneId() && dx * dx + dy * dy <= 40.0f * 40.0f;
        _citizenDeathsAtSpot = sameSpot ? std::min<uint8>(255, _citizenDeathsAtSpot + 1) : 1;
        _citizenDeathMap = me->GetMapId();
        _citizenDeathZone = me->GetZoneId();
        _citizenDeathX = me->GetPositionX();
        _citizenDeathY = me->GetPositionY();
        if (_citizenDeathsAtSpot >= 3)
        {
            // Escalate from observed repeated failures: three deaths gives a
            // 4.5-minute withdrawal, then it grows quadratically up to a
            // 15-minute cap. No zone, mob, or coordinate is special-cased.
            uint32 const cooldownSeconds = std::min<uint32>(900,
                30 * uint32(_citizenDeathsAtSpot) * uint32(_citizenDeathsAtSpot));
            _citizenDangerZoneUntilMs = WorldTimer::getMSTime() + cooldownSeconds * 1000;
            // Do not make a party member disappear.  Independent citizens
            // hand this request to the population allocator on its next tick.
            _citizenSafetyRelocationRequested = !me->GetGroup() ||
                sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup());
            if (_citizenSafetyRelocationRequested && botEntry)
            {
                // This entry survives the normal death/logout path, so the
                // allocator cannot lose the evidence between AI updates.
                botEntry->zoneWorldSafetyRelocation = true;
                botEntry->zoneWorldSafetyExcludeMap = _citizenDeathMap;
                botEntry->zoneWorldSafetyExcludeZone = _citizenDeathZone;
            }
            int32 const cellX = int32(std::floor(me->GetPositionX() / 40.0f));
            int32 const cellY = int32(std::floor(me->GetPositionY() / 40.0f));
            CharacterDatabase.DirectPExecute(
                "INSERT INTO bot_citizen_danger_memory "
                "(char_guid,map_id,zone_id,cell_x,cell_y,deaths,safe_until,updated_at) "
                "VALUES (%u,%u,%u,%d,%d,%u,UNIX_TIMESTAMP()+%u,UNIX_TIMESTAMP()) "
                "ON DUPLICATE KEY UPDATE deaths=LEAST(255,deaths+1), "
                "safe_until=GREATEST(safe_until,UNIX_TIMESTAMP()+LEAST(900,30*(deaths+1)*(deaths+1))), "
                "updated_at=UNIX_TIMESTAMP()",
                me->GetGUIDLow(), _citizenDeathMap, _citizenDeathZone, cellX, cellY,
                _citizenDeathsAtSpot, cooldownSeconds);
            sLog.outString("[ZoneCitizen][Survival] danger-zone guid:%u deaths:%u zone:%u",
                           me->GetGUIDLow(), _citizenDeathsAtSpot, _citizenDeathZone);
        }
    }
    // BL-003: companion death completes the encounter; persist before reset.
    TryPersistLearningSummary();
}
bool PlayerBotAI::TryOffensiveCastOrAttack(Unit* target)
{
    if (TryCitizenEmergencySelfHeal())
        return true;
    uint32 const spellId = SelectOffensiveSpell(target);
    // BL-002: the rotation decision boundary (0 = no eligible ability).
    _encounter.RecordDecision(spellId, false);
    if (!spellId)
    {
        _encounter.RecordCastResult(0, Companion::Encounter::CastOutcome::NoEligibleAbility);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Offense] no-eligible-ability GUID:%u t:%u fallback:ordinary-attack",
                           me->GetGUIDLow(), target->GetGUIDLow());
        me->Attack(target, true);
        return false;
    }
    SpellCastResult const castRes = me->CastSpell(target, spellId, false);
    Companion::Combat::CastReport const report =
        Companion::Combat::ReportCast(spellId, (uint32)castRes, MapCastReject((uint32)castRes));
    // BL-002: the cast result at the existing cast boundary.
    _encounter.RecordCastResult(spellId, ToEncounterOutcome(report.outcome));
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
    // BL-002: the taunt cast result (recorded only while an encounter is active).
    _encounter.RecordCastResult(spellId, ToEncounterOutcome(report.outcome));
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

    // BL-001A: baseline plan (identity permutation); a future non-baseline
    // plan will be supplied here after validation.
    Companion::Combat::RotationPlan const plan =
        Companion::Combat::RotationPlan::Baseline(static_cast<int>(profiles.size()));
    return Companion::Combat::SelectExecutable(profiles, query, plan).selected;
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

uint8 PlayerBotAI::GetCitizenSpecIndex()
{
    if (_citizenSpecInitialized)
        return static_cast<uint8>(_citizenSpecIndex);

    if (!me || !IsZoneCitizen() || me->GetLevel() < 10)
        return 0;
    _citizenSpecInitialized = true;

    // Rank each citizen within its class by stable character GUID. Cycling
    // that rank over the three DBC talent tabs keeps every class split even,
    // and new citizens append to the distribution without reshuffling peers.
    QueryResult* result = CharacterDatabase.PQuery(
        "SELECT COUNT(*) FROM playerbot pb JOIN characters c ON c.guid=pb.char_guid "
        "WHERE pb.ai='ZoneCitizenAI' AND c.class=%u AND pb.char_guid<=%u",
        me->GetClass(), me->GetGUIDLow());
    uint32 rank = 0;
    if (result)
    {
        rank = result->Fetch()[0].GetUInt32();
        delete result;
    }
    _citizenSpecIndex = rank ? (rank - 1) % 3 : me->GetGUIDLow() % 3;
    return static_cast<uint8>(_citizenSpecIndex);
}

char const* PlayerBotAI::GetCitizenSpecName(uint8 specIndex) const
{
    static char const* const trees[12][3] = {
        { "", "", "" },
        { "Arms", "Fury", "Protection" },
        { "Holy", "Protection", "Retribution" },
        { "Beast Mastery", "Marksmanship", "Survival" },
        { "Assassination", "Combat", "Subtlety" },
        { "Discipline", "Holy", "Shadow" },
        { "", "", "" },
        { "Elemental", "Enhancement", "Restoration" },
        { "Arcane", "Fire", "Frost" },
        { "Affliction", "Demonology", "Destruction" },
        { "", "", "" },
        { "Balance", "Feral", "Restoration" }
    };
    uint32 const classId = me ? me->GetClass() : 0;
    return classId < 12 && specIndex < 3 ? trees[classId][specIndex] : "Unknown";
}

void PlayerBotAI::AutoAssignCitizenTalents()
{
    if (!me || !IsZoneCitizen() || me->GetLevel() < 10)
        return;

    me->InitTalentForLevel();
    uint32 const freeAtStart = me->GetFreeTalentPoints();
    if (!freeAtStart)
        return;

    uint8 const specIndex = GetCitizenSpecIndex();
    uint32 const* talentTabs = GetTalentTabPages(me->GetClass());
    uint32 const talentTab = talentTabs ? talentTabs[specIndex] : 0;
    if (!talentTab)
    {
        sLog.outError("[ZoneCitizen][Spec] no talent tab for guid:%u class:%u tree:%u",
                      me->GetGUIDLow(), me->GetClass(), specIndex);
        return;
    }

    auto currentRank = [this](TalentEntry const& talent) -> uint32
    {
        for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            if (talent.RankID[rank] && me->HasSpell(talent.RankID[rank]))
                return static_cast<uint32>(rank + 1);
        return 0;
    };

    // Prefer the earliest row, continue ranks already started, and use the
    // column/DBC id as a stable tie break. LearnTalent enforces row,
    // prerequisite, class and rank rules exactly as it does for players.
    for (uint32 guard = 0; guard < freeAtStart; ++guard)
    {
        std::vector<TalentEntry const*> candidates;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(i);
            if (!talent || talent->TalentTab != talentTab)
                continue;
            uint32 const rank = currentRank(*talent);
            if (rank < MAX_TALENT_RANK && talent->RankID[rank])
                candidates.push_back(talent);
        }
        std::sort(candidates.begin(), candidates.end(), [&](TalentEntry const* left, TalentEntry const* right)
        {
            if (left->Row != right->Row) return left->Row < right->Row;
            bool const leftStarted = currentRank(*left) != 0;
            bool const rightStarted = currentRank(*right) != 0;
            if (leftStarted != rightStarted) return leftStarted;
            if (left->Col != right->Col) return left->Col < right->Col;
            return left->TalentID < right->TalentID;
        });

        uint32 const before = me->GetFreeTalentPoints();
        bool learned = false;
        for (TalentEntry const* talent : candidates)
        {
            uint32 const rank = currentRank(*talent);
            me->LearnTalent(talent->TalentID, rank);
            if (me->GetFreeTalentPoints() < before)
            {
                learned = true;
                break;
            }
        }
        if (!learned)
            break;
    }

    uint32 const spent = freeAtStart - me->GetFreeTalentPoints();
    if (spent || me->GetFreeTalentPoints())
        sLog.outString("[ZoneCitizen][Spec] guid:%u class:%u spec:%s points-spent:%u points-remaining:%u",
                       me->GetGUIDLow(), me->GetClass(), GetCitizenSpecName(specIndex),
                       spent, me->GetFreeTalentPoints());
}

bool PlayerBotAI::MaintainCitizenHunterPet(uint32 diff)
{
    if (!me || !IsZoneCitizen() || me->GetClass() != CLASS_HUNTER || me->GetLevel() < 10)
        return false;

    if (Pet* pet = me->GetPet())
    {
        if (!_hunterPetReported)
            sLog.outString("[ZoneCitizen][Pet] hunter-ready guid:%u pet-entry:%u pet-level:%u",
                           me->GetGUIDLow(), pet->GetEntry(), pet->GetLevel());
        _hunterPetReported = true;
        return false;
    }
    if (_hunterPetReported)
    {
        sLog.outString("[ZoneCitizen][Pet] hunter-pet-missing guid:%u level:%u",
                       me->GetGUIDLow(), me->GetLevel());
        _hunterPetReported = false;
    }

    if (me->IsNonMeleeSpellCasted(false))
        return true;
    if (me->IsInCombat() || _hunterPetAttemptTimer > diff)
    {
        if (_hunterPetAttemptTimer > diff)
            _hunterPetAttemptTimer -= diff;
        return false;
    }
    _hunterPetAttemptTimer = 30000;

    uint32 const tameBeast = 1515;
    if (!me->HasSpell(tameBeast))
    {
        sLog.outString("[ZoneCitizen][Pet] waiting-for-tame-spell guid:%u level:%u",
                       me->GetGUIDLow(), me->GetLevel());
        return false;
    }

    Creature* target = nullptr;
    NearestTameableCreatureCheck check(me, 30.0f);
    MaNGOS::CreatureLastSearcher<NearestTameableCreatureCheck> searcher(target, check);
    Cell::VisitGridObjects(me, searcher, 30.0f);
    if (!target)
        return false;

    SpellCastResult const result = me->CastSpell(target, tameBeast, false);
    if (result == SPELL_CAST_OK)
    {
        sLog.outString("[ZoneCitizen][Pet] taming guid:%u creature-entry:%u creature-level:%u",
                       me->GetGUIDLow(), target->GetEntry(), target->GetLevel());
        return true;
    }
    sLog.outString("[ZoneCitizen][Pet] tame-rejected guid:%u creature-entry:%u result:%u",
                   me->GetGUIDLow(), target->GetEntry(), uint32(result));
    return false;
}

void PlayerBotAI::AutoEquipForLevel()
{
    if (!me)
        return;

    // PORT-024 (KAP-558): owned companions progress equipment only
    // from items they legitimately receive through the loot policy
    // (Companion/Equipment.h + EvaluateReceivedEquipment). The free
    // level-based refresh stays authoritative for ambient world
    // bots. One gate here covers login, level-up, the UpdateAI
    // level-change fallback and party leave/rejoin (a rejoin is a
    // relogin).
    if (IsOwnedCompanion())
        return;

    uint8 level = me->GetLevel();
    bool citizenInitialGearRoll = false;
    bool citizenGearRollDue = true;
    if (IsZoneCitizen())
    {
        QueryResult* roll = CharacterDatabase.PQuery(
            "SELECT gear_roll_level,gear_roll_policy_version "
            "FROM bot_citizen_journal WHERE char_guid=%u",
            me->GetGUIDLow());
        uint8 lastRollLevel = 0;
        uint8 lastRollPolicyVersion = 0;
        if (roll)
        {
            Field* fields = roll->Fetch();
            lastRollLevel = fields[0].GetUInt8();
            lastRollPolicyVersion = fields[1].GetUInt8();
            delete roll;
        }
        citizenInitialGearRoll = !lastRollLevel;
        citizenGearRollDue = citizenInitialGearRoll || level > lastRollLevel ||
            lastRollPolicyVersion < Companion::CitizenGearPolicy::kPolicyVersion;
        if (!citizenGearRollDue)
            return;
    }
    // Citizens receive items usable at their current level. Their stable
    // per-slot quality roll keeps the equipment varied without giving them
    // items above their level; earned better gear is never removed.
    uint8 const citizenGearLevel = level;
    uint8 const citizenSpec = IsZoneCitizen() && level >= 10 ? GetCitizenSpecIndex() : 0;
    bool const protectionWarrior = IsZoneCitizen() && me->GetClass() == CLASS_WARRIOR && citizenSpec == 2;
    bool const shieldAndOneHandSpec = IsZoneCitizen() && level >= 10 &&
        (protectionWarrior ||
         (me->GetClass() == CLASS_PALADIN && citizenSpec <= 1) ||
         (me->GetClass() == CLASS_SHAMAN && (citizenSpec == 0 || citizenSpec == 2)) ||
         (me->GetClass() == CLASS_SHAMAN && citizenSpec == 1 && level < 20));
    bool const dualWieldAvailable = level >= 10 && me->HasSpell(674); // Dual Wield
    bool const dualWieldWeapons = IsZoneCitizen() && dualWieldAvailable &&
        ((me->GetClass() == CLASS_ROGUE) ||
         ((me->GetClass() == CLASS_WARRIOR || me->GetClass() == CLASS_HUNTER) && level >= 20));
    bool const prefersTwoHand = IsZoneCitizen() && level >= 10 &&
        ((me->GetClass() == CLASS_WARRIOR && citizenSpec == 0) ||
         (me->GetClass() == CLASS_PALADIN && citizenSpec == 2) ||
         (me->GetClass() == CLASS_SHAMAN && citizenSpec == 1 && level >= 20));
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

    // One stable quality roll per citizen/level/equipment slot. Re-logging at
    // the same level therefore never rerolls the gear; leveling creates the
    // next deterministic opportunity to replace an item.
    auto desiredCitizenQuality = [&](uint16 slot) -> uint32
    {
        uint32 const roll = (me->GetGUIDLow() * 2654435761u + uint32(level) * 2246822519u +
                             uint32(slot & 0xff) * 3266489917u) %
                            Companion::CitizenGearPolicy::kRollRange;
        switch (Companion::CitizenGearPolicy::QualityForRoll(roll))
        {
            case Companion::CitizenGearPolicy::QualityTier::Blue:
                return ITEM_QUALITY_RARE;
            case Companion::CitizenGearPolicy::QualityTier::Green:
                return ITEM_QUALITY_UNCOMMON;
            default:
                return ITEM_QUALITY_NORMAL;
        }
    };

    std::map<std::pair<uint16, uint32>, GearChoice> bestBySlotQuality;

    for (auto const& pair : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& proto = pair.second;

        if (proto.Class != ITEM_CLASS_ARMOR && proto.Class != ITEM_CLASS_WEAPON)
            continue;

        if (!IsAllowedSource(proto))
            continue;

        if (!IsZoneCitizen() && proto.Quality > ITEM_QUALITY_NORMAL)
            continue;
        if (IsZoneCitizen() && proto.Quality > ITEM_QUALITY_RARE)
            continue;

        if (proto.RequiredLevel > citizenGearLevel)
            continue;

        // avoid items too far below current level if required level known
        uint32 effectiveLevel = proto.RequiredLevel;
        if (!effectiveLevel)
        {
            // crude estimate: item level maps roughly to 2/3 of required level
            effectiveLevel = proto.ItemLevel ? std::max<uint32>(1, (proto.ItemLevel * 2) / 3) : level;
        }
        bool const levelEligible = IsZoneCitizen()
            ? Companion::CitizenGearPolicy::IsEligibleEffectiveLevel(citizenGearLevel, effectiveLevel)
            : (effectiveLevel <= citizenGearLevel &&
               std::abs(int(citizenGearLevel) - int(effectiveLevel)) <= int(_gearMaxDiff));
        if (!levelEligible)
            continue;

        if (proto.RequiredSkill || proto.RequiredSpell || proto.RequiredHonorRank || proto.RequiredCityRank || proto.RequiredReputationRank)
            continue;

        if (proto.AllowableClass && !(proto.AllowableClass & classMask))
            continue;
        if (proto.AllowableRace && !(proto.AllowableRace & raceMask))
            continue;

        if (proto.Class == ITEM_CLASS_ARMOR && !CanEquipArmorSubclass(proto))
            continue;
        bool const casterHoldable = proto.InventoryType == INVTYPE_HOLDABLE && IsZoneCitizen() &&
            (me->GetClass() == CLASS_PALADIN || me->GetClass() == CLASS_PRIEST ||
             me->GetClass() == CLASS_SHAMAN || me->GetClass() == CLASS_MAGE ||
             me->GetClass() == CLASS_WARLOCK || me->GetClass() == CLASS_DRUID);
        if (proto.Class == ITEM_CLASS_WEAPON && !casterHoldable && !CanEquipWeaponSubclass(proto))
            continue;
        if (shieldAndOneHandSpec && proto.Class == ITEM_CLASS_WEAPON &&
            proto.InventoryType != INVTYPE_WEAPON && proto.InventoryType != INVTYPE_WEAPONMAINHAND)
            continue;
        if (prefersTwoHand && proto.Class == ITEM_CLASS_WEAPON &&
            proto.InventoryType != INVTYPE_2HWEAPON && proto.InventoryType != INVTYPE_WEAPON)
            continue;

        std::vector<uint8> preferredSlots(1, NULL_SLOT);
        if (shieldAndOneHandSpec && proto.Class == ITEM_CLASS_WEAPON)
            preferredSlots[0] = EQUIPMENT_SLOT_MAINHAND;
        else if (dualWieldWeapons && proto.Class == ITEM_CLASS_WEAPON && proto.InventoryType == INVTYPE_WEAPON)
            preferredSlots = { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND };
        else if (prefersTwoHand && proto.Class == ITEM_CLASS_WEAPON)
            preferredSlots[0] = EQUIPMENT_SLOT_MAINHAND;
        else if (IsZoneCitizen() && proto.InventoryType == INVTYPE_HOLDABLE &&
                 (me->GetClass() == CLASS_PALADIN || me->GetClass() == CLASS_PRIEST ||
                  me->GetClass() == CLASS_SHAMAN || me->GetClass() == CLASS_MAGE ||
                  me->GetClass() == CLASS_WARLOCK || me->GetClass() == CLASS_DRUID))
            preferredSlots[0] = EQUIPMENT_SLOT_OFFHAND;

        int32 statScore = 0;
        for (auto const& stat : proto.ItemStat)
        {
            int32 weight = 0;
            switch (stat.ItemStatType)
            {
                case ITEM_MOD_STRENGTH:
                    weight = (me->GetClass() == CLASS_WARRIOR || me->GetClass() == CLASS_PALADIN ||
                              (me->GetClass() == CLASS_SHAMAN && citizenSpec == 1) ||
                              (me->GetClass() == CLASS_DRUID && citizenSpec == 1)) ? 3 : 1;
                    break;
                case ITEM_MOD_AGILITY:
                    weight = (me->GetClass() == CLASS_HUNTER || me->GetClass() == CLASS_ROGUE ||
                              me->GetClass() == CLASS_DRUID ||
                              (me->GetClass() == CLASS_SHAMAN && citizenSpec == 1)) ? 3 : 1;
                    break;
                case ITEM_MOD_STAMINA:
                    weight = (protectionWarrior ||
                              (me->GetClass() == CLASS_PALADIN && citizenSpec == 1) ||
                              (me->GetClass() == CLASS_WARLOCK && citizenSpec == 1)) ? 4 : 2;
                    break;
                case ITEM_MOD_INTELLECT:
                    weight = (me->GetClass() == CLASS_PRIEST || me->GetClass() == CLASS_MAGE ||
                              me->GetClass() == CLASS_WARLOCK || me->GetClass() == CLASS_PALADIN ||
                              me->GetClass() == CLASS_SHAMAN || me->GetClass() == CLASS_DRUID) ? 3 : 1;
                    break;
                case ITEM_MOD_SPIRIT:
                    weight = (me->GetClass() == CLASS_PRIEST || me->GetClass() == CLASS_MAGE ||
                              me->GetClass() == CLASS_WARLOCK || me->GetClass() == CLASS_DRUID ||
                              me->GetClass() == CLASS_PALADIN || me->GetClass() == CLASS_SHAMAN) ? 2 : 1;
                    if (me->GetClass() == CLASS_PRIEST && citizenSpec == 2)
                        weight = 1;
                    break;
                default: break;
            }
            statScore += stat.ItemStatValue * weight;
        }
        uint32 score = effectiveLevel * 1000 + proto.ItemLevel + std::max<int32>(0, statScore) * 20;
        if (shieldAndOneHandSpec && proto.Class == ITEM_CLASS_ARMOR && proto.SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
            score += 5000;
        if (prefersTwoHand && proto.Class == ITEM_CLASS_WEAPON && proto.InventoryType == INVTYPE_2HWEAPON)
            score += 5000;
        if (me->GetClass() == CLASS_HUNTER && proto.Class == ITEM_CLASS_WEAPON && proto.SubClass == ITEM_SUBCLASS_WEAPON_BOW)
            score += 5000; // Hunters prefer their ranged bow for ordinary hunting.

        for (uint8 preferredSlot : preferredSlots)
        {
            uint16 dest = NULL_SLOT;
            InventoryResult res = me->CanEquipItem(preferredSlot, dest, &proto, nullptr, true);
            if (res != EQUIP_ERR_OK || dest == NULL_SLOT)
                continue;
            auto key = std::make_pair(dest, proto.Quality);
            auto it = bestBySlotQuality.find(key);
            if (it == bestBySlotQuality.end() || it->second.score < score)
                bestBySlotQuality[key] = { &proto, score };
        }
    }

    std::map<uint16, GearChoice> selectedBySlot;
    uint32 targetBlueSlots = 0;
    uint32 targetGreenSlots = 0;
    uint32 targetNormalSlots = 0;
    uint32 selectedBlueSlots = 0;
    uint32 selectedGreenSlots = 0;
    uint32 selectedNormalSlots = 0;
    uint32 selectedProtoBlueSlots = 0;
    uint32 selectedProtoGreenSlots = 0;
    uint32 selectedProtoNormalSlots = 0;
    for (auto const& candidate : bestBySlotQuality)
    {
        uint16 const dest = candidate.first.first;
        if (selectedBySlot.count(dest))
            continue;

        // Enumerate each slot once and search by target tier. Iterating the
        // (slot, quality) map directly selected its lowest key first.
        uint32 const target = IsZoneCitizen() ? desiredCitizenQuality(dest) : ITEM_QUALITY_NORMAL;
        if (target == ITEM_QUALITY_RARE)
            ++targetBlueSlots;
        else if (target == ITEM_QUALITY_UNCOMMON)
            ++targetGreenSlots;
        else
            ++targetNormalSlots;
        for (uint32 rank = 0; rank < 4; ++rank)
        {
            uint32 const quality = IsZoneCitizen()
                ? Companion::CitizenGearPolicy::FallbackQualityAt(target, rank)
                : (rank == 0 ? ITEM_QUALITY_NORMAL :
                   rank == 1 ? ITEM_QUALITY_UNCOMMON :
                   rank == 2 ? ITEM_QUALITY_RARE : ITEM_QUALITY_POOR);
            auto found = bestBySlotQuality.find(std::make_pair(dest, quality));
            if (found != bestBySlotQuality.end())
            {
                selectedBySlot[dest] = found->second;
                if (quality == ITEM_QUALITY_RARE)
                    ++selectedBlueSlots;
                else if (quality == ITEM_QUALITY_UNCOMMON)
                    ++selectedGreenSlots;
                else
                    ++selectedNormalSlots;
                if (found->second.proto)
                {
                    if (found->second.proto->Quality == ITEM_QUALITY_RARE)
                        ++selectedProtoBlueSlots;
                    else if (found->second.proto->Quality == ITEM_QUALITY_UNCOMMON)
                        ++selectedProtoGreenSlots;
                    else
                        ++selectedProtoNormalSlots;
                }
                break;
            }
        }
    }

    // AC-10 R4: count equip failures so the gear-roll marker is never
    // written ahead of durable equipment changes.
    uint32 equipFailures = 0;
    uint32 equipChanges = 0;
    auto hasFreeCitizenInventorySlot = [&]() -> bool
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                return true;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* contents = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
                for (uint8 slot = 0; slot < contents->GetBagSize(); ++slot)
                    if (!me->GetItemByPos(bag, slot))
                        return true;
        return false;
    };
    auto discardOneCitizenInventoryItem = [&]() -> bool
    {
        if (!IsZoneCitizen())
            return false;

        auto discardAt = [&](uint8 bag, uint8 slot) -> bool
        {
            Item* item = me->GetItemByPos(bag, slot);
            if (!item || item->GetEntry() == 6948) // Hearthstone is always retained.
                return false;

            uint32 const entry = item->GetEntry();
            me->DestroyItem(bag, slot, true);
            if (me->GetItemByPos(bag, slot))
                return false;

            sLog.outString("[ZoneCitizen][Gear] inventory item discarded guid:%u item:%u bag:%u slot:%u",
                           me->GetGUIDLow(), entry, uint32(bag), uint32(slot));
            return true;
        };

        // Clear carried contents only. Equipped bags and bank contents are
        // preserved; any carried item except the Hearthstone may be discarded
        // when a replacement needs room for the item it is replacing.
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (discardAt(INVENTORY_SLOT_BAG_0, slot))
                return true;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* contents = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
                for (uint8 slot = 0; slot < contents->GetBagSize(); ++slot)
                    if (discardAt(bag, slot))
                        return true;
        return false;
    };
    for (auto const& it : selectedBySlot)
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
                // AC-10 R4: the slot already holds exactly the selected
                // item; a re-run of the deterministic roll (for example
                // after a marker-write failure) must not churn it.
                if (existingProto->ItemId == proto->ItemId)
                    continue;
                bool const requiresShield = shieldAndOneHandSpec && slot == EQUIPMENT_SLOT_OFFHAND &&
                                            existingProto->InventoryType != INVTYPE_SHIELD;
                bool const requiresOneHand = shieldAndOneHandSpec && slot == EQUIPMENT_SLOT_MAINHAND &&
                    (existingProto->InventoryType == INVTYPE_2HWEAPON ||
                     existingProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                     existingProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                     existingProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                     existingProto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF ||
                     existingProto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM);
                bool const existingValuableUpgrade = existingProto->Quality >= ITEM_QUALITY_UNCOMMON &&
                    existingProto->ItemLevel >= proto->ItemLevel;
                bool const sameOrBetterQualityAtLevel = existingProto->ItemLevel >= proto->ItemLevel &&
                    existingProto->Quality >= proto->Quality;
                bool const materiallyHigherLevel = existingProto->ItemLevel > proto->ItemLevel + 2;
                bool const preserveInitialUpgrade = existingValuableUpgrade ||
                    existingProto->ItemLevel > proto->ItemLevel + 10;
                if (!requiresShield && !requiresOneHand &&
                    (citizenInitialGearRoll ? preserveInitialUpgrade :
                     (sameOrBetterQualityAtLevel || materiallyHigherLevel)))
                    continue;
            }
            // First try the normal 1-for-1 replacement. If every carried
            // slot is occupied, discard carried items until the old gear can
            // be stored. The Hearthstone is excluded from cleanup.
            me->AutoUnequipItemFromSlot(slot, false);
            while (me->GetItemByPos(bag, slot) && IsZoneCitizen() &&
                   !hasFreeCitizenInventorySlot() && discardOneCitizenInventoryItem())
                me->AutoUnequipItemFromSlot(slot, false);

            // If no disposable item remains (for example, only a Hearthstone
            // is carried), preserve the replaced gear in mail rather than
            // blocking the selected upgrade.
            if (me->GetItemByPos(bag, slot) && IsZoneCitizen())
                me->AutoUnequipItemFromSlot(slot, true);
            if (me->GetItemByPos(bag, slot))
            {
                ++equipFailures;
                if (IsZoneCitizen())
                    sLog.outError("[ZoneCitizen][Gear] replacement blocked guid:%u level:%u slot:%u reason:occupied",
                                  me->GetGUIDLow(), level, uint32(slot));
                continue;
            }
        }

        Item* equipped = me->EquipNewItem(dest, proto->ItemId, true);
        Item* actual = me->GetItemByPos(bag, slot);
        if (!equipped || !actual || actual->GetEntry() != proto->ItemId)
        {
            ++equipFailures;
            if (IsZoneCitizen())
                sLog.outError("[ZoneCitizen][Gear] equip verification failed guid:%u level:%u slot:%u item:%u",
                              me->GetGUIDLow(), level, uint32(slot), proto->ItemId);
        }
        else
            ++equipChanges;
    }

    uint32 equippedBlueSlots = 0;
    uint32 equippedGreenSlots = 0;
    uint32 equippedNormalSlots = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* equipped = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (ItemPrototype const* proto = equipped->GetProto())
            {
                if (proto->Quality == ITEM_QUALITY_RARE)
                    ++equippedBlueSlots;
                else if (proto->Quality == ITEM_QUALITY_UNCOMMON)
                    ++equippedGreenSlots;
                else
                    ++equippedNormalSlots;
            }

    if (IsZoneCitizen() && citizenGearRollDue)
    {
        // AC-10 R4: the gear-roll marker must not outlive the equipment
        // changes it records. Persist the inventory first (synchronous
        // transaction), then write the marker. If any equip or the
        // marker write fails, the marker stays behind the equipment and
        // the deterministic roll harmlessly re-runs at the next login
        // (slots already holding the selected item are skipped).
        if (equipFailures)
        {
            sLog.outError("[ZoneCitizen][Gear] marker skipped guid:%u level:%u equip-failures:%u",
                          me->GetGUIDLow(), level, equipFailures);
            return;
        }
        me->SaveInventoryAndGoldToDB();
        bool const markerOk = CharacterDatabase.DirectPExecute(
            "INSERT INTO bot_citizen_journal (char_guid, gear_roll_level, gear_roll_policy_version) "
            "VALUES (%u,%u,%u) ON DUPLICATE KEY UPDATE "
            "gear_roll_level=GREATEST(gear_roll_level,VALUES(gear_roll_level)), "
            "gear_roll_policy_version=GREATEST(gear_roll_policy_version,VALUES(gear_roll_policy_version))",
            me->GetGUIDLow(), level,
            uint32(Companion::CitizenGearPolicy::kPolicyVersion));
        if (!markerOk)
            sLog.outError("[ZoneCitizen][Gear] marker write failed guid:%u level:%u (re-roll next login)",
                          me->GetGUIDLow(), level);
        sLog.outString("[ZoneCitizen][Gear] roll-complete guid:%u level:%u initial:%u slots:%u "
                       "target-blue:%u target-green:%u target-normal:%u "
                       "selected-blue:%u selected-green:%u selected-normal:%u "
                       "proto-blue:%u proto-green:%u proto-normal:%u "
                       "equipped-blue:%u equipped-green:%u equipped-normal:%u changed:%u",
                       me->GetGUIDLow(), level, citizenInitialGearRoll ? 1 : 0,
                       uint32(selectedBySlot.size()), targetBlueSlots, targetGreenSlots,
                       targetNormalSlots, selectedBlueSlots, selectedGreenSlots,
                       selectedNormalSlots, selectedProtoBlueSlots, selectedProtoGreenSlots,
                       selectedProtoNormalSlots, equippedBlueSlots, equippedGreenSlots,
                       equippedNormalSlots, equipChanges);
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
    // PORT-019 (KAP-558): personality runtime effects die with the
    // session; the declared profile is re-applied from its source.
    _personalityChaseDist = kOwnerFollowChaseDist;
    _personalityLastExprMs = 0;
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

bool PlayerBotAI::TryCitizenEmergencySelfHeal()
{
    if (!me || !IsZoneCitizen() ||
        (me->GetGroup() && !sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup())) ||
        !me->IsInCombat() ||
        me->GetHealth() * 100 >= me->GetMaxHealth() * 55 ||
        me->IsNonMeleeSpellCasted(true))
        return false;

    uint32 baseSpell = 0;
    switch (me->GetClass())
    {
        case CLASS_PALADIN: baseSpell = 635; break;   // Holy Light
        case CLASS_PRIEST:  baseSpell = 2050; break;  // Lesser Heal
        case CLASS_SHAMAN:  baseSpell = 331; break;   // Healing Wave
        case CLASS_DRUID:   baseSpell = 5185; break;  // Healing Touch
        default: return false;
    }
    uint32 const spellId = GetHighestKnownSpell(baseSpell);
    if (!spellId || !me->HasSpell(spellId) || me->HasSpellCooldown(spellId))
        return false;
    SpellEntry const* const info = sSpellMgr.GetSpellEntry(spellId);
    if (!info || me->GetPower(static_cast<Powers>(info->powerType)) < info->manaCost)
        return false;
    SpellCastResult const result = me->CastSpell(me, spellId, false);
    if (result != SPELL_CAST_OK)
        return false;
    _abilityTimer = 1500;
    sLog.outString("[ZoneCitizen][Survival] self-heal guid:%u spell:%u hp:%u/%u",
                   me->GetGUIDLow(), spellId, me->GetHealth(), me->GetMaxHealth());
    return true;
}

bool PlayerBotAI::UpdateCitizenRetreat(uint32 diff)
{
    if (!_citizenRetreat.Active() || !me)
        return false;
    using Companion::CitizenRecovery::Outcome;
    Outcome const outcome = _citizenRetreat.Update(diff, me->GetPositionX(),
                                                  me->GetPositionY(), me->IsInCombat());
    if (outcome == Outcome::Walking)
        return true; // rest, loot and target acquisition must not cancel this leg

    float const dx = me->GetPositionX() - _citizenRetreatStartX;
    float const dy = me->GetPositionY() - _citizenRetreatStartY;
    char const* reason = outcome == Outcome::Escaped ? "escaped" :
                         outcome == Outcome::Stalled ? "stalled" : "timed-out";
    sLog.outString("[ZoneCitizen][Survival] retreat-result guid:%u threat:%u result:%s displacement:%.1f combat:%u",
                   me->GetGUIDLow(), _citizenRetreatThreat, reason,
                   std::sqrt(dx * dx + dy * dy), me->IsInCombat() ? 1 : 0);
    if (!MotionIdle())
        ClearCitizenRetreatMotion();
    if (outcome == Outcome::Escaped)
    {
        _citizenRetreatRetry.Reset();
        // Rest only after observed displacement and natural combat clearance.
        _worldRestUntilMs = WorldTimer::getMSTime() + 15000;
        _activityPauseMs = 15000;
        SetCitizenActivityIntent(4, "retreat-escaped");
        return true;
    }
    RecordCitizenRetreatFailure(true, reason);
    return false; // a failed escape must leave normal self-defense available
}

void PlayerBotAI::ClearCitizenRetreatMotion()
{
    if (!me)
        return;
    MotionMaster* const motion = me->GetMotionMaster();
    // Clearing every generator leaves MotionMaster empty; its next world
    // update asserts. Restore a valid idle default before control returns.
    motion->Clear(false, true);
    motion->MoveIdle();
    me->StopMoving();
}

void PlayerBotAI::RecordCitizenRetreatFailure(bool hasEndpoint, char const* reason)
{
    _citizenRetreatRetry.Fail(hasEndpoint, _citizenRetreatEndX, _citizenRetreatEndY);
    bool const deferred = _citizenRetreatRetry.Deferred();
    SetCitizenActivityIntent(4, deferred ? "retreat-deferred-self-defense" : "retreat-retry");
    sLog.outString("[ZoneCitizen][Survival] retreat-retry guid:%u reason:%s failures:%u retry-ms:%u deferred:%u",
                   me->GetGUIDLow(), reason, _citizenRetreatRetry.Failures(),
                   _citizenRetreatRetry.RetryMs(), deferred ? 1 : 0);
}

bool PlayerBotAI::TryCitizenDisengage(Unit* threat)
{
    if (!me || !IsZoneCitizen() ||
        (me->GetGroup() && !sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup())) ||
        !me->GetMap() ||
        !me->GetMaxHealth() || !threat || !threat->IsInWorld() ||
        threat->GetMap() != me->GetMap() || !_citizenRetreatRetry.CanTry() ||
        !Companion::CitizenRecovery::ShouldRetreat(
            me->GetHealth(), me->GetMaxHealth(),
            _citizenDangerZoneUntilMs > WorldTimer::getMSTime(),
            threat->GetHealth(), threat->GetMaxHealth()))
        return false;
    if (_citizenRetreat.Active())
        return true;

    // Failure memory belongs to this map even if no candidate can be issued.
    _citizenRetreatMap = me->GetMapId();
    _failedHuntGuid = threat->GetTypeId() == TYPEID_UNIT ? threat->GetGUIDLow() : 0;
    _failedHuntMs = 60000;

    float const sx = me->GetPositionX(), sy = me->GetPositionY(), sz = me->GetPositionZ();
    float const tx = threat->GetPositionX(), ty = threat->GetPositionY();
    float const startThreatDistance = std::hypot(sx - tx, sy - ty);
    float originX = tx, originY = ty, originZ = sz;
    float leashDistance = sWorld.getConfig(CONFIG_FLOAT_THREAT_RADIUS);
    if (threat->GetTypeId() == TYPEID_UNIT)
    {
        Creature const* creature = static_cast<Creature const*>(threat);
        creature->GetCombatStartPosition(originX, originY, originZ);
        leashDistance = std::max(leashDistance, creature->GetAttackDistance(me) * 1.5f);
    }
    // Core evasion requires both participants outside the combat-start radius
    // and an expired leash-extension timer. A fixed 30-yard endpoint was still
    // inside the default 50-yard radius and could never establish separation.
    float const retreatDistance = std::max(35.0f, std::min(90.0f, leashDistance + 15.0f));
    float const originDistance = std::hypot(sx - originX, sy - originY);
    float const away = originDistance > 0.1f ? std::atan2(sy - originY, sx - originX)
                                           : me->GetOrientation() + 3.14159265f;
    // Prefer away and sideways alternatives. A missing or nearby activity home
    // cannot prevent an attempt, and home never overrides the observed threat.
    float const offsets[] = {0.0f, 0.78539816f, -0.78539816f, 1.57079633f,
                             -1.57079633f, 2.35619449f, -2.35619449f, 3.14159265f};
    BotCitizenEscapeScan scan(me, retreatDistance + 30.0f);
    Creature* unused = nullptr;
    MaNGOS::CreatureLastSearcher<BotCitizenEscapeScan> searcher(unused, scan);
    Cell::VisitGridObjects(me, searcher, retreatDistance + 30.0f);
    float bestX = sx, bestY = sy, bestZ = sz, bestScore = -1.0f;
    uint32 blocked = 0, unsafe = 0, remembered = 0;
    if (!scan.overflow)
    {
        for (float offset : offsets)
        {
            float x = sx + retreatDistance * std::cos(away + offset);
            float y = sy + retreatDistance * std::sin(away + offset);
            float z = sz;
            me->UpdateGroundPositionZ(x, y, z);
            if (!me->GetMap()->GetWalkHitPosition(nullptr, sx, sy, sz, x, y, z) ||
                !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                std::hypot(x - sx, y - sy) < 6.0f)
            {
                ++blocked;
                continue;
            }
            if (_citizenRetreatRetry.Excludes(x, y))
            {
                ++remembered;
                continue;
            }
            float const distance = std::hypot(x - tx, y - ty);
            bool safe = distance >= startThreatDistance + 5.0f;
            for (Creature const* hostile : scan.hostiles)
            {
                if (hostile == threat)
                    continue;
                float const oldDistance = std::hypot(sx - hostile->GetPositionX(), sy - hostile->GetPositionY());
                float const newDistance = std::hypot(x - hostile->GetPositionX(), y - hostile->GetPositionY());
                // Existing attackers must become farther away. Other hostiles
                // must remain outside aggro range at both ends and along the leg.
                if (hostile->GetVictim() == me)
                {
                    if (newDistance < oldDistance + 3.0f)
                        safe = false;
                }
                else
                {
                    float const vx = x - sx, vy = y - sy;
                    float const length2 = vx * vx + vy * vy;
                    float const projection = std::max(0.0f, std::min(1.0f,
                        ((hostile->GetPositionX() - sx) * vx + (hostile->GetPositionY() - sy) * vy) / length2));
                    float const clearance = std::hypot(sx + projection * vx - hostile->GetPositionX(),
                                                       sy + projection * vy - hostile->GetPositionY());
                    if (clearance < hostile->GetAttackDistance(me) + 3.0f)
                        safe = false;
                }
            }
            if (!safe)
            {
                ++unsafe;
                continue;
            }
            float const score = distance - startThreatDistance;
            if (score > bestScore)
            {
                bestScore = score;
                bestX = x; bestY = y; bestZ = z;
            }
        }
    }
    if (bestScore < 0.0f)
    {
        RecordCitizenRetreatFailure(false, scan.overflow ? "crowded" : "no-safe-candidate");
        sLog.outString("[ZoneCitizen][Survival] retreat-unavailable guid:%u threat:%u blocked:%u unsafe:%u crowded:%u home:%u remembered:%u",
                       me->GetGUIDLow(), threat->GetGUIDLow(), blocked, unsafe,
                       scan.overflow ? 1 : 0, _activityHomeSet ? 1 : 0, remembered);
        return false; // do not clear combat, stop motion, or pretend to rest
    }

    _citizenRetreatThreat = threat->GetGUIDLow();
    _citizenRetreatMap = me->GetMapId();
    _citizenRetreatStartX = sx; _citizenRetreatStartY = sy;
    _citizenRetreatEndX = bestX; _citizenRetreatEndY = bestY;
    me->AttackStop();
    ClearTarget();
    if (!MotionIdle())
        me->GetMotionMaster()->Clear(false);
    _travelActive = false;
    _travelHuntGuid = 0;
    _travelNudging = false;
    _worldRestUntilMs = 0;
    _activityPauseMs = 0;
    _citizenRetreat.Begin(sx, sy);
    me->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING);
    SetCitizenActivityIntent(4, "combat-retreat");
    sLog.outString("[ZoneCitizen][Survival] retreat-start guid:%u threat:%u hp:%u/%u blocked:%u unsafe:%u",
                   me->GetGUIDLow(), _citizenRetreatThreat, me->GetHealth(), me->GetMaxHealth(), blocked, unsafe);
    return true;
}

void PlayerBotAI::StartActivityTravel(float x, float y, float z,
                                      uint64_t huntGuid, uint8 targetNode,
                                      bool progressionLeg)
{
    _travelX = x;
    _travelY = y;
    _travelZ = z;
    _travelTimeMs = progressionLeg ? 90000 : 45000;
    _travelActive = true;
    _travelHuntGuid = huntGuid;
    _travelFromNode = 255;
    for (uint8 i = 0; i < _activityNodeCount; ++i)
        if (me->GetDistance(_activityNodes[i].x, _activityNodes[i].y,
                            _activityNodes[i].z) < 15.0f)
        {
            _travelFromNode = i;
            break;
        }
    _travelTargetNode = targetNode;
    _travelProgressMs = 5000;
    _travelLastDistance = me->GetDistance(x, y, z);
    _travelRecoveryStage = 0;
    _travelNudging = false;
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
}

bool PlayerBotAI::BeginProgressionTravel()
{
    bool const autonomousLeader = me && me->GetGroup() &&
        sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup()) &&
        me->GetGroup()->GetLeaderGuid() == me->GetObjectGuid();
    if (!me || !IsZoneCitizen() ||
        (me->GetGroup() && !autonomousLeader) || me->IsInCombat() ||
        !_progressionOverleveled || _progressionRetryMs)
        return false;
    uint32 targetZone = 0;
    std::vector<Companion::CitizenTravel::Waypoint> routeWaypoints;
    bool routePartial = false;
    uint32 nextCandidateCursor = _progressionCandidateCursor;
    Companion::CitizenTravel::BlockedAnchor const* blockedAnchor =
        _progressionBlockedRemainingMs && _progressionBlockedAnchor.active
            ? &_progressionBlockedAnchor : nullptr;
    if (!sPlayerBotMgr.SelectCitizenProgressionDestination(
            me, targetZone, _progressionFinalX, _progressionFinalY, _progressionFinalZ,
            &routeWaypoints, blockedAnchor, &routePartial,
            _progressionCandidateCursor, &nextCandidateCursor))
    {
        // Do not repeat a full zone scan every AI tick when the current
        // location has no verified, level-appropriate nearby destination.
        _progressionRetryMs = 60000;
        _progressionCandidateCursor = nextCandidateCursor;
        PersistCitizenJournal(0);
        sLog.outString("[ZoneCitizen][Progression] no verified suitable destination guid:%u map:%u zone:%u level:%u",
                       me->GetGUIDLow(), me->GetMapId(), me->GetZoneId(), me->GetLevel());
        return false;
    }
    _progressionTargetZone = targetZone;
    _progressionCandidateCursor = 0;
    _progressionMap = me->GetMapId();
    _progressionTravelActive = true;
    _progressionWaypoints.swap(routeWaypoints);
    _progressionWaypointIndex = 0;
    _progressionRoutePartial = routePartial;
    _progressionPartialReplans = 0;
    _progressionPartialEndpoints.clear();
    if (routePartial && !_progressionWaypoints.empty())
        _progressionPartialEndpoints.push_back(_progressionWaypoints.back());
    PersistCitizenJournal(1);
    AreaEntry const* target = AreaEntry::GetById(targetZone);
    sLog.outString("[ZoneCitizen][Progression] route start guid:%u from:%u to:%u (%s)",
                   me->GetGUIDLow(), me->GetZoneId(), targetZone,
                   target && target->Name ? target->Name : "unknown");
    if (_activitySpeechUntilMs <= WorldTimer::getMSTime() && target && target->Name)
    {
        bool const inHuntingGroup = me->GetGroup() &&
            sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup());
        std::string const line = inHuntingGroup
            ? "Our hunting party is taking the road toward " + std::string(target->Name) + "."
            : "I am taking the road toward " + std::string(target->Name) + ".";
        me->Say(line.c_str(), LANG_UNIVERSAL);
        _activitySpeechUntilMs = WorldTimer::getMSTime() + 90000;
    }
    if (me->GetGroup() && sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup()))
        sLog.outString("[ZoneCitizen][HuntingGroup] traveling group:%u leader:%u from:%u to:%u",
                       me->GetGroup()->GetId(), me->GetGUIDLow(), me->GetZoneId(), targetZone);
    return AdvanceProgressionTravel();
}

void PlayerBotAI::FinishProgressionTravel(bool arrived, char const* reason)
{
    if (arrived)
    {
        _progressionBlockedAnchor = Companion::CitizenTravel::BlockedAnchor();
        _progressionBlockedRemainingMs = 0;
        _progressionBlockedReason = Companion::CitizenTravel::RouteFailureReason::None;
    }
    else if (_progressionTargetZone)
    {
        _progressionBlockedAnchor.active = true;
        _progressionBlockedAnchor.mapId = _progressionMap;
        _progressionBlockedAnchor.zoneId = _progressionTargetZone;
        _progressionBlockedAnchor.x = _progressionFinalX;
        _progressionBlockedAnchor.y = _progressionFinalY;
        _progressionBlockedAnchor.z = _progressionFinalZ;
        _progressionBlockedRemainingMs = Companion::CitizenTravel::kBlockedAnchorCooldownMs;
        _progressionBlockedReason = reason && std::strstr(reason, "stalled")
            ? Companion::CitizenTravel::RouteFailureReason::Stalled
            : (reason && std::strstr(reason, "anchor")
                ? Companion::CitizenTravel::RouteFailureReason::AnchorBlocked
                : Companion::CitizenTravel::RouteFailureReason::NoPath);
        sLog.outString("[ZoneCitizen][Progression] anchor cooldown guid:%u zone:%u reason:%u seconds:%u",
                       me ? me->GetGUIDLow() : 0, _progressionTargetZone,
                       (uint32)_progressionBlockedReason,
                       _progressionBlockedRemainingMs / 1000);
    }
    _progressionTravelActive = false;
    _progressionTargetZone = 0;
    _progressionFinalX = _progressionFinalY = _progressionFinalZ = 0.0f;
    _progressionWaypoints.clear();
    _progressionWaypointIndex = 0;
    _progressionRoutePartial = false;
    _progressionPartialReplans = 0;
    _progressionPartialEndpoints.clear();
    if (arrived)
    {
        _activityHomeSet = false;
        ++_citizenVisitedZones;
        PersistCitizenJournal(0, true);
        return;
    }
    _progressionRetryMs = 120000;
    SetCitizenActivityIntent(4, reason);
    PersistCitizenJournal(0);
}

bool PlayerBotAI::AdvanceProgressionTravel()
{
    if (!me || !_progressionTravelActive || !me->GetMap())
        return false;
    float const dx = _progressionFinalX - me->GetPositionX();
    float const dy = _progressionFinalY - me->GetPositionY();
    float const distance = std::sqrt(dx * dx + dy * dy);

    // Explicit arrival (TW-BOTS-002 S1): only the selected hunting anchor
    // on verified usable ground ends the route as success. Entering the
    // destination zone alone never counts as arrival.
    using Companion::CitizenTravel::ArrivalDecision;
    Companion::CitizenTravel::ArrivalInput input;
    input.sameMap = me->GetMapId() == _progressionMap;
    input.inTargetZone = me->GetZoneId() == _progressionTargetZone;
    input.distanceToAnchorYards = distance;
    if (input.sameMap && distance < Companion::CitizenTravel::kArrivalRadiusYards)
    {
        float ax = _progressionFinalX, ay = _progressionFinalY, az = _progressionFinalZ;
        me->UpdateGroundPositionZ(ax, ay, az);
        input.anchorWalkable = std::isfinite(ax) && std::isfinite(ay) && std::isfinite(az) &&
            me->GetMap()->GetWalkHitPosition(nullptr, me->GetPositionX(),
                                             me->GetPositionY(), me->GetPositionZ(),
                                             ax, ay, az) &&
            std::hypot(ax - _progressionFinalX, ay - _progressionFinalY) <= 2.0f &&
            me->GetMap()->GetTerrain()->GetZoneId(ax, ay, az) == _progressionTargetZone;
    }
    ArrivalDecision const decision = Companion::CitizenTravel::DecideArrival(input);
    if (decision == ArrivalDecision::ArrivedAnchor)
    {
        sLog.outString("[ZoneCitizen][Progression] route arrived guid:%u zone:%u anchor:%.1f/%.1f",
                       me->GetGUIDLow(), me->GetZoneId(),
                       _progressionFinalX, _progressionFinalY);
        FinishProgressionTravel(true, "progression-arrived");
        return true;
    }
    if (decision == ArrivalDecision::MapLost)
    {
        sLog.outError("[ZoneCitizen][Progression] route map-lost guid:%u map:%u expected:%u to-zone:%u",
                      me->GetGUIDLow(), me->GetMapId(), _progressionMap,
                      _progressionTargetZone);
        FinishProgressionTravel(false, "progression-map-lost");
        return false;
    }
    if (decision == ArrivalDecision::BlockedNearAnchor)
    {
        sLog.outError("[ZoneCitizen][Progression] route blocked guid:%u to-zone:%u anchor-unusable",
                      me->GetGUIDLow(), _progressionTargetZone);
        FinishProgressionTravel(false, "progression-anchor-blocked");
        return false;
    }

    if (_progressionWaypointIndex >= _progressionWaypoints.size())
    {
        if (_progressionRoutePartial &&
            ++_progressionPartialReplans > Companion::CitizenTravel::kMaximumPartialRouteReplans)
        {
            sLog.outError("[ZoneCitizen][Progression] route partial-limit guid:%u to-zone:%u",
                          me->GetGUIDLow(), _progressionTargetZone);
            FinishProgressionTravel(false, "progression-route-stalled");
            return false;
        }
        std::vector<Companion::CitizenTravel::Waypoint> replannedWaypoints;
        bool partial = false;
        float estimatedYards = 0.0f;
        if (!sPlayerBotMgr.BuildCitizenProgressionRoute(me, _progressionTargetZone,
                _progressionFinalX, _progressionFinalY, _progressionFinalZ,
                replannedWaypoints, partial, estimatedYards) || replannedWaypoints.empty())
        {
            sLog.outError("[ZoneCitizen][Progression] route no-path guid:%u to-zone:%u",
                          me->GetGUIDLow(), _progressionTargetZone);
            FinishProgressionTravel(false, "progression-no-route");
            return false;
        }
        if (partial)
        {
            Companion::CitizenTravel::Waypoint const& endpoint = replannedWaypoints.back();
            for (Companion::CitizenTravel::Waypoint const& prior : _progressionPartialEndpoints)
                if (std::hypot(endpoint.x - prior.x, endpoint.y - prior.y) <
                    Companion::CitizenTravel::kRepeatedPartialEndpointToleranceYards)
                {
                    sLog.outError("[ZoneCitizen][Progression] route repeated-partial-endpoint guid:%u to-zone:%u",
                                  me->GetGUIDLow(), _progressionTargetZone);
                    FinishProgressionTravel(false, "progression-route-stalled");
                    return false;
                }
            _progressionPartialEndpoints.push_back(endpoint);
            if (_progressionPartialEndpoints.size() > 8)
                _progressionPartialEndpoints.erase(_progressionPartialEndpoints.begin());
        }
        else
        {
            _progressionPartialReplans = 0;
            _progressionPartialEndpoints.clear();
        }
        _progressionWaypoints.swap(replannedWaypoints);
        _progressionWaypointIndex = 0;
        _progressionRoutePartial = partial;
    }

    while (_progressionWaypointIndex < _progressionWaypoints.size())
    {
        Companion::CitizenTravel::Waypoint const& waypoint =
            _progressionWaypoints[_progressionWaypointIndex++];
        if (!std::isfinite(waypoint.x) || !std::isfinite(waypoint.y) || !std::isfinite(waypoint.z))
            continue;
        float const legDistance = std::sqrt(
            std::pow(waypoint.x - me->GetPositionX(), 2.0f) +
            std::pow(waypoint.y - me->GetPositionY(), 2.0f) +
            std::pow(waypoint.z - me->GetPositionZ(), 2.0f));
        if (legDistance < 8.0f)
            continue;
        StartActivityTravel(waypoint.x, waypoint.y, waypoint.z, 0, 255, true);
        sLog.outString("[ZoneCitizen][Progression] route waypoint guid:%u zone:%u to-zone:%u dist:%.1f partial:%u remaining:%u",
                       me->GetGUIDLow(), me->GetZoneId(), _progressionTargetZone,
                       legDistance, _progressionRoutePartial ? 1 : 0,
                       (uint32)(_progressionWaypoints.size() - _progressionWaypointIndex));
        return true;
    }

    sLog.outError("[ZoneCitizen][Progression] route no-progress guid:%u to-zone:%u",
                  me->GetGUIDLow(), _progressionTargetZone);
    FinishProgressionTravel(false, "progression-route-stalled");
    return false;
}

void PlayerBotAI::RememberActivityArrival()
{
    uint8 reached = 255;
    for (uint8 i = 0; i < _activityNodeCount; ++i)
    {
        float const dx = _activityNodes[i].x - me->GetPositionX();
        float const dy = _activityNodes[i].y - me->GetPositionY();
        if (dx * dx + dy * dy < 12.0f * 12.0f)
        {
            reached = i;
            break;
        }
    }
    if (reached == 255 && _activityNodeCount < 12)
    {
        reached = _activityNodeCount++;
        _activityNodes[reached].x = me->GetPositionX();
        _activityNodes[reached].y = me->GetPositionY();
        _activityNodes[reached].z = me->GetPositionZ();
    }
    if (reached != 255)
    {
        bool const newLink = _travelFromNode < _activityNodeCount &&
            reached != _travelFromNode &&
            !(_activityNodes[_travelFromNode].links & (1u << reached));
        if (_travelFromNode < _activityNodeCount && reached != _travelFromNode)
        {
            _activityNodes[_travelFromNode].links |= uint16(1u << reached);
            _activityNodes[reached].links |= uint16(1u << _travelFromNode);
        }
        _activityNodes[reached].blockedMs = 0;
        if (_activityNodes[reached].visits < 255)
            ++_activityNodes[reached].visits;
        _activityCurrentNode = reached;
        if (newLink)
        {
            sPlayerBotMgr.RecordCitizenRouteEdge(me, _activityNodes[_travelFromNode].x,
                                                 _activityNodes[_travelFromNode].y,
                                                 _activityNodes[reached].x,
                                                 _activityNodes[reached].y);
            sLog.outString("[WorldRoute] learned guid:%u from:%u to:%u nodes:%u",
                           me->GetGUIDLow(), _travelFromNode, reached,
                           _activityNodeCount);
        }
    }
    _travelActive = false;
    _travelHuntGuid = 0;
    _travelNudging = false;
}

void PlayerBotAI::AbandonActivityTravel()
{
    bool const abandonedProgression = _progressionTravelActive;
    uint32 const failedTarget = _travelHuntGuid
        ? ObjectGuid(_travelHuntGuid).GetCounter() : 0;
    if (_travelHuntGuid)
    {
        _failedHuntGuid = failedTarget;
        _failedHuntMs = 60000;
    }
    if (_travelTargetNode < _activityNodeCount)
        _activityNodes[_travelTargetNode].blockedMs = 60000;
    sLog.outString("[WorldRoute] abandoned guid:%u target:%u stage:%u",
                   me->GetGUIDLow(), failedTarget, _travelRecoveryStage);
    if (!MotionIdle())
        me->GetMotionMaster()->Clear(false);
    _travelActive = false;
    _travelHuntGuid = 0;
    _travelNudging = false;
    if (abandonedProgression)
        FinishProgressionTravel(false, "progression-route-stalled");
    else
        _progressionTravelActive = false;
    _activityPauseMs = 3000;
}

bool PlayerBotAI::UpdateIndependentActivity(uint32 diff)
{
    if (!me || !me->IsAlive() || !me->GetMap())
        return false;
    uint32 const nowMs = WorldTimer::getMSTime();
    if (_progressionRetryMs)
        _progressionRetryMs = _progressionRetryMs > diff ? _progressionRetryMs - diff : 0;
    if (_progressionBlockedRemainingMs)
    {
        _progressionBlockedRemainingMs = _progressionBlockedRemainingMs > diff
            ? _progressionBlockedRemainingMs - diff : 0;
        if (!_progressionBlockedRemainingMs)
        {
            _progressionBlockedAnchor.active = false;
            _progressionBlockedReason = Companion::CitizenTravel::RouteFailureReason::None;
            PersistCitizenJournal(_progressionTravelActive ? 1 : 0);
        }
    }
    if (_progressionReportMs > diff)
        _progressionReportMs -= diff;
    else
    {
        _progressionReportMs = 60000;
        RefreshCitizenProgression(false);
    }
    // An established fight remains with the existing combat/loot executor.
    // A party invite preempts this method before it can choose a new goal.
    if (me->IsInCombat() || me->GetVictim() || GetAliveHeldTarget())
    {
        _travelActive = false;
        _travelHuntGuid = 0;
        _travelNudging = false;
        return false;
    }
    if (_targets.corpse)
        return true; // finish the normal loot attempt before walking away

    if (IsZoneCitizen() && _citizenRetreatRetry.Deferred())
    {
        // Combat already returned above, so self-defense remains available.
        // While out of combat, defer new hunts/travel until the bounded retry.
        SetCitizenActivityIntent(4, "retreat-deferred-self-defense");
        if (!MotionIdle())
            ClearCitizenRetreatMotion();
        _travelActive = false;
        _travelHuntGuid = 0;
        _travelNudging = false;
        return true;
    }

    if (_worldRestUntilMs > WorldTimer::getMSTime())
    {
        SetCitizenActivityIntent(4, "model-or-health-rest");
        if (!MotionIdle())
            me->GetMotionMaster()->Clear(false);
        _travelActive = false;
        _travelHuntGuid = 0;
        _travelNudging = false;
        return true;
    }

    if (_citizenDangerZoneUntilMs > WorldTimer::getMSTime() &&
        _citizenDeathMap == me->GetMapId() && _citizenDeathZone == me->GetZoneId())
    {
        // Three deaths at essentially one location is evidence that this
        // ground is unsafe for this solo citizen.  The normal death path
        // already uses this safe manager logout; use it here as well rather
        // than spending the entire warning resting in the same kill pocket.
        if (botEntry && botEntry->zoneWorldSafetyRelocation &&
            (!me->GetGroup() || sPlayerBotMgr.IsAutonomousCitizenGroup(me->GetGroup())))
        {
            sLog.outString("[ZoneCitizen][Survival] leaving-danger guid:%u map:%u zone:%u",
                           me->GetGUIDLow(), _citizenDeathMap, _citizenDeathZone);
            sPlayerBotMgr.DeleteBot(me->GetGUIDLow());
            return true;
        }
        // A pending handoff has not reached the roster entry yet. Do not
        // reopen combat while it does; retain the older short retreat only
        // for that brief handoff window.
        SetCitizenActivityIntent(4, "danger-zone-retreat");
        if (MotionIdle() && _activityHomeSet && _activityMap == me->GetMapId() &&
            _activityZone == me->GetZoneId())
        {
            float x = me->GetPositionX(), y = me->GetPositionY(), z = me->GetPositionZ();
            float const dx = _activityHomeX - x, dy = _activityHomeY - y;
            float const distance = std::sqrt(dx * dx + dy * dy);
            if (distance > 4.0f)
            {
                float const leg = std::min(35.0f, distance);
                x += dx * leg / distance;
                y += dy * leg / distance;
                me->UpdateGroundPositionZ(x, y, z);
                if (me->GetMap()->GetWalkHitPosition(nullptr, me->GetPositionX(),
                                                     me->GetPositionY(), me->GetPositionZ(),
                                                     x, y, z))
                    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
            }
        }
        return true;
    }

    if (!_activityHomeSet || _activityMap != me->GetMapId() ||
        (!_progressionTravelActive && _activityZone != me->GetZoneId()))
    {
        _activityHomeSet = true;
        _activityMap = me->GetMapId();
        _activityZone = me->GetZoneId();
        _activityHomeX = me->GetPositionX();
        _activityHomeY = me->GetPositionY();
        _activityHomeZ = me->GetPositionZ();
        _travelActive = false;
        _travelHuntGuid = 0;
        _travelNudging = false;
        _activityPauseMs = 0;
        _destinationScanMs = 0;
        _noHuntTargetMs = 0;
        _noHuntTargetTier = 0;
        _activityNodeCount = 1;
        _activityCurrentNode = 0;
        _activityNodes[0] = ActivityNode();
        _activityNodes[0].x = _activityHomeX;
        _activityNodes[0].y = _activityHomeY;
        _activityNodes[0].z = _activityHomeZ;
    }

    // A zone can have a broad level profile while this citizen's local pocket
    // has no suitable prey. After sustained healthy misses, widen ordinary
    // walkable patrol within the same zone so its existing combat scan can
    // find real, currently available targets. Never treat walking itself as
    // hunting or progression.
    float const patrolRadius = _noHuntTargetMs >= kCitizenNoTargetSecondExpansionMs
        ? kCitizenPatrolRadiusWide
        : (_noHuntTargetMs >= kCitizenNoTargetFirstExpansionMs
            ? kCitizenPatrolRadiusExpanded : kCitizenPatrolRadiusNormal);

    _destinationScanMs = _destinationScanMs > diff ? _destinationScanMs - diff : 0;
    if (_failedHuntMs)
    {
        _failedHuntMs = _failedHuntMs > diff ? _failedHuntMs - diff : 0;
        if (!_failedHuntMs)
            _failedHuntGuid = 0;
    }
    for (uint8 i = 0; i < _activityNodeCount; ++i)
        _activityNodes[i].blockedMs = _activityNodes[i].blockedMs > diff
            ? _activityNodes[i].blockedMs - diff : 0;

    // A progression route is made of short path-found legs. Combat, loot,
    // party commands, death and rest preempt it above; otherwise it has the
    // first claim on off-duty movement and cannot be replaced by a nearby
    // low-level hunt.
    if (_progressionTravelActive && !_travelActive)
    {
        SetCitizenActivityIntent(1, "progression-route");
        AdvanceProgressionTravel();
        return true;
    }
    if (_progressionTravelActive)
        SetCitizenActivityIntent(1, "progression-route");
    if (!_progressionTravelActive && IsZoneCitizen() && _progressionOverleveled &&
        BeginProgressionTravel())
        return true;

    // Pace the scan independently of the world tick even with a larger
    // citizen roster. Low health makes the bot rest instead of pulling.
    if (_huntScanMs <= diff && !_progressionTravelActive)
    {
        _huntScanMs = 2000;
        if (me->GetHealth() * 100 >= me->GetMaxHealth() *
            Companion::CitizenRecovery::kHuntReadyHealthPercent)
        {
            BotCitizenHuntScan scan(me, _failedHuntMs ? _failedHuntGuid : 0);
            Creature* found = nullptr;
            MaNGOS::CreatureLastSearcher<BotCitizenHuntScan> searcher(found, scan);
            Cell::VisitGridObjects(me, searcher, 25.0f);
            if (Creature* target = scan.Best())
            {
                if (IsZoneCitizen() && _noHuntTargetTier)
                    sLog.outString("[ZoneCitizen][Hunt] prey-found guid:%u after-no-target-ms:%u",
                                   me->GetGUIDLow(), _noHuntTargetMs);
                _noHuntTargetMs = 0;
                _noHuntTargetTier = 0;
                _travelActive = false;
                _travelHuntGuid = 0;
                _travelNudging = false;
                RememberCombatTarget(target);
                me->Attack(target, true);
                sPlayerBotMgr.FormCitizenCombatGroup(me, target);
                UpdateCombatPursuit(target, diff);
                SetCitizenActivityIntent(2, "nearby-target");
                sLog.outString("[ZoneCitizen] hunt guid:%u target:%u level:%u",
                               me->GetGUIDLow(), target->GetGUIDLow(), target->GetLevel());
                return true;
            }
            if (IsZoneCitizen())
            {
                _noHuntTargetMs = std::min<uint32>(kCitizenNoTargetRecenterMs,
                                                   _noHuntTargetMs + _huntScanMs);
                uint8 const tier = _noHuntTargetMs >= kCitizenNoTargetSecondExpansionMs ? 2
                    : (_noHuntTargetMs >= kCitizenNoTargetFirstExpansionMs ? 1 : 0);
                if (tier > _noHuntTargetTier)
                {
                    _noHuntTargetTier = tier;
                    sLog.outString("[ZoneCitizen][Hunt] no-target-expand guid:%u zone:%u level:%u minutes:%u radius:%u",
                                   me->GetGUIDLow(), me->GetZoneId(), me->GetLevel(),
                                   tier == 1 ? 5 : 15,
                                   (uint32)(tier == 1 ? kCitizenPatrolRadiusExpanded : kCitizenPatrolRadiusWide));
                }
            }
        }
    }
    else
        _huntScanMs -= diff;

    if (me->GetHealth() * 100 < me->GetMaxHealth() * 75)
    {
        SetCitizenActivityIntent(4, "low-health");
        if (!MotionIdle())
            me->GetMotionMaster()->Clear(false);
        _travelActive = false;
        _travelHuntGuid = 0;
        _travelNudging = false;
        return true;
    }

    if (_travelActive)
    {
        if (_travelHuntGuid)
        {
            Creature* destination = me->GetMap()->GetCreature(ObjectGuid(_travelHuntGuid));
            if (!BotCitizenHuntScan::Eligible(me, destination))
            {
                if (!MotionIdle())
                    me->GetMotionMaster()->Clear(false);
                _travelActive = false;
                _travelHuntGuid = 0;
                _travelNudging = false;
                _activityPauseMs = 3000;
                return true;
            }
        }
        _travelTimeMs = _travelTimeMs > diff ? _travelTimeMs - diff : 0;
        if (me->GetDistance(_travelX, _travelY, _travelZ) < 4.0f)
        {
            if (_progressionTravelActive)
            {
                _travelActive = false;
                _travelHuntGuid = 0;
                _travelNudging = false;
                AdvanceProgressionTravel();
                return true;
            }
            RememberActivityArrival();
            _activityPauseMs = urand(4000, 9000);
            sLog.outString("[ZoneCitizen] travel reached guid:%u zone:%u",
                           me->GetGUIDLow(), _activityZone);
            if (IsZoneCitizen() && nowMs >= _activitySpeechUntilMs)
            {
                me->Say("I am checking another part of the area.", LANG_UNIVERSAL);
                _activitySpeechUntilMs = nowMs + urand(90000, 180000);
            }
        }
        else
        {
            if (_travelNudging &&
                me->GetDistance(_travelNudgeX, _travelNudgeY, _travelNudgeZ) < 3.0f)
            {
                _travelNudging = false;
                _travelProgressMs = 5000;
                _travelLastDistance = me->GetDistance(_travelX, _travelY, _travelZ);
                me->GetMotionMaster()->MovePoint(0, _travelX, _travelY, _travelZ,
                                                 MOVE_PATHFINDING);
                return true;
            }
            _travelProgressMs = _travelProgressMs > diff ? _travelProgressMs - diff : 0;
            if (!_travelTimeMs)
            {
                AbandonActivityTravel();
                return true;
            }
            if (!_travelProgressMs || (MotionIdle() && _travelRecoveryStage == 0))
            {
                float const remaining = me->GetDistance(_travelX, _travelY, _travelZ);
                if (!_travelNudging && _travelLastDistance - remaining >= 2.0f)
                    _travelRecoveryStage = 0;
                else if (_travelRecoveryStage == 0)
                {
                    _travelRecoveryStage = 1;
                    me->GetMotionMaster()->MovePoint(0, _travelX, _travelY, _travelZ,
                                                     MOVE_PATHFINDING);
                    sLog.outString("[WorldRoute] repath guid:%u", me->GetGUIDLow());
                }
                else if (_travelRecoveryStage == 1)
                {
                    float x = me->GetPositionX();
                    float y = me->GetPositionY();
                    float z = me->GetPositionZ();
                    if (me->GetMap()->GetWalkRandomPosition(nullptr, x, y, z, 8.0f) &&
                        me->GetDistance(x, y, z) >= 2.0f &&
                        (x - _activityHomeX) * (x - _activityHomeX) +
                        (y - _activityHomeY) * (y - _activityHomeY) <= 240.0f * 240.0f &&
                        me->GetMap()->GetTerrain()->GetZoneId(x, y, z) == _activityZone)
                    {
                        _travelRecoveryStage = 2;
                        _travelNudging = true;
                        _travelNudgeX = x;
                        _travelNudgeY = y;
                        _travelNudgeZ = z;
                        me->GetMotionMaster()->MovePoint(1, x, y, z, MOVE_PATHFINDING);
                        sLog.outString("[WorldRoute] nudge guid:%u", me->GetGUIDLow());
                    }
                    else
                        _travelRecoveryStage = 2;
                }
                else
                {
                    AbandonActivityTravel();
                    return true;
                }
                _travelLastDistance = remaining;
                _travelProgressMs = 5000;
            }
            return true;
        }
    }
    if (_activityPauseMs > diff)
    {
        _activityPauseMs -= diff;
        return true;
    }
    _activityPauseMs = 0;

    // Keep exploration bounded while allowing it to continue through large
    // zones. Once the wide patrol area has been searched for another 15
    // minutes, move the local center to the citizen's current walked-to spot.
    if (IsZoneCitizen() && _noHuntTargetMs >= kCitizenNoTargetRecenterMs && MotionIdle())
    {
        _activityHomeX = me->GetPositionX();
        _activityHomeY = me->GetPositionY();
        _activityHomeZ = me->GetPositionZ();
        _noHuntTargetMs = 0;
        _noHuntTargetTier = 0;
        sLog.outString("[ZoneCitizen][Hunt] patrol-center-advanced guid:%u zone:%u",
                       me->GetGUIDLow(), me->GetZoneId());
    }

    Map* map = me->GetMap();
    if (!_destinationScanMs && me->GetHealth() * 100 >= me->GetMaxHealth() * 50)
    {
        _destinationScanMs = 10000;
        BotCitizenHuntingGroundScan scan(me, _activityHomeX, _activityHomeY,
                                          _activityZone, _failedHuntGuid, patrolRadius);
        Creature* found = nullptr;
        MaNGOS::CreatureLastSearcher<BotCitizenHuntingGroundScan> searcher(found, scan);
        Cell::VisitGridObjects(me, searcher, 110.0f);
        if (Creature* ground = scan.Best())
        {
            StartActivityTravel(ground->GetPositionX(), ground->GetPositionY(),
                                ground->GetPositionZ(), ground->GetObjectGuid().GetRawValue());
            SetCitizenActivityIntent(2, "hunting-ground");
            sLog.outString("[ZoneCitizen] travel guid:%u zone:%u purpose:hunt target:%u to:%.1f/%.1f",
                           me->GetGUIDLow(), _activityZone, ground->GetGUIDLow(),
                           _travelX, _travelY);
            return true;
        }
    }
    if (IsZoneCitizen() && _noHuntTargetMs >= kCitizenNoTargetFirstExpansionMs &&
        me->GetHealth() * 100 >= me->GetMaxHealth() * 80)
    {
        // Random patrol widening can keep a citizen walking through empty
        // pockets. After sustained misses, route it to a static spawn that
        // can produce ordinary prey in its safe level band. This is a normal
        // pathfinding trip; nearby combat scans and retreat checks still run
        // while it travels and at the destination.
        float const anchorSearchRadius = _noHuntTargetMs >= kCitizenNoTargetSecondExpansionMs
            ? 2000.0f : patrolRadius;
        WorldLocation anchor;
        if (sPlayerBotMgr.SelectCitizenHuntAnchor(me->GetMapId(), _activityZone,
                me->GetLevel(), me->GetPositionX(), me->GetPositionY(),
                anchorSearchRadius, anchor))
        {
            StartActivityTravel(anchor.x, anchor.y, anchor.z, 0);
            SetCitizenActivityIntent(2, "hunting-spawn-anchor");
            sLog.outString("[ZoneCitizen][Hunt] spawn-anchor guid:%u zone:%u level:%u radius:%u to:%.1f/%.1f",
                           me->GetGUIDLow(), _activityZone, me->GetLevel(),
                           (uint32)anchorSearchRadius, anchor.x, anchor.y);
            return true;
        }
    }
    if (_activityNodeCount > 1 && urand(0, 2) == 0)
    {
        uint8 nearest = 255;
        for (uint8 i = 0; i < _activityNodeCount; ++i)
            if (me->GetDistance(_activityNodes[i].x, _activityNodes[i].y,
                                _activityNodes[i].z) < 15.0f)
            {
                nearest = i;
                break;
            }
        if (nearest != 255)
        {
            _activityCurrentNode = nearest;
            uint8 best = 255;
            for (uint8 i = 0; i < _activityNodeCount; ++i)
            {
                if (!(_activityNodes[nearest].links & (1u << i)) ||
                    _activityNodes[i].blockedMs ||
                    me->GetDistance(_activityNodes[i].x, _activityNodes[i].y,
                                    _activityNodes[i].z) < 30.0f)
                    continue;
                if (best == 255 || _activityNodes[i].visits < _activityNodes[best].visits)
                    best = i;
            }
            if (best != 255)
            {
                StartActivityTravel(_activityNodes[best].x, _activityNodes[best].y,
                                    _activityNodes[best].z, 0, best);
                SetCitizenActivityIntent(3, "learned-route-replay");
                sLog.outString("[WorldRoute] replay guid:%u from:%u to:%u",
                               me->GetGUIDLow(), nearest, best);
                return true;
            }
        }
    }
    for (uint32 attempt = 0; attempt < 8; ++attempt)
    {
        float x = me->GetPositionX();
        float y = me->GetPositionY();
        float z = me->GetPositionZ();
        if (!map->GetWalkRandomPosition(nullptr, x, y, z, frand(45.0f, 80.0f)))
            continue;
        float const dx = x - me->GetPositionX();
        float const dy = y - me->GetPositionY();
        float const hx = x - _activityHomeX;
        float const hy = y - _activityHomeY;
        if (dx * dx + dy * dy < 30.0f * 30.0f ||
            hx * hx + hy * hy > patrolRadius * patrolRadius ||
            map->GetTerrain()->GetZoneId(x, y, z) != _activityZone)
            continue;
        StartActivityTravel(x, y, z, 0);
        SetCitizenActivityIntent(3, "walkable-patrol");
        sLog.outString("[ZoneCitizen] travel guid:%u zone:%u purpose:patrol to:%.1f/%.1f",
                       me->GetGUIDLow(), _activityZone, x, y);
        return true;
    }
    _activityPauseMs = 5000;
    return true;
}

void ZoneCitizenAI::PrepareZoneSpawn(uint32 map, uint32 zone, uint32 team,
                                     float x, float y, float z)
{
    _spawnMap = map;
    _spawnZone = zone;
    _spawnTeam = team;
    _spawnX = x;
    _spawnY = y;
    _spawnZ = z;
    _spawnPrepared = true;
}

void ZoneCitizenAI::BeforeAddToMap(Player* player)
{
    if (!_spawnPrepared || !player)
        return;
    _spawnPrepared = false; // never reuse a stale location on a later login
    if (player->GetTeam() != _spawnTeam ||
        sTerrainMgr.GetZoneId(_spawnMap, _spawnX, _spawnY, _spawnZ) != _spawnZone)
    {
        sLog.outError("[ZoneCitizen] rejected stale spawn guid:%u zone:%u",
                      player->GetGUIDLow(), _spawnZone);
        return;
    }
    player->Relocate(_spawnX, _spawnY, _spawnZ, player->GetOrientation());
    player->SetLocationMapId(_spawnMap);
    sLog.outString("[ZoneCitizen] placed guid:%u map:%u zone:%u pos:%.1f/%.1f/%.1f",
                   player->GetGUIDLow(), _spawnMap, _spawnZone,
                   _spawnX, _spawnY, _spawnZ);
}

PlayerBotAI* CreatePlayerBotAI(std::string ainame)
{
    if (ainame == "ZoneCitizenAI")
        return new ZoneCitizenAI();
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
    // BL-002: an accepted owner order closes an active encounter as owner
    // override before mutating target/order state.
    _encounter.RecordOwnerOverride();
    // BL-003: persist the closed encounter before the new order state.
    TryPersistLearningSummary();
    _followSeq = seq;
    _followLeaderGuid = leaderGuid;
    _followGroupId = me->GetGroup() ? me->GetGroup()->GetId() : 0;
    _following = true;
    _followReached = false;
    _presence.ResetParty();
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
    // BL-002: an accepted owner order closes an active encounter as owner
    // override before mutating target/order state.
    _encounter.RecordOwnerOverride();
    // BL-003: persist the closed encounter before the new order state.
    TryPersistLearningSummary();
    _following = false;
    _followLeaderGuid = 0;
    _followReached = false;
    _presence.ResetParty();
    _pursuitLeash.Disarm(); // PORT-008: the pursuit is over; fresh budget for the next one
    // Invalidate the current goal immediately (TW-014 AC1).
    me->GetMotionMaster()->Clear(true);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[PlayerBot][Follow] inactive GUID:%u", me->GetGUIDLow());
}

void PlayerBotAI::ReleaseToWorld()
{
    FollowStop();
    _followGroupId = 0;
    sPlayerBotMgr.ConversationTransport().Invalidate(me->GetGUIDLow());
    _worldIntentPending = false;
    _worldIntentNextMs = 0;
    _worldRestUntilMs = 0;
    _combatCautionUntilMs = 0;
    _partyMemoryLesson = 0;
    _personalityChaseDist = kOwnerFollowChaseDist;
    _held = false;
    _assistTargetGuid = 0;
    _defendTargetGuid = 0;
    _independentHomeSet = false;
    _activityHomeSet = false;
    _travelActive = false;
    _travelHuntGuid = 0;
    _travelNudging = false;
    _activityNodeCount = 0;
    if (!me->IsInCombat())
        ClearTarget();
}

void PlayerBotAI::Hold(uint32 seq)
{
    if (!me || seq <= _followSeq)
        return;
    // BL-002: an accepted owner order closes an active encounter as owner
    // override before mutating target/order state.
    _encounter.RecordOwnerOverride();
    // BL-003: persist the closed encounter before the new order state.
    TryPersistLearningSummary();
    _followSeq = seq;
    _held = true;
    _following = false;
    _followLeaderGuid = 0;
    _followReached = false;
    _presence.ResetParty();
    _assistTargetGuid = 0; // PORT-005: a hold cancels an active assist
    _pursuitLeash.Disarm(); // PORT-008: a hold starts a fresh pursuit budget
    AbandonActivityTravel(); // a hold supersedes an in-flight citizen travel
    ClearTarget();
    me->InterruptNonMeleeSpells(false);
    if (me->GetVictim())
        me->CombatStop();
    // Clear the whole stack and explicitly install an idle generator;
    // Clear(..., true) does not repopulate an all-cleared motion stack.
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();
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
    // BL-002: an accepted owner order closes an active encounter as owner
    // override before mutating target/order state.
    _encounter.RecordOwnerOverride();
    // BL-003: persist the closed encounter before the new order state.
    TryPersistLearningSummary();
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
    if (!IsFollowOwnerAvailable())
        return nullptr;
    Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
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
    if (!me || !me->GetMap() || !botEntry)
        return false;
    uint32 const account = botEntry->ownerAccountId ? botEntry->ownerAccountId :
        (IsZoneCitizen() && botEntry->recruiterGuid == _followLeaderGuid
             ? botEntry->recruiterAccountId : 0);
    if (!account)
        return false;
    Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
    if (!owner || !owner->IsAlive() || !owner->GetSession() ||
        owner->GetSession()->GetAccountId() != account)
        return false;
    if (_followGroupId && (!me->GetGroup() || me->GetGroup()->GetId() != _followGroupId ||
        owner->GetGroup() != me->GetGroup()))
        return false;
    return true;
}

bool PlayerBotAI::MotionIdle() const
{
    if (!me)
        return true;
    return me->GetMotionMaster()->empty() ||
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE;
}

bool PlayerBotAI::IsOwnedCompanion() const
{
    return botEntry && (botEntry->ownerAccountId != 0 ||
        (IsZoneCitizen() && botEntry->recruiterAccountId != 0));
}

Player* PlayerBotAI::FindOwnerByAccount() const
{
    if (!botEntry)
        return nullptr;
    uint32 const account = botEntry->ownerAccountId ? botEntry->ownerAccountId :
        (IsZoneCitizen() ? botEntry->recruiterAccountId : 0);
    if (!account)
        return nullptr;
    HashMapHolder<Player>::MapType const& players = sObjectAccessor.GetPlayers();
    for (auto const& itr : players)
    {
        Player* p = itr.second;
        if (p && p->GetSession() &&
            p->GetSession()->GetAccountId() == account && p->IsAlive() &&
            (!botEntry->recruiterGuid || p->GetGUIDLow() == botEntry->recruiterGuid))
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
    // The leader must not be an owned companion: a bot-led party has no
    // player owner to plan for. An unowned roster entry (the lab owner
    // fixture) is still a player for planning purposes.
    PlayerBotEntry* leaderEntry = sPlayerBotMgr.FindBotByGuid(leaderLow);
    if (!leaderLow || (leaderEntry &&
        (leaderEntry->ownerAccountId ||
         (leaderEntry->ai && leaderEntry->ai->IsZoneCitizen()))))
        return; // bot-led: no player owner to plan for
    Player* leader = sObjectAccessor.FindPlayer(group->GetLeaderGuid());
    uint32 const leaderAccount = leader && leader->GetSession()
        ? leader->GetSession()->GetAccountId() : 0;
    if (!leaderAccount)
        return;
    Companion::Planner::PlannerTransport& transport =
        sPlayerBotMgr.PlannerTransport();
    if (groupId != _plannerGroupId || leaderLow != _plannerLeaderGuid)
    {
        // Any signature change (new group or new leader) is a party-
        // session change for the tracked key: kill its round so no
        // outstanding result can cross the membership boundary.
        if (_plannerLeaderGuid)
            transport.InvalidateSession(_plannerLeaderGuid);
        _plannerGroupId = groupId;
        _plannerLeaderGuid = leaderLow;
        _plannerLastSubmitMs = 0; // fresh session: immediate first round
    }
    uint32 const nowMs = WorldTimer::getMSTime();
    // Collect owned companions and citizens actively leased to this leader;
    // the lowest-GUID participant submits the shared party round.
    uint32 submitterLow = 0;
    uint32 botCount = 0;
    uint32 botLows[Companion::Planner::kMaxPartyBots] = {};
    uint8_t botCls[Companion::Planner::kMaxPartyBots] = {};
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        uint32 const slotLow = slot.guid.GetCounter();
        Player* member = sObjectAccessor.FindPlayer(ObjectGuid(slotLow));
        PlayerBotEntry* be = member ? sPlayerBotMgr.FindBotByGuid(slotLow) : nullptr;
        bool const ownedCompanion = be && be->ownerAccountId == leaderAccount;
        bool const leasedCitizen = be && be->ai && be->ai->IsZoneCitizen() &&
            be->recruiterAccountId == leaderAccount &&
            be->recruiterGuid == leaderLow && member->GetGroup() == group;
        if (!ownedCompanion && !leasedCitizen)
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
    // PORT-021 (KAP-558): consume the prior ready round BEFORE
    // submitting the next one. A submit supersedes a recorded-but-
    // unfetched response on the same tick; with the production tick
    // cadence (PlayerBot.UpdateMs >= the 2 s planner pace) the old
    // order starved every fetch and no offer was ever applied.
    // Record-only consumption (PORT-019 maps offers to behavior): every
    // owned companion fetches its own step from the shared round.
    Companion::Planner::Step offer;
    _plannerOfferValid =
        transport.FetchOffer(leaderLow, me->GetGUIDLow(), nowMs, offer);
    if (_plannerOfferValid)
    {
        _plannerOffer = offer;
        // PORT-019 (KAP-558): Preference is the only planner
        // action with a personality effect. It maps to bounded,
        // profile-allowlisted outcomes and fails closed to the
        // deterministic baseline.
        if (_plannerOffer.action ==
            (uint8_t)Companion::Planner::Action::Preference)
            ApplyPlannerPreference(nowMs);
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
}

void PlayerBotAI::ApplyPlannerPreference(uint32_t nowMs)
{
    using namespace Companion::Personality;
    // PORT-020 (KAP-558): the persisted per-companion profile
    // (seeded from config at first login); the config value
    // never overrides a persisted identity.
    Profile const profile = botEntry
        ? (Profile)botEntry->personalityProfile
        : Profile::None;
    if (profile == Profile::None || !me)
        return; // baseline: no declared profile, no effect
    PrefId id = PrefId::Invalid;
    uint8_t value = 0;
    if (!ParsePreference(_plannerOffer.preference, id, value))
        return; // unknown id: deterministic baseline
    switch (id)
    {
        case PrefId::FollowChase:
        {
            float yd = 0.0f;
            if (!MapChaseYd(profile, value, yd))
                return; // out-of-set value: keep the current distance
            if (yd != _personalityChaseDist)
            {
                _personalityChaseDist = yd;
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[Personality] chase GUID:%u profile:%s dist:%.1f",
                                   me->GetGUIDLow(), ProfileName(profile), yd);
            }
            break;
        }
        case PrefId::Expression:
        {
            // Rate excess or an out-of-set slot stays silent.
            // Expression is cosmetic: it never moves, targets or
            // interrupts anything, and a dead companion is silent.
            if (!me->IsAlive() ||
                nowMs - _personalityLastExprMs < kExprIntervalMs)
                return;
            char const* line = nullptr;
            if (!MapExpressionLine(profile, value, line))
                return;
            _personalityLastExprMs = nowMs;
            me->Say(line, LANG_UNIVERSAL);
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[Personality] expr GUID:%u profile:%s slot:%u",
                               me->GetGUIDLow(), ProfileName(profile), value);
            break;
        }
        default:
            break;
    }
}

void PlayerBotAI::WorldIntentStep()
{
    if ((!IsOwnedCompanion() && !IsZoneCitizen()) || !me ||
        !botEntry || !botEntry->persistent)
        return;
    if (!sPlayerBotMgr.IsWorldIntentEnabled())
        return;
    Companion::Conversation::ConversationTransport& transport =
        sPlayerBotMgr.ConversationTransport();
    if (me->GetGroup() || _following || _held || _assistTargetGuid ||
        (_questPhase != 0 && _questPhase != 4) ||
        !me->IsAlive() || me->IsInCombat())
    {
        if (_worldIntentPending)
            transport.Invalidate(me->GetGUIDLow());
        _worldIntentPending = false;
        _worldRestUntilMs = 0;
        return;
    }
    if (!transport.Enabled())
        return; // deterministic local roaming remains the fallback

    uint32 const nowMs = WorldTimer::getMSTime();
    if (_worldIntentPending)
    {
        std::string reply;
        if (transport.Poll(me->GetGUIDLow(), 0, 0, nowMs, reply))
        {
            _worldIntentPending = false;
            if (reply == "rest")
            {
                _worldRestUntilMs = nowMs + 60000;
                me->GetMotionMaster()->Clear(true);
            }
            else if (reply == "roam")
                _worldRestUntilMs = 0;
            if (reply == "rest" || reply == "roam")
                sLog.outString("[WorldIntent] accepted bot:%u intent:%s",
                               me->GetGUIDLow(), reply.c_str());
        }
        else if (nowMs - _worldIntentSubmitMs > 6000)
        {
            transport.Invalidate(me->GetGUIDLow());
            _worldIntentPending = false;
        }
    }
    if (_worldIntentPending || nowMs < _worldIntentNextMs ||
        (!IsZoneCitizen() && !FindOwnerByAccount()))
        return;

    // Persistent names are letters-only, but re-check at this boundary so
    // no database text can become a prompt instruction or URL component.
    std::string const& name = botEntry->name;
    if (name.size() < 2 || name.size() > 12)
        return;
    for (char c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return;
    std::string const context = name + "|" +
        (_worldRestUntilMs > nowMs ? "rest" : "roam");
    if (sPlayerBotMgr.SubmitWorldIntent(botEntry, context, nowMs))
    {
        _worldIntentPending = true;
        _worldIntentSubmitMs = nowMs;
        _worldIntentNextMs = nowMs + sPlayerBotMgr.GetWorldIntentIntervalMs();
    }
}

void PlayerBotAI::ConversationRoundStep()
{
    // PORT-022 (KAP-558): one bounded conversation reply per companion.
    // The world thread never waits: the transport already did the I/O on
    // its worker; here we only consume a reply whose party signature still
    // matches and whose age is within budget, then Say it. A dead
    // companion is silent and an empty reply says nothing.
    if ((!IsOwnedCompanion() && !IsZoneCitizen()) || !me)
        return;
    Companion::Conversation::ConversationTransport& transport =
        sPlayerBotMgr.ConversationTransport();
    Group* group = me->GetGroup();
    if (!group)
    {
        // No party: any outstanding reply is dead (leave/exit/rejoin).
        if (!_worldIntentPending)
            transport.Invalidate(me->GetGUIDLow());
        return;
    }
    uint32 const groupSig = group->GetId();
    uint32 const leaderLow = group->GetLeaderGuid().GetCounter();
    std::string reply;
    if (!transport.Poll(me->GetGUIDLow(), groupSig, leaderLow,
                        WorldTimer::getMSTime(), reply))
        return;
    // Final sanitize (defense in depth over the adapter's): bounded text,
    // no leading dot, no control characters. Never reinterpreted as a command.
    std::string safe;
    for (unsigned char ch : reply)
        if (ch >= 0x20 && ch <= 0x7e)
            safe.push_back((char)ch);
    size_t const b = safe.find_first_not_of(" ");
    size_t const e = safe.find_last_not_of(" ");
    if (b == std::string::npos)
        safe.clear();
    else
        safe = safe.substr(b, e - b + 1);
    while (!safe.empty() && safe[0] == '.')
        safe.erase(0, 1);
    if (safe.empty() || !me->IsAlive())
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Conversation] reply suppressed GUID:%u (empty or dead)",
                           me->GetGUIDLow());
        return;
    }
    me->Say(safe.c_str(), LANG_UNIVERSAL);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[Conversation] reply GUID:%u profile:%s len:%u",
                       me->GetGUIDLow(),
                       Companion::Personality::ProfileName(
                           botEntry
                               ? (Companion::Personality::Profile)botEntry->personalityProfile
                               : Companion::Personality::Profile::None),
                       (uint32)safe.size());
}

// ---------------------------------------------------------------------------
// PORT-027 (KAP-558): in-world status lines for the declared
// cooperative quest. The companion says its quest state so the
// owner does not need the server log. Only the server-maintained
// per-objective counts (QuestStatusData) are read - no inventory
// scan, no fabricated credit. Single kill objective: "6/15 <mob>";
// single item objective: "3/10 <item>"; otherwise the summed
// "9/17 objectives".
// ---------------------------------------------------------------------------
namespace
{
uint32 CoopQuestProgressHave(QuestStatusData const* qStatus)
{
    uint32 total = 0;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        total += qStatus->m_creatureOrGOcount[i] + qStatus->m_itemcount[i];
    return total;
}

std::string CoopQuestProgressLine(Quest const* qInfo, QuestStatusData const* qStatus)
{
    uint32 totalNeed = 0, killNeed = 0, itemNeed = 0;
    char const* killName = nullptr;
    char const* itemName = nullptr;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        uint32 const cNeed = qInfo->ReqCreatureOrGOCount[i];
        uint32 const iNeed = qInfo->ReqItemCount[i];
        totalNeed += cNeed + iNeed;
        if (cNeed)
        {
            killNeed += cNeed;
            if (!killName && qInfo->ReqCreatureOrGOId[i] > 0)
                if (CreatureInfo const* ci = sObjectMgr.GetCreatureTemplate((uint32)qInfo->ReqCreatureOrGOId[i]))
                    killName = ci->name.c_str();
        }
        if (iNeed)
        {
            itemNeed += iNeed;
            if (!itemName && qInfo->ReqItemId[i])
                if (ItemPrototype const* ip = sObjectMgr.GetItemPrototype(qInfo->ReqItemId[i]))
                    itemName = ip->Name1.c_str();
        }
    }
    uint32 const have = CoopQuestProgressHave(qStatus);
    std::string line = std::to_string(have) + "/" + std::to_string(totalNeed) + " ";
    if (killNeed && !itemNeed && killName)
        line += killName;
    else if (itemNeed && !killNeed && itemName)
        line += itemName;
    else
        line += "objectives";
    return line;
}
} // namespace

// ---------------------------------------------------------------------------
// PORT-030 (KAP-558): dynamic owner-quest mirror (kill objectives only).
//
// No per-quest configuration: the companion reads the owner's quest log
// and mirrors any quest the owner holds and that it can itself take, but
// only when every objective is a creature kill - objective progression
// is normal combat participation under the engine's vanilla group-credit
// rules (the companion fights the target mobs, both logs advance).
// Collect, talk, game-object, source-item and spell-cast objectives are
// skipped: the companion has no dedicated behavior for them, and
// mirroring would leave its log stalled. Accepts and turn-ins reuse the
// same authoritative helpers as the declared path (CanTakeQuest/
// CanAddQuest/AddQuest; CanCompleteQuest/CompleteQuest/CanRewardQuest/
// RewardQuest), re-validated live before acting.
//
// Gating matches the declared path: owned companion, owner alive and in
// the party, hold/combat/death suppression upstream, one action per
// tick, shared denial backoff (_coopQuestDenyTimer).
namespace
{
struct CoopQuestAnchorCheck
{
public:
    CoopQuestAnchorCheck(Player const* obj, uint32 questId, float dist,
                         bool finisher)
        : i_obj(obj), i_questId(questId), i_dist(dist), i_finisher(finisher) {}
    WorldObject const& GetFocusObject() const { return *i_obj; }
    bool operator()(Creature const* u)
    {
        if (!u->IsAlive())
            return false;
        // PORT-033 (KAP-558): the accept anchor must be a questrelation
        // holder (giver); the turn-in anchor must be an involvedquest
        // holder (finisher). The engine's complete/reward path requires
        // HasInvolvedQuest (QuestHandler.cpp:418/460), and a quest's
        // giver and finisher can be different creatures.
        bool const rel = i_finisher ? u->HasInvolvedQuest(i_questId)
                                    : u->HasQuest(i_questId);
        if (!rel)
            return false;
        return i_obj->IsWithinDistInMap(u, i_dist);
    }
private:
    Player const* const i_obj;
    uint32 const i_questId;
    float const i_dist;
    bool const i_finisher;
    CoopQuestAnchorCheck(CoopQuestAnchorCheck const&);
};

// Nearest live creature standing in the given quest relation within
// i_dist of from: give = questrelation (can hand the quest out),
// finish = involvedquest (the turn-in/reward target).
Creature* FindCoopQuestGiver(Player* from, uint32 questId, float i_dist)
{
    Creature* anchor = nullptr;
    CoopQuestAnchorCheck check(from, questId, i_dist, false);
    MaNGOS::CreatureLastSearcher<CoopQuestAnchorCheck> searcher(anchor, check);
    Cell::VisitGridObjects(from, searcher, i_dist);
    return anchor;
}

Creature* FindCoopQuestFinisher(Player* from, uint32 questId, float i_dist)
{
    Creature* anchor = nullptr;
    CoopQuestAnchorCheck check(from, questId, i_dist, true);
    MaNGOS::CreatureLastSearcher<CoopQuestAnchorCheck> searcher(anchor, check);
    Cell::VisitGridObjects(from, searcher, i_dist);
    return anchor;
}

// A quest is mirror-eligible iff every objective is a creature kill
// (see the block comment above).
bool IsKillOnlyQuest(Quest const* qInfo)
{
    if (!qInfo)
        return false;
    bool anyKill = false;
    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        if (qInfo->ReqCreatureOrGOCount[i] == 0)
            continue;
        if (qInfo->ReqCreatureOrGOId[i] <= 0)
            return false; // game-object objective
        anyKill = true;
    }
    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (qInfo->ReqItemId[i] != 0)
            return false; // collect objective
    for (uint32 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
        if (qInfo->ReqSourceId[i] != 0)
            return false; // source item required to accept
    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (qInfo->ReqSpell[i] != 0)
            return false; // cast objective
    return anyKill;
}
} // namespace

void PlayerBotAI::CooperativeQuestProgressAnnounce(uint32 questId, Quest const* qInfo, QuestStatusData const* qStatus, uint8 status)
{
    if (!qStatus)
        return;
    if (qStatus->m_rewarded)
    {
        _coopQuestAnnounce.erase(questId);
        return;
    }
    int32 const progress = (status == Companion::Quest::kStatusNone)
        ? -1 : (int32)CoopQuestProgressHave(qStatus);
    CoopQuestAnnounceState& st = _coopQuestAnnounce[questId];
    if (status != st.status)
    {
        if (status == Companion::Quest::kStatusComplete)
            me->Say("[Quest] Complete - turning in.", LANG_UNIVERSAL);
        st.status = status;
        st.progress = progress;
    }
    else if (status == Companion::Quest::kStatusInProgress && progress != st.progress)
    {
        st.progress = progress;
        std::string line = "[Quest] " + CoopQuestProgressLine(qInfo, qStatus) + ".";
        me->Say(line.c_str(), LANG_UNIVERSAL);
    }
}

// ---------------------------------------------------------------------------
// PORT-023 (KAP-558): one owner-driven cooperative quest action
// per tick.
//
// The companion mirrors the owner for one declared supported quest
// (PlayerBot.CooperativeQuestId). The value-only policy
// (Companion/Quest.h) selects Accept or TurnIn from the snapshot;
// this adapter re-validates everything live and calls the same
// authoritative quest helpers the packet handlers use (accept:
// CanTakeQuest/CanAddQuest/AddQuest; turn-in:
// CanCompleteQuest/CompleteQuest/CanRewardQuest/RewardQuest).
// Objective progression is normal combat participation - the
// vanilla tap/group credit rules move both personal quest logs;
// the companion never selects a quest, never leads, and never
// fabricates credit.
//
// Gating: owned companions only; the owner (session account ==
// the entry's owner account) must be a live member of the
// companion's party; hold, combat or death suppresses the step
// (death preempts at the lifecycle level in UpdateRecovery).
// Leaving the party stops cooperative planning without erasing
// the companion's persisted quest state.
void PlayerBotAI::CooperativeQuestStep(uint32 diff)
{
    if (!IsOwnedCompanion() || !me || !me->IsAlive() || !me->GetMap())
        return;
    if (_coopQuestDenyTimer)
    {
        _coopQuestDenyTimer = (_coopQuestDenyTimer > diff) ? _coopQuestDenyTimer - diff : 0;
        _coopTurninWalkGuid = 0; // PORT-034: no planning this tick
        return;
    }
    Group* group = me->GetGroup();
    if (!group)
    {
        _coopTurninWalkGuid = 0; // PORT-034
        return;
    }
    Player* owner = FindOwnerByAccount();
    if (!owner || !group->IsMember(owner->GetObjectGuid()))
    {
        _coopTurninWalkGuid = 0; // PORT-034
        return; // owner loss or party loss: no cooperative planning
    }

    uint32 const questId = sPlayerBotMgr.GetCooperativeQuestId();
    if (questId)
    {
        _coopTurninWalkGuid = 0; // PORT-034: declared mode owns the plan
        CooperativeDeclaredQuestStep(questId, owner);
        return;
    }
    // PORT-030 (KAP-558): dynamic mirror mode (kill-only objectives).
    if (sPlayerBotMgr.GetMirrorOwnerQuests())
        MirrorOwnerQuestStep(owner);
}

void PlayerBotAI::CooperativeDeclaredQuestStep(uint32 questId, Player* owner)
{
    Quest const* qInfo = sObjectMgr.GetQuestTemplate(questId);
    if (!qInfo)
        return;
    // The policy snapshot (value-only; Companion/Quest.h).
    Companion::Quest::Observation observation;
    observation.generation = _followSeq;
    observation.myStatus = (uint8_t)Companion::Quest::MapQuestStatus((uint32)me->GetQuestStatus(questId));
    observation.ownerStatus = (uint8_t)Companion::Quest::MapQuestStatus((uint32)owner->GetQuestStatus(questId));
    QuestStatusData const* qStatus = me->GetQuestStatusData(questId);
    observation.rewarded = (qStatus != nullptr && qStatus->m_rewarded);
    observation.held = _held;
    observation.inCombat = me->IsInCombat();
    observation.ownerInParty = true;
    observation.ownerAvailable = true;
    // PORT-027: in-world status line before the action path (the
    // accept/turn-in lines are said from those paths themselves).
    CooperativeQuestProgressAnnounce(questId, qInfo, qStatus, observation.myStatus);
    // Nearest live giver (questrelation) and finisher (involvedquest)
    // within INTERACTION_DISTANCE; the declared quest's giver and
    // finisher can be different creatures (PORT-033). Each action
    // re-looks-up its own anchor immediately before acting.
    observation.giverAvailable =
        (FindCoopQuestGiver(me, questId, INTERACTION_DISTANCE) != nullptr);
    observation.finisherAvailable =
        (FindCoopQuestFinisher(me, questId, INTERACTION_DISTANCE) != nullptr);
    Companion::Quest::Intent const intent = Companion::Quest::Select(observation);
    if (intent.action == Companion::Quest::Action::None)
        return;
    if (intent.action == Companion::Quest::Action::Accept)
    {
        // PORT-033: fresh giver anchor immediately before acting.
        Creature* anchor = FindCoopQuestGiver(me, questId, INTERACTION_DISTANCE);
        if (!anchor || !me->CanInteractWithQuestGiver(anchor))
            return;
        if (me->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            return; // the log moved between snapshot and act
        if (me->CanTakeQuest(qInfo, false) && me->CanAddQuest(qInfo, false))
        {
            me->AddQuest(qInfo, anchor);
            if (me->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            {
                sLog.outString("[CoopQuest] accepted GUID:%u quest:%u ownerStatus:%u anchor:%u",
                               me->GetGUIDLow(), questId,
                               (uint32)observation.ownerStatus, anchor->GetEntry());
                // PORT-027: in-world accept line with the starting count.
                QuestStatusData const* aStatus = me->GetQuestStatusData(questId);
                if (aStatus)
                {
                    std::string line = "[Quest] Accepted " + qInfo->GetTitle() + " (" +
                                       CoopQuestProgressLine(qInfo, aStatus) + ").";
                    me->Say(line.c_str(), LANG_UNIVERSAL);
                }
                CoopQuestAnnounceState& st = _coopQuestAnnounce[questId];
                st.status = Companion::Quest::kStatusInProgress;
                st.progress = aStatus ? (int32)CoopQuestProgressHave(aStatus) : 0;
                return;
            }
        }
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[CoopQuest] accept denied GUID:%u quest:%u interact:%u take:%u add:%u",
                           me->GetGUIDLow(), questId,
                           me->CanInteractWithQuestGiver(anchor) ? 1 : 0,
                           me->CanTakeQuest(qInfo, false) ? 1 : 0,
                           me->CanAddQuest(qInfo, false) ? 1 : 0);
        _coopQuestDenyTimer = 5000;
        return;
    }
    // TurnIn (choice index 0: a socketless session cannot choose).
    // PORT-033: fresh finisher anchor immediately before acting.
    Creature* anchor =
        FindCoopQuestFinisher(me, questId, INTERACTION_DISTANCE);
    if (!anchor || !me->CanInteractWithQuestGiver(anchor))
        return;
    // When the last credit lands the engine already marks the quest
    // COMPLETE, so the handler-shaped sequence applies: CompleteQuest
    // only while still INCOMPLETE; the reward step only needs COMPLETE.
    if (me->CanCompleteQuest(questId))
        me->CompleteQuest(questId);
    if (me->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
    {
        _coopQuestDenyTimer = 5000;
        return;
    }
    if (me->CanRewardQuest(qInfo, false))
    {
        uint32 xpBefore = me->GetUInt32Value(PLAYER_XP);
        me->RewardQuest(qInfo, 0, anchor, true);
        sLog.outString("[CoopQuest] turnin GUID:%u quest:%u anchor:%u xpBefore:%u "
                       "xpAfter:%u",
                       me->GetGUIDLow(), questId, anchor->GetEntry(), xpBefore,
                       me->GetUInt32Value(PLAYER_XP));
        // PORT-027: in-world turn-in line.
        std::string line = "[Quest] Turned in " + qInfo->GetTitle() + ".";
        me->Say(line.c_str(), LANG_UNIVERSAL);
        _coopQuestAnnounce.erase(questId);
    }
    else
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[CoopQuest] reward denied GUID:%u quest:%u", me->GetGUIDLow(), questId);
        _coopQuestDenyTimer = 5000;
    }
}


// KAP-558 review (finding 2): persisted mirror provenance. "The owner also
// holds this quest" is not proof the companion mirrored it (a seeded row
// or the declared quest path can share the quest id), so the mirror
// turn-in gate requires a marker row in bot_mirror_quest as well. Set on
// mirror accept, cleared on reward; BackfillMirrorQuestMarkers covers
// mirrors accepted before the table existed (one-shot legacy migration at
// login, owner cross-checked against the persisted owner quest logs).
bool PlayerBotAI::HasMirrorQuestMarker(uint32 questId) const
{
    if (!me)
        return false;
    QueryResult *res = CharacterDatabase.PQuery(
        "SELECT 1 FROM bot_mirror_quest WHERE char_guid = %u AND quest_id = %u LIMIT 1",
        me->GetGUIDLow(), questId);
    bool found = (res != nullptr);
    delete res;
    return found;
}

void PlayerBotAI::RecordMirrorQuestMarker(uint32 questId)
{
    if (!me)
        return;
    if (!CharacterDatabase.DirectPExecute(
            "INSERT INTO bot_mirror_quest (char_guid, quest_id, mirrored_at) "
            "VALUES (%u, %u, %u) ON DUPLICATE KEY UPDATE mirrored_at = VALUES(mirrored_at)",
            me->GetGUIDLow(), questId, (uint32)(WorldTimer::getMSTime() / 1000)))
    {
        sLog.outError("Playerbot mirror: marker set failed GUID:%u quest:%u", me->GetGUIDLow(), questId);
        return;
    }
    sLog.outString("[CoopQuest] mirror-marker GUID:%u quest:%u set", me->GetGUIDLow(), questId);
}

void PlayerBotAI::ClearMirrorQuestMarker(uint32 questId)
{
    if (!me)
        return;
    if (!CharacterDatabase.DirectPExecute(
            "DELETE FROM bot_mirror_quest WHERE char_guid = %u AND quest_id = %u",
            me->GetGUIDLow(), questId))
    {
        sLog.outError("Playerbot mirror: marker clear failed GUID:%u quest:%u", me->GetGUIDLow(), questId);
        return;
    }
    sLog.outString("[CoopQuest] mirror-marker GUID:%u quest:%u cleared", me->GetGUIDLow(), questId);
}

void PlayerBotAI::BackfillMirrorQuestMarkers()
{
    if (!me || !IsOwnedCompanion() || !sPlayerBotMgr.GetMirrorOwnerQuests() ||
        !sPlayerBotMgr.GetMirrorMarkerBackfill())
        return;
    QueryResult *own = CharacterDatabase.PQuery(
        "SELECT owner_account_id FROM bot_ownership WHERE char_guid = %u", me->GetGUIDLow());
    if (!own)
        return;
    Field *of = own->Fetch();
    uint32 ownerAccount = (of && !of->IsNULL()) ? of->GetUInt32() : 0;
    delete own;
    if (!ownerAccount)
        return;
    // Fork quest-status enum: 1 = COMPLETE, 3 = INCOMPLETE; an in-progress
    // row or an unrewarded complete row is the actionable legacy state.
    QueryResult *res = CharacterDatabase.PQuery(
        "SELECT q.quest FROM character_queststatus q "
        "WHERE q.guid = %u AND (q.status = 3 OR (q.status = 1 AND q.rewarded = 0)) "
        "AND EXISTS (SELECT 1 FROM character_queststatus oq "
        "  WHERE oq.quest = q.quest AND (oq.status = 1 OR oq.status = 3) "
        "  AND oq.guid IN (SELECT guid FROM characters WHERE account = %u))",
        me->GetGUIDLow(), ownerAccount);
    int32 marked = 0;
    while (res)
    {
        Field *f = res->Fetch();
        RecordMirrorQuestMarker(f->GetUInt32());
        ++marked;
        if (marked >= 32) // bound the legacy scan
            break;
    }
    delete res;
    if (marked && sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[CoopQuest] mirror-marker backfill GUID:%u marked:%d "
                       "(legacy rows accepted before the persisted marker)",
                       me->GetGUIDLow(), marked);
}

void PlayerBotAI::MirrorOwnerQuestStep(Player* owner)
{
    // Accept: the owner holds a mirror-eligible quest the companion does
    // not, and the quest anchor is in range. CanTakeQuest/CanAddQuest
    // (level, class, reputation, exclusive groups) stay authoritative.
    for (QuestStatusMap::const_iterator it = owner->getQuestStatusMap().begin();
         it != owner->getQuestStatusMap().end(); ++it)
    {
        uint32 const qid = it->first;
        if (it->second.m_status != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* qInfo = sObjectMgr.GetQuestTemplate(qid);
        if (!qInfo || !IsKillOnlyQuest(qInfo))
            continue;
        QuestStatusData const* my = me->GetQuestStatusData(qid);
        if (my)
        {
            // Skip unless a stale rewarded repeatable row can be re-taken.
            bool const staleDone = (my->m_status == QUEST_STATUS_COMPLETE &&
                                    my->m_rewarded && qInfo->IsRepeatable());
            if (!staleDone)
                continue;
        }
        Creature* anchor = FindCoopQuestGiver(me, qid, INTERACTION_DISTANCE);
        if (!anchor || !me->CanInteractWithQuestGiver(anchor))
            continue;
        if (me->CanTakeQuest(qInfo, false) && me->CanAddQuest(qInfo, false))
        {
            me->AddQuest(qInfo, anchor);
            if (me->GetQuestStatus(qid) != QUEST_STATUS_NONE)
            {
                RecordMirrorQuestMarker(qid); // KAP-558 review: persist the provenance
                sLog.outString("[CoopQuest] mirror-accepted GUID:%u quest:%u anchor:%u",
                               me->GetGUIDLow(), qid, anchor->GetEntry());
                QuestStatusData const* aStatus = me->GetQuestStatusData(qid);
                if (aStatus)
                {
                    std::string line = "[Quest] Accepted " + qInfo->GetTitle() + " (" +
                                       CoopQuestProgressLine(qInfo, aStatus) + ").";
                    me->Say(line.c_str(), LANG_UNIVERSAL);
                }
                CoopQuestAnnounceState& st = _coopQuestAnnounce[qid];
                st.status = Companion::Quest::kStatusInProgress;
                st.progress = aStatus ? (int32)CoopQuestProgressHave(aStatus) : 0;
                return;
            }
        }
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[CoopQuest] mirror accept denied GUID:%u quest:%u take:%u add:%u",
                           me->GetGUIDLow(), qid,
                           me->CanTakeQuest(qInfo, false) ? 1 : 0,
                           me->CanAddQuest(qInfo, false) ? 1 : 0);
        _coopQuestDenyTimer = 5000;
        return;
    }

    // Progress announce for in-flight mirrored quests (the companion's
    // log entry must also be in the owner's log).
    for (QuestStatusMap::const_iterator it = me->getQuestStatusMap().begin();
         it != me->getQuestStatusMap().end(); ++it)
    {
        if (it->second.m_status != QUEST_STATUS_INCOMPLETE &&
            it->second.m_status != QUEST_STATUS_COMPLETE)
            continue;
        if (owner->GetQuestStatus(it->first) == QUEST_STATUS_NONE)
            continue;
        Quest const* qInfo = sObjectMgr.GetQuestTemplate(it->first);
        if (!qInfo)
            continue;
        CooperativeQuestProgressAnnounce(it->first, qInfo, &it->second,
                                         Companion::Quest::MapQuestStatus((uint32)it->second.m_status));
    }

    // Turn-in: our own COMPLETE (unrewarded) quest whose finisher is in
    // range; the same handler-shaped sequence as the declared path.
    // KAP-558 review (finding 3): one clear before the scan; the old
    // per-entry clear let a later entry erase a walk target armed by an
    // earlier one in the same tick, handing motion back to normal follow
    // until the next quest tick. One nearest eligible finisher is tracked
    // during the scan and armed once after it.
    _coopTurninWalkGuid = 0;
    uint32 bestWalkQid = 0;
    uint64_t bestWalkGuid = 0;
    float bestWalkDist = 0.0f;
    for (QuestStatusMap::const_iterator it = me->getQuestStatusMap().begin();
         it != me->getQuestStatusMap().end(); ++it)
    {
        uint32 const qid = it->first;
        if (it->second.m_status != QUEST_STATUS_COMPLETE || it->second.m_rewarded)
            continue;
        // PORT-033 (KAP-558): the owner-log cross-check keeps an
        // owner-abandoned quest from being turned in for the companion.
        // KAP-558 review (finding 2) adds the persisted mirror marker:
        // "the owner also holds it" alone is not proof the companion
        // mirrored it (a seeded row or the declared quest path can share
        // the quest id); the marker in bot_mirror_quest is the provenance
        // record, and it survives restarts by construction.
        if (owner->GetQuestStatus(qid) == QUEST_STATUS_NONE)
            continue; // provenance gone
        if (!HasMirrorQuestMarker(qid))
        {
            if (sPlayerBotMgr.IsDebugEnabled() &&
                WorldTimer::getMSTime() > _coopTurninDebugUntilMs)
            {
                _coopTurninDebugUntilMs = WorldTimer::getMSTime() + 5000;
                sLog.outString("[CoopQuest] mirror turnin skip GUID:%u quest:%u no-marker "
                               "(accepted outside the mirror path; left to its own handler)",
                               me->GetGUIDLow(), qid);
            }
            continue;
        }
        Quest const* qInfo = sObjectMgr.GetQuestTemplate(qid);
        if (!qInfo)
            continue;
        // PORT-033: the turn-in anchor is the quest's finisher
        // (involvedquest), which can be a different creature than the
        // giver; the authoritative checks below re-validate at the act.
        Creature* anchor = FindCoopQuestFinisher(me, qid, INTERACTION_DISTANCE);
        if (!anchor)
        {
            // PORT-034 (KAP-558): group credit can complete the
            // quest while the finisher stands out of interaction
            // range (the party, not this companion, landed the
            // kills). Track the nearest eligible finisher; one bounded
            // walk is armed after the scan (KAP-558 review, finding 3).
            Creature* walkTarget =
                FindCoopQuestFinisher(me, qid, kCoopTurninWalkSearchRange);
            if (walkTarget && !_held && !me->IsInCombat() &&
                !me->HasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
            {
                float const d = me->GetDistance(walkTarget);
                if (!bestWalkGuid || d < bestWalkDist)
                {
                    bestWalkQid = qid;
                    bestWalkGuid = walkTarget->GetObjectGuid().GetRawValue();
                    bestWalkDist = d;
                }
            }
            continue;
        }
        if (!me->CanInteractWithQuestGiver(anchor))
        {
            // PORT-034 (KAP-558): the silent skip that blocked Lab D;
            // log the decision context (5 s throttle) when debug is on.
            if (sPlayerBotMgr.IsDebugEnabled() &&
                WorldTimer::getMSTime() > _coopTurninDebugUntilMs)
            {
                _coopTurninDebugUntilMs = WorldTimer::getMSTime() + 5000;
                // Replicate CanInteractWithNPC's gates so the skip
                // line names the failing check (bit = gate passed).
                uint32 gates = 0;
                if (anchor)
                {
                    if (me->IsInWorld() && !me->IsTaxiFlying())
                        gates |= 0x1;
                    if (!me->HasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
                        gates |= 0x2;
                    if (anchor->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
                        gates |= 0x4;
                    if (anchor->IsAlive())
                        gates |= 0x8;
                    if (!(me->IsAlive() && anchor->IsInvisibleForAlive()))
                        gates |= 0x10;
                    if (!anchor->GetCharmerGuid())
                        gates |= 0x20;
                    if (!anchor->IsHostileTo(me))
                        gates |= 0x40;
                    if (!anchor->IsInCombat())
                        gates |= 0x80;
                    if (!anchor->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))
                        gates |= 0x100;
                    if (anchor->IsWithinDistInMap(me, INTERACTION_DISTANCE))
                        gates |= 0x200;
                }
                sLog.outString(
                    "[CoopQuest] mirror turnin skip GUID:%u quest:%u "
                    "anchor:%u interact:%u dist:%.1f gates:%x",
                    me->GetGUIDLow(), qid,
                    anchor ? (uint32)anchor->GetGUIDLow() : 0,
                    (anchor && me->CanInteractWithQuestGiver(anchor)) ? 1 : 0,
                    anchor ? me->GetDistance(anchor) : -1.0f, gates);
            }
            continue;
        }
        if (me->CanCompleteQuest(qid))
            me->CompleteQuest(qid);
        if (me->GetQuestStatus(qid) != QUEST_STATUS_COMPLETE)
        {
            _coopQuestDenyTimer = 5000;
            return;
        }
        if (me->CanRewardQuest(qInfo, false))
        {
            uint32 xpBefore = me->GetUInt32Value(PLAYER_XP);
            me->RewardQuest(qInfo, 0, anchor, true);
            sLog.outString("[CoopQuest] mirror-turnin GUID:%u quest:%u anchor:%u "
                           "xpBefore:%u xpAfter:%u",
                           me->GetGUIDLow(), qid, anchor->GetEntry(), xpBefore,
                           me->GetUInt32Value(PLAYER_XP));
            ClearMirrorQuestMarker(qid); // KAP-558 review: provenance consumed
            _coopTurninWalkGuid = 0; // PORT-034: goal complete
            std::string line = "[Quest] Turned in " + qInfo->GetTitle() + ".";
            me->Say(line.c_str(), LANG_UNIVERSAL);
            _coopQuestAnnounce.erase(qid);
            return;
        }
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[CoopQuest] mirror reward denied GUID:%u quest:%u", me->GetGUIDLow(), qid);
        _coopQuestDenyTimer = 5000;
        return;
    }
    // KAP-558 review (finding 3): arm the walk once, for the single
    // nearest eligible finisher found by the scan. PORT-034 (KAP-558):
    // issue the walk here, not only from UpdateFollow - a companion
    // without an active follow order never runs the follow path, and
    // its group credit can still complete out of interaction range.
    // Hold and combat preempt it: the Hold executor clears motion after
    // this step, and the combat executor re-issues its own motion.
    // MotionIdle (not empty): the MotionMaster keeps the static idle
    // generator at the stack bottom, so empty() never observed the
    // no-motion state and the walk was armed but never issued
    // (cohort run #7).
    if (bestWalkGuid && !_held && !me->IsInCombat() &&
        !me->HasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        _coopTurninWalkGuid = bestWalkGuid;
        Creature* finisher = me->GetMap()->GetCreature(ObjectGuid(bestWalkGuid));
        if (finisher && finisher->IsInWorld() && finisher->IsAlive() && MotionIdle())
            me->GetMotionMaster()->MovePoint(
                0, finisher->GetPositionX(), finisher->GetPositionY(),
                finisher->GetPositionZ(), MOVE_PATHFINDING);
        if (sPlayerBotMgr.IsDebugEnabled() &&
            WorldTimer::getMSTime() > _coopTurninWalkLogUntilMs)
        {
            _coopTurninWalkLogUntilMs = WorldTimer::getMSTime() + 5000;
            sLog.outString(
                "[CoopQuest] turnin-walk GUID:%u quest:%u finisher:%u dist:%.1f",
                me->GetGUIDLow(), bestWalkQid,
                finisher ? finisher->GetGUIDLow() : 0, bestWalkDist);
        }
    }
}

// ---------------------------------------------------------------------------
// PORT-025 (KAP-558): one bounded bag-pressure episode per owned
// companion. The episode starts when the declared trigger fires
// (free item slots at or below kPressureFreeSlots, or a stored-loot
// event the inventory could not accept) and ends when pressure
// clears. At most one status line is said per episode; the no-vendor
// failure is reported on the first failed scan and then rate-limited.
// The vendor discovery is a bounded same-grid scan at the declared
// pace; the value policy (Companion/Inventory.h) classifies every
// item before any travel or sale, and the sale mirrors the vendor
// packet handler's guards and APIs exactly. Nothing is ever deleted
// or sold outside the declared junk set; an ambiguous item is
// protected.
uint8_t PlayerBotAI::CountFreeSlots() const
{
    if (!me)
        return 0;
    uint8_t free = 0;
    // Bag0 item slots (equipment slots are worn gear: protected).
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;
    // Sub-bags: declared capacity minus occupied slots.
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Item* bagItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (!bagItem || !bagItem->GetProto())
            continue;
        uint8 const cap = (uint8)bagItem->GetProto()->ContainerSlots;
        uint8 used = 0;
        for (uint8 slot = 0; slot < cap; ++slot)
            if (me->GetItemByPos(bag, slot))
                ++used;
        if (cap > used)
            free += (cap - used);
    }
    return free;
}

Companion::Inventory::ItemInfo PlayerBotAI::FillItemInfo(Item* item) const
{
    Companion::Inventory::ItemInfo info;
    if (!item)
        return info; // unknown: fail closed (protected)
    ItemPrototype const* proto = item->GetProto();
    if (!proto)
        return info;
    info.quality = proto->Quality;
    info.itemClass = proto->Class;
    info.bonding = proto->Bonding;
    info.sellPrice = proto->SellPrice;
    info.isEquipped = item->IsEquipped();
    info.isBag = item->IsBag();
    info.isQuest = proto->Class == ITEM_CLASS_QUEST || proto->StartQuest != 0 ||
                   me->HasQuestForItem(item->GetEntry());
    info.isKeyOrCurrency = proto->Class == ITEM_CLASS_KEY || proto->Class == ITEM_CLASS_MONEY;
    info.isUnique = proto->MaxCount == 1 && proto->Stackable == 0 &&
                    proto->Class != ITEM_CLASS_CONSUMABLE;
    info.hasEnchant = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) != 0 ||
                      item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0 ||
                      item->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_0) != 0 ||
                      item->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_1) != 0 ||
                      item->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_2) != 0 ||
                      item->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_3) != 0;
    // The upgrade-over-equipped verdict needs the authoritative slot
    // check; only the junk-matrix equipment classes can need it.
    if (info.quality == 0 &&
        (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR))
    {
        uint16 dest = 0;
        if (me->CanEquipItem(NULL_SLOT, dest, item, true) == EQUIP_ERR_OK && dest != NULL_SLOT)
        {
            uint8 const eslot = dest & 0xFF;
            if (eslot < EQUIPMENT_SLOT_END)
            {
                Item* equipped = me->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot);
                uint32 const equippedScore = equipped
                    ? Companion::Equipment::Score(
                          EquipmentStatsFromProto(equipped->GetProto(), me->GetLevel()))
                    : 0;
                info.upgradeOverEquipped = Companion::Equipment::Score(
                    EquipmentStatsFromProto(proto, me->GetLevel())) > equippedScore;
            }
        }
    }
    return info;
}

bool PlayerBotAI::VendorPressureStep(uint32 diff)
{
    if (!IsOwnedCompanion() || !me || !me->IsAlive() || !me->GetMap())
        return false;
    if (_held)
        return false; // a hold preempts the whole cleanup path

    uint8_t const freeSlots = CountFreeSlots();
    bool const pressure = Companion::Inventory::PressureActive(freeSlots, _inventoryPressure);
    if (pressure)
    {
        if (!_pressureReported)
        {
            // One bounded status line per pressure episode.
            _pressureReported = true;
            Companion::Personality::Profile const profile =
                botEntry ? (Companion::Personality::Profile)botEntry->personalityProfile
                         : Companion::Personality::Profile::None;
            char const* line = nullptr;
            if (Companion::Personality::MapPressureLine(profile, line))
                me->Say(line, LANG_UNIVERSAL);
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[Inventory] pressure GUID:%u free:%u stored:%u profile:%s",
                               me->GetGUIDLow(), freeSlots, _inventoryPressure,
                               Companion::Personality::ProfileName(profile));
        }
        if (!_vendorExhausted && !_vendorTargetGuid && !me->IsInCombat())
        {
            _vendorScanTimer += diff;
            if (_vendorFailReported)
                _vendorFailTimer += diff; // pace the next failure report
            if (_vendorScanTimer >= Companion::Inventory::kVendorScanPaceMs)
            {
                _vendorScanTimer = 0;
                BotVendorSearcher probe(me);
                Creature* found = nullptr;
                MaNGOS::CreatureLastSearcher<BotVendorSearcher> searcher(found, probe);
                Cell::VisitGridObjects(me, searcher, Companion::Inventory::kVendorSearchRadiusYd);
                Creature* vendor = probe.Best();
                if (vendor)
                {
                    _vendorTargetGuid = vendor->GetObjectGuid().GetRawValue();
                    _vendorFailReported = false;
                    _vendorFailTimer = 0;
                    if (sPlayerBotMgr.IsDebugEnabled())
                        sLog.outString("[Inventory] vendor found GUID:%u vendor:%u entry:%u dist:%.1f",
                                       me->GetGUIDLow(), vendor->GetGUIDLow(), vendor->GetEntry(),
                                       me->GetDistance(vendor));
                }
                else if (!_vendorFailReported ||
                         _vendorFailTimer >= Companion::Inventory::kVendorFailReportMs)
                {
                    // Bounded failure report: first scan immediate, then
                    // rate-limited. The companion waits for owner guidance
                    // without deleting or selling anything.
                    _vendorFailReported = true;
                    _vendorFailTimer = 0;
                    Companion::Personality::Profile const profile =
                        botEntry ? (Companion::Personality::Profile)botEntry->personalityProfile
                                 : Companion::Personality::Profile::None;
                    char const* line = nullptr;
                    if (Companion::Personality::MapNoVendorLine(profile, line))
                        me->Say(line, LANG_UNIVERSAL);
                    if (sPlayerBotMgr.IsDebugEnabled())
                        sLog.outString("[Inventory] no vendor GUID:%u radius:%u",
                                       me->GetGUIDLow(),
                                       (uint32)Companion::Inventory::kVendorSearchRadiusYd);
                }
            }
        }
    }
    else
    {
        if (_pressureReported || _vendorTargetGuid || _vendorExhausted)
        {
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[Inventory] pressure cleared GUID:%u free:%u",
                               me->GetGUIDLow(), freeSlots);
        }
        _pressureReported = false;
        _vendorTargetGuid = 0;
        _vendorExhausted = false;
        _vendorFailReported = false;
        _vendorFailTimer = 0;
        _vendorScanTimer = 0;
        _inventoryPressure = 0; // episode resolved: stored loot is acceptable again
    }
    return pressure;
}

bool PlayerBotAI::BotRepairStep(uint32 diff)
{
    if ((!IsZoneCitizen() && !IsOwnedCompanion()) || !me ||
        !me->IsAlive() || !me->GetMap() || me->IsInCombat() ||
        me->GetVictim() || _held ||
        _assistTargetGuid || me->IsNonMeleeSpellCasted(true))
    {
        _repairScanMs = 0;
        return false;
    }

    static uint32 const kRepairScanPaceMs = 5000;
    if (_repairScanMs < kRepairScanPaceMs)
    {
        _repairScanMs += diff;
        if (_repairScanMs < kRepairScanPaceMs)
            return false;
    }
    _repairScanMs = 0;

    bool damaged = false;
    uint32 durabilityBefore[EQUIPMENT_SLOT_END] = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item)
        {
            durabilityBefore[slot] = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            if (durabilityBefore[slot] < item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                damaged = true;
        }
    }
    if (!damaged)
        return false;

    BotRepairVendorSearcher probe(me);
    Creature* repairer = nullptr;
    MaNGOS::CreatureLastSearcher<BotRepairVendorSearcher> searcher(repairer, probe);
    Cell::VisitGridObjects(me, searcher, INTERACTION_DISTANCE);
    repairer = probe.Best();
    if (!repairer)
        return false;

    Creature* npc = me->GetNPCIfCanInteractWith(
        repairer->GetObjectGuid(), UNIT_NPC_FLAG_REPAIR);
    if (!npc)
        return false;

    uint32 const repairedCost = me->DurabilityRepairAll(
        true, me->GetReputationPriceDiscount(npc));
    bool repairedEquipment = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) > durabilityBefore[slot])
        {
            repairedEquipment = true;
            break;
        }
    }
    if (repairedEquipment)
        me->Say("I've got my gear repaired. Let's keep moving.", LANG_UNIVERSAL);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[BotRepair] GUID:%u vendor:%u cost:%u repaired:%u",
                       me->GetGUIDLow(), npc->GetGUIDLow(), repairedCost,
                       repairedEquipment ? 1 : 0);
    return repairedEquipment;
}

void PlayerBotAI::ExecuteVendor(Companion::Intent const& intent, uint32 diff)
{
    if (!me || !me->IsAlive() || !me->GetMap())
        return;
    Creature* vendor = me->GetMap()->GetCreature(ObjectGuid(intent.target));
    if (!vendor || !vendor->IsAlive() || !vendor->IsInWorld() || !vendor->IsVendor() ||
        !vendor->IsWithinDistInMap(me, Companion::Inventory::kVendorSearchRadiusYd))
    {
        // Stale or out-of-bounds vendor: drop it; the scan re-selects
        // at the declared pace while pressure remains.
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Inventory] vendor lost GUID:%u vendor:%u reason:stale",
                           me->GetGUIDLow(), (uint32)ObjectGuid(intent.target).GetCounter());
        _vendorTargetGuid = 0;
        me->GetMotionMaster()->Clear(false);
        return;
    }
    float const dist = me->GetDistance(vendor);
    if (dist > INTERACTION_DISTANCE)
    {
        // Bounded approach: one path per idle or moved vendor, like
        // the follow goal; a timed cast holds position so it completes.
        if (me->IsNonMeleeSpellCasted(true))
        {
            if (!me->GetMotionMaster()->empty())
                me->GetMotionMaster()->Clear(false);
            return;
        }
        float const vx = vendor->GetPositionX();
        float const vy = vendor->GetPositionY();
        float const vz = vendor->GetPositionZ();
        float const dx = vx - _followPathX;
        float const dy = vy - _followPathY;
        bool const moved = (dx * dx + dy * dy) > (2.0f * 2.0f);
        if (me->GetMotionMaster()->empty() || moved ||
            _followPathAgeMs >= kFollowPathRefreshMs)
        {
            _followPathX = vx;
            _followPathY = vy;
            _followPathZ = vz;
            _followPathAgeMs = 0;
            me->GetMotionMaster()->MovePoint(0, vx, vy, vz, MOVE_PATHFINDING);
        }
        else
            _followPathAgeMs += diff;
        return;
    }
    SellJunkToVendor(vendor);
}

void PlayerBotAI::SellJunkToVendor(Creature* vendor)
{
    if (!me || !vendor)
        return;
    // The authoritative interaction check the packet handler makes:
    // service flag, life, hostility, combat state, reputation and the
    // 5 yd range. A failed check drops the vendor (re-scan at pace).
    Creature* npc = me->GetNPCIfCanInteractWith(vendor->GetObjectGuid(), UNIT_NPC_FLAG_VENDOR);
    if (!npc)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Inventory] vendor lost GUID:%u vendor:%u reason:interact",
                           me->GetGUIDLow(), vendor->GetGUIDLow());
        _vendorTargetGuid = 0;
        return;
    }
    uint8 sold = 0;
    bool sawSellable = false; // an item passed the declared junk gate this pass
    auto sellPass = [&](uint8 bag, uint8 slot)
    {
        Item* item = me->GetItemByPos(bag, slot);
        if (!item)
            return;
        Companion::Inventory::ItemInfo const info = FillItemInfo(item);
        if (Companion::Inventory::Classify(info) != Companion::Inventory::Verdict::Sellable)
            return; // protected: leave it
        sawSellable = true;
        if (sold >= Companion::Inventory::kMaxSalesPerTick)
            return; // pace: the rest is sold on later ticks
        // Handler-shaped full-stack sale: re-resolve by GUID and apply
        // the same guards the vendor packet handler applies.
        Item* live = me->GetItemByGuid(item->GetObjectGuid());
        if (!live || me->GetObjectGuid() != live->GetOwnerGuid() ||
            me->IsBankPos(live->GetPos()) ||
            me->GetLootGuid() == live->GetObjectGuid() ||
            (live->IsBag() && !((Bag*)live)->IsEmpty()))
            return;
        ItemPrototype const* proto = live->GetProto();
        if (!proto || proto->SellPrice == 0)
            return;
        uint32 money = proto->SellPrice * live->GetCount();
        // Handler parity: a negative-charge spell prices the item
        // proportionally to the charges remaining.
        for (auto i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            auto const &spell = proto->Spells[i];
            if (spell.SpellId != 0 && spell.SpellCharges < 0)
            {
                auto const multiplier = static_cast<float>(live->GetSpellCharges(i)) /
                                       static_cast<float>(spell.SpellCharges);
                money *= multiplier;
                break;
            }
        }
        me->LogItem(live, LogItemAction::Sold);
        me->ItemRemovedQuestCheck(live->GetEntry(), live->GetCount());
        me->RemoveItem(live->GetBagSlot(), live->GetSlot(), true);
        me->InterruptSpellsWithCastItem(live);
        live->RemoveFromUpdateQueueOf(me);
        me->AddItemToBuyBackSlot(live, money, npc->GetObjectGuid());
        me->LogModifyMoney(money, "SellItem", npc->GetObjectGuid(), live->GetEntry());
        ++sold;
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Inventory] sold GUID:%u item:%u count:%u money:%u vendor:%u",
                           me->GetGUIDLow(), live->GetEntry(), live->GetCount(), money,
                           npc->GetGUIDLow());
    };
    // Bounded pass: bag0 item slots, then sub-bags (the shape
    // EvaluateReceivedEquipment already uses).
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        sellPass(INVENTORY_SLOT_BAG_0, slot);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        for (uint8 slot = 0; slot < 36; ++slot)
            sellPass(bag, slot);
    if (!sawSellable)
    {
        // Every remaining item is protected: the declared junk set is
        // empty. Stop the vendor travel for this episode; the prior
        // owner goal resumes and the companion waits for owner
        // guidance.
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Inventory] exhausted GUID:%u free:%u",
                           me->GetGUIDLow(), CountFreeSlots());
        _vendorExhausted = true;
        _vendorTargetGuid = 0;
        me->GetMotionMaster()->Clear(false);
    }
}

bool PlayerBotAI::UpdateCompanion(uint32 diff)
{
    uint32 const nowMs = WorldTimer::getMSTime();
    if (_combatCautionUntilMs && nowMs >= _combatCautionUntilMs)
    {
        _combatCautionUntilMs = 0;
        _personalityChaseDist = kOwnerFollowChaseDist;
        _partyMemoryLesson = 0;
    }
    // A client-side uninvite bypasses .botdismiss. Drop the old group-bound
    // order here so an owned companion resumes its off-duty world activity
    // instead of holding still behind an unavailable follow leader.
    if (_followGroupId && (!me->GetGroup() ||
        me->GetGroup()->GetId() != _followGroupId))
    {
        ReleaseToWorld();
        if (botEntry)
            botEntry->defendEnabled = false;
        sLog.outString("[PlayerBot][Party] released stale group order GUID:%u", me->GetGUIDLow());
    }
    PresenceStep(diff);
    WorldIntentStep();
    // PORT-018 (KAP-558): planner rounds key on party membership, not
    // order state, so the round step runs before the no-order early
    // return.
    PlannerRoundStep(diff);
    ConversationRoundStep(); // PORT-022: consume one bounded reply, if any
    CooperativeQuestStep(diff); // PORT-023: at most one cooperative quest action
    if (!_following && !_held && !_assistTargetGuid)
        return false;
    // PORT-025 (KAP-558): bag-pressure episode bookkeeping (one
    // status line per episode) + bounded vendor discovery. Runs
    // before the observation fill so the Vendor candidate is visible
    // to Select; hold, combat, recovery and owner loss preempt the
    // travel and sale at the policy and executor level.
    bool const bagPressure = VendorPressureStep(diff);
    Companion::Observation observation;
    observation.generation = _followSeq;
    observation.following = _following;
    observation.held = _held;
    observation.ownerAvailable = IsFollowOwnerAvailable();
    // PORT-025: the resolved vendor (re-validated live) and the
    // declared pressure state; the executor re-resolves the vendor
    // from its GUID and re-checks the world before any approach or
    // sale.
    observation.bagPressure = bagPressure;
    if (_vendorTargetGuid)
    {
        Creature* vendor = me->GetMap()->GetCreature(ObjectGuid(_vendorTargetGuid));
        if (vendor && vendor->IsAlive() && vendor->IsInWorld() && vendor->IsVendor())
            observation.vendorTarget = vendor->GetObjectGuid().GetRawValue();
        else
            _vendorTargetGuid = 0;
    }
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
    if ((intent.action == Companion::Action::Follow || intent.action == Companion::Action::Loot ||
         intent.action == Companion::Action::Vendor) &&
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
    if ((intent.action == Companion::Action::Follow || intent.action == Companion::Action::Loot ||
         intent.action == Companion::Action::Vendor) &&
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

// Owner distress is a transition, sampled from the live owner while this bot
// follows in the same party. One eligible companion speaks; the shared cue
// cooldown also covers regroup speech.
void PlayerBotAI::PresenceStep(uint32 diff)
{
    _presence.Tick(diff);
    if (!me || !IsOwnedCompanion() || !me->IsAlive() || !_following ||
        _held || !me->GetGroup() || !IsFollowOwnerAvailable())
    {
        _presence.ResetParty();
        return;
    }

    Player* owner = me->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, _followLeaderGuid));
    if (!owner || !owner->GetMaxHealth())
        return;
    uint8_t const healthPct = static_cast<uint8_t>(
        (static_cast<uint64_t>(owner->GetHealth()) * 100) / owner->GetMaxHealth());
    bool const canSpeak = me->GetDistance(owner) <= 30.0f &&
        IsPresenceSpeaker(owner);
    Companion::Presence::Cue const cue = _presence.ObserveOwner(
        true, healthPct, owner->IsInCombat(), canSpeak, me->GetGUIDLow());
    SayPresenceCue(cue);
}

bool PlayerBotAI::IsPresenceSpeaker(Player const* owner) const
{
    if (!me || !owner || !me->GetGroup() || owner->GetGroup() != me->GetGroup() ||
        !owner->GetSession() || !botEntry)
        return false;
    uint32 const account = owner->GetSession()->GetAccountId();
    bool const eligible = botEntry->ownerAccountId == account ||
        (IsZoneCitizen() && botEntry->recruiterAccountId == account &&
         botEntry->recruiterGuid == owner->GetGUIDLow());
    if (!eligible)
        return false;
    uint32 speakerLow = 0;
    for (Group::MemberSlot const& slot : me->GetGroup()->GetMemberSlots())
    {
        uint32 const low = slot.guid.GetCounter();
        Player* member = sObjectAccessor.FindPlayer(slot.guid);
        PlayerBotEntry* entry = member ? sPlayerBotMgr.FindBotByGuid(low) : nullptr;
        bool const samePartyOwner = entry && entry->ai &&
            (entry->ownerAccountId == account ||
             (entry->ai->IsZoneCitizen() && entry->recruiterAccountId == account &&
              entry->recruiterGuid == owner->GetGUIDLow()));
        if (!member || !samePartyOwner ||
            !member->IsInWorld() || member->GetGroup() != me->GetGroup() ||
            member->GetMap() != me->GetMap() || !member->IsAlive() ||
            entry->ai->_held || !entry->ai->_following ||
            entry->ai->_followLeaderGuid != owner->GetGUIDLow() ||
            member->GetDistance(owner) > 30.0f)
            continue;
        if (!speakerLow || low < speakerLow)
            speakerLow = low;
    }
    return speakerLow == me->GetGUIDLow();
}

void PlayerBotAI::SayPresenceCue(Companion::Presence::Cue cue)
{
    if (cue == Companion::Presence::Cue::None || !me || !me->IsAlive())
        return;

    Companion::Personality::Profile const profile = botEntry
        ? (Companion::Personality::Profile)botEntry->personalityProfile
        : Companion::Personality::Profile::None;
    uint32 const sequence = _presence.sequence++;
    char const* line = cue == Companion::Presence::Cue::Regroup
        ? Companion::Presence::RegroupLine(profile, sequence)
        : Companion::Presence::OwnerInjuredLine(profile, sequence);
    me->Say(line, LANG_UNIVERSAL);
    if (sPlayerBotMgr.IsDebugEnabled())
        sLog.outString("[Presence] %s GUID:%u profile:%s seq:%u next:%u",
                       cue == Companion::Presence::Cue::Regroup ? "regroup" : "owner-injured",
                       me->GetGUIDLow(), Companion::Personality::ProfileName(profile),
                       sequence, _presence.remainingMs);
}

// ---------------------------------------------------------------------------
// PORT-009 (KAP-558): one normal companion death and recovery path. While
// dead, Update() skips every offensive, loot and quest path; an owned
// owned companion or zone citizen reclaims its own corpse
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
    // Persistent owned companions and zone citizens reclaim their own
    // corpses even without an order. Other ambient bots keep dead-idle.
    if (!IsOwnedCompanion() && !IsZoneCitizen() &&
        !_following && !_held && !_assistTargetGuid)
        return false;
    if (!_recoveryDead)
    {
        _recoveryDead = true;
        _presence.ResetParty();
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
    // the companion is not actively moving (MotionIdle - empty() alone
    // is never true: the idle generator sits at the stack bottom) and
    // the retry window has elapsed, so a failed path is retried at a
    // bounded pace and the companion is never trapped by an
    // unreachable corpse.
    if (MotionIdle() && _recoveryWalkMs <= diff)
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
                // The assist contract is the owner's canonical gate
                // (IsAssistLegalTarget): any non-friendly targetable
                // creature is attackable, because quest mobs are usually
                // passive wildlife. The non-forced CanAttack also
                // requires faction hostility, which the real player
                // melee path does not, so assist revalidation uses the
                // forced form for player executors only; the
                // ContinueCombat/Damage verdicts keep their reviewed
                // semantics unchanged.
                snap.canAttack = me->CanAttack(target,
                                               me->IsPlayer() &&
                                               req.source == Companion::Combat::Source::Assist);
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
        // BL-002: a dropped engagement closes any active encounter with its
        // explicit reason (idempotent: a matching-target death already
        // ended it as target-death).
        _encounter.End(Companion::Encounter::EndReason::TargetInvalid);
        // BL-003: persist the closed encounter before the engagement drops.
        TryPersistLearningSummary();
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
        // BL-002: an expired pursuit closes any active encounter.
        _encounter.End(Companion::Encounter::EndReason::LeashExpired);
        // BL-003: persist the closed encounter before the pursuit ends.
        TryPersistLearningSummary();
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
    // BL-002: observe-only encounter recording (no behavior change): the
    // legal engagement begins or continues one encounter and ticks its
    // duration once per ExecuteCombat call; a new target closes the prior
    // encounter as target-changed before beginning.
    EncounterEngage(req.source,
                    Companion::Combat::RouteOffense(
                        req.source,
                        req.source == Companion::Combat::Source::Assist && IsDeclaredTank()),
                    req.target, diff);
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
    UpdateCombatPursuit(target, diff);
    if (me->CanReachWithMeleeAutoAttack(target))
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
    // BL-001B: the per-source offense route is a pure value decision in
    // Companion::Combat; only an Assist with the declared-tank gate true
    // owns the Tank branch (below); every other source takes the rotation path.
    Companion::Combat::OffenseRoute const route =
        Companion::Combat::RouteOffense(
            req.source,
            req.source == Companion::Combat::Source::Assist && IsDeclaredTank());
    if (route == Companion::Combat::OffenseRoute::Tank)
    {
        Companion::Tank::Observation const obs = FillTankObservation(target);
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[Tank] threat GUID:%u me:%u owner:%u t:%u victim:%u",
                           me->GetGUIDLow(), obs.tankThreat, obs.ownerThreat,
                           target->GetGUIDLow(), obs.victimGuid);
        Companion::Tank::Decision const decision = Companion::Tank::SelectAction(obs);
        // BL-002: the tank decision boundary (Taunt or ordinary attack).
        _encounter.RecordDecision(
            decision.action == Companion::Tank::Action::Taunt
                ? Companion::Tank::kDeclaredTankTaunt : 0, true);
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
    if (intent.action == Companion::Action::Vendor)
    {
        // PORT-025 (KAP-558): the hold gate above already stopped
        // motion for Hold / held / owner loss; the bounded vendor
        // approach and handler-shaped sale run only when the owner
        // goal is otherwise idle. A stale or vanished vendor drops
        // the goal; the scan re-selects at the declared pace.
        ExecuteVendor(intent, diff);
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
        // Repairing is an opportunistic service stop. Keep the follow order
        // intact, pause its motion for this tick only when a repair completes,
        // and resume following on the next update. Combat, assist, defend,
        // loot, hold and bag-pressure intents retain their normal priority.
        if (BotRepairStep(diff))
            return;
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

    // PORT-034 (KAP-558): bounded turn-in walk. CooperativeQuestStep
    // armed a finisher target when a mirrored quest completed with
    // the finisher out of interaction range; steer to it at
    // follow-motion level until in range, then let the next quest
    // tick act. A gone or unreachable target drops the goal and
    // normal follow resumes.
    if (_coopTurninWalkGuid)
    {
        Creature* finisher =
            me->GetMap()->GetCreature(ObjectGuid(_coopTurninWalkGuid));
        if (finisher && finisher->IsInWorld() && finisher->IsAlive() &&
            !finisher->IsWithinDistInMap(me, INTERACTION_DISTANCE))
        {
            if (MotionIdle())
                me->GetMotionMaster()->MovePoint(
                    0, finisher->GetPositionX(), finisher->GetPositionY(),
                    finisher->GetPositionZ(), MOVE_PATHFINDING);
            return true;
        }
        _coopTurninWalkGuid = 0; // reached or lost: normal follow resumes
    }

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

    // KAP-558 review (finding 4): stable per-companion party slot. The
    // slot point is shared by the out-of-range approach and the in-range
    // rest position, so compute it once. Slot 0 is the legacy right-side
    // point (side vector for facing (sin o, cos o) is (cos o, -sin o),
    // PORT-032), unchanged for a solo follower; each further party slot
    // steps 45 deg around the leader, so a party of companions fans out
    // instead of pathing one shared point (crowding + repeated path
    // corrections). The kFollowRange stop rule still owns arrival, so
    // the companion never overlaps the player's center.
    float const lx = leader->GetPositionX();
    float const ly = leader->GetPositionY();
    float const lz = leader->GetPositionZ();
    float const o = leader->GetOrientation();
    int slot = 0;
    if (Group* grp = leader->GetGroup())
    {
        ObjectGuid const leaderGuid = grp->GetLeaderGuid();
        if (leaderGuid != me->GetObjectGuid())
        {
            for (Group::MemberSlotList::const_iterator it = grp->GetMemberSlots().begin();
                 it != grp->GetMemberSlots().end(); ++it)
            {
                if (it->guid == leaderGuid)
                    continue;
                if (it->guid == me->GetObjectGuid())
                    break;
                ++slot;
            }
        }
    }
    float const a = -o + slot * 0.7853982f; // 45 deg per slot
    float const tx = lx + cosf(a) * kFollowSideOffsetYd;
    float const ty = ly + sinf(a) * kFollowSideOffsetYd;

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
        if (me->GetDistance(leader) >= Companion::Presence::kRegroupArmDistanceYd)
            _presence.MarkAway();
        // The same path-find-to-position pattern idle wander and the quest
        // giver pursuit use (a player chase is a no-op without a victim).
        // PORT-008 (KAP-558): throttle follow path re-issuance. The path is
        // re-issued only when the motion master is empty (the walk finished
        // but the owner is still out of range), the owner moved more than
        // 2.0 yd (2D) from the last issued target, or the issued target is
        // stale (kFollowPathRefreshMs).
        {
            float const dpx = tx - _followPathX;
            float const dpy = ty - _followPathY;
            bool const moved = (dpx * dpx + dpy * dpy) > (2.0f * 2.0f);
            if (me->GetMotionMaster()->empty() || moved ||
                _followPathAgeMs >= kFollowPathRefreshMs)
            {
                _followPathX = tx;
                _followPathY = ty;
                _followPathZ = lz;
                _followPathAgeMs = 0;
                me->GetMotionMaster()->MovePoint(0, tx, ty, lz, MOVE_PATHFINDING);
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot][Follow] path GUID:%u leader:%u slot:%d "
                                   "target:%.2f,%.2f",
                                   me->GetGUIDLow(), leader->GetGUIDLow(), slot, tx, ty);
            }
            else
                _followPathAgeMs += diff;
        }
        return true;
    }

    // KAP-558 review (finding 4): in-range rest at the own slot point.
    // Followers that all stopped at the leader's shared approach point
    // crowd there; rest at the slot instead. Re-issue only when more
    // than 0.75 yd from it, so a settled party stays still.
    {
        float const dsx = me->GetPositionX() - tx;
        float const dsy = me->GetPositionY() - ty;
        if (dsx * dsx + dsy * dsy > 0.75f * 0.75f)
        {
            if (me->IsNonMeleeSpellCasted(true))
            {
                if (!me->GetMotionMaster()->empty())
                    me->GetMotionMaster()->Clear(false);
                return true; // cast-interruptible: hold position (PORT-015)
            }
            float const dpx = tx - _followPathX;
            float const dpy = ty - _followPathY;
            bool const moved = (dpx * dpx + dpy * dpy) > (0.75f * 0.75f);
            if (me->GetMotionMaster()->empty() || moved ||
                _followPathAgeMs >= kFollowPathRefreshMs)
            {
                _followPathX = tx;
                _followPathY = ty;
                _followPathZ = lz;
                _followPathAgeMs = 0;
                me->GetMotionMaster()->MovePoint(0, tx, ty, lz, MOVE_PATHFINDING);
            }
            else
                _followPathAgeMs += diff;
            return true;
        }
    }

    if (!me->GetMotionMaster()->empty())
        me->GetMotionMaster()->Clear(false);
    _followPathAgeMs = 0; // PORT-008: fresh path window after a completed approach
    if (!_followReached)
    {
        _followReached = true;
        bool const canSpeak = IsOwnedCompanion() && !me->IsInCombat() &&
            !_held && me->GetGroup() && IsFollowOwnerAvailable() &&
            IsPresenceSpeaker(leader);
        SayPresenceCue(_presence.Arrived(canSpeak, me->GetGUIDLow()));
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot][Follow] reached GUID:%u leader:%u dist:%.2f seq:%u",
                           me->GetGUIDLow(), _followLeaderGuid, me->GetDistance(leader), _followSeq);
    }
    return true;
}
