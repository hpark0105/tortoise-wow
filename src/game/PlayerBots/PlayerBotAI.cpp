#include "PlayerBotAI.h"
#include "Player.h"
#include "DBCStores.h"
#include "Log.h"
#include "SocialMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "MoveSpline.h"
#include "PlayerBotMgr.h"
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

    if (!me->IsAlive())
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
                RememberLootTarget(victim);
            if (sPlayerBotMgr.IsDebugEnabled())
                sLog.outString("[PlayerBot] fighting GUID:%u victim:%u dist:%.2f",
                               me->GetGUIDLow(), victim->GetGUIDLow(), me->GetDistance(victim));
            if (!me->CanReachWithMeleeAutoAttack(victim))
                me->GetMotionMaster()->MoveChase(victim);

            if (_abilityTimer == 0)
            {
                if (uint32 spellId = SelectOffensiveSpell(victim))
                {
                    me->CastSpell(victim, spellId, false);
                    _abilityTimer = urand(2000, 4000);
                }
                else
                    me->Attack(victim, true);
            }
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
                if (sPlayerBotMgr.IsDebugEnabled())
                    sLog.outString("[PlayerBot] fighting GUID:%u victim:%u dist:%.2f hp:%u/%u",
                                   me->GetGUIDLow(), held->GetGUIDLow(), me->GetDistance(held),
                                   held->GetHealth(), held->GetMaxHealth());
                if (_abilityTimer == 0)
                {
                    if (uint32 spellId = SelectOffensiveSpell(held))
                    {
                        me->CastSpell(held, spellId, false);
                        _abilityTimer = urand(2000, 4000);
                    }
                    else
                        me->Attack(held, true);
                }
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
            if (Unit* target = me->SelectNearestTarget(30.0f))
            {
                // Autonomous companions never initiate PvP. A hostile player
                // may still be the current victim while defending; this guard
                // applies only to idle target acquisition.
                if (!target->IsPlayer())
                {
                    if (target->GetTypeId() == TYPEID_UNIT)
                        RememberLootTarget(target);
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
            if (GetAliveHeldTarget() || me->SelectNearestTarget(30.0f))
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
                float radius = frand(8.0f, 20.0f);

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
    if (!_lootTargetGuid || !me || !me->GetMap())
        return nullptr;
    Creature* creature = me->GetMap()->GetCreature(_lootTargetGuid);
    if (creature && creature->IsAlive())
        return creature;
    return nullptr;
}

void PlayerBotAI::ClearTarget()
{
    _lootTargetGuid = ObjectGuid();
    _lootRetryCount = 0;
}

bool PlayerBotAI::TryLootDefeatedTarget()
{
    if (!_lootTargetGuid || !me->GetMap())
        return false;

    Creature* creature = me->GetMap()->GetCreature(_lootTargetGuid);
    if (!creature)
    {
        if (sPlayerBotMgr.IsDebugEnabled())
            sLog.outString("[PlayerBot] loot target missing GUID:%u target:%u",
                           me->GetGUIDLow(), _lootTargetGuid.GetCounter());
        _lootTargetGuid = ObjectGuid();
        _lootRetryCount = 0;
        return false;
    }
    if (creature->IsAlive())
    {
        // The target is still alive; it is the held combat target that the
        // combat loop keeps pursuing. Do not clear it here.
        return false;
    }
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
        _lootTargetGuid = ObjectGuid();
        _lootRetryCount = 0;
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
        _lootTargetGuid = ObjectGuid();
        _lootRetryCount = 0;
    }
    return true;
}

void PlayerBotAI::RememberLootTarget(Unit* unit)
{
    if (!unit)
        return;
    ObjectGuid const guid = unit->GetObjectGuid();
    if (_lootTargetGuid == guid)
        return;
    _lootTargetGuid = guid;
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
        if (me->CanCompleteQuest(_questId))
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
        me->GetMotionMaster()->MoveChase(giver);
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

    auto distOk = [&](Action const& a) -> bool
    {
        float dist = me->GetCombatDistance(target);
        if (a.minRange > 0.0f && dist < a.minRange)
            return false;
        if (a.maxRange > 0.0f && dist > a.maxRange)
            return false;
        if (a.meleeOnly && !me->CanReachWithMeleeAutoAttack(target))
            return false;
        return true;
    };

    auto canCast = [&](uint32 spellId, bool requireMissingAura) -> bool
    {
        if (!spellId)
            return false;
        if (!me->HasSpell(spellId))
            return false;
        if (me->HasSpellCooldown(spellId))
            return false;
        SpellEntry const* info = sSpellMgr.GetSpellEntry(spellId);
        if (!info)
            return false;
        Powers powerType = static_cast<Powers>(info->powerType);
        if (me->GetPower(powerType) < info->manaCost)
            return false;
        if (requireMissingAura && TargetHasAuraFromChain(target, spellId))
            return false;
        return true;
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

    for (auto const& act : actions)
    {
        if (!distOk(act))
            continue;
        uint32 spellId = GetHighestKnownSpell(act.baseSpell);
        if (!canCast(spellId, act.requiresMissingAura))
            continue;
        return spellId;
    }

    return 0;
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
