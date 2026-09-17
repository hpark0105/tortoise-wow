#ifndef MANGOS_PLAYERBOTAI_H
#define MANGOS_PLAYERBOTAI_H

#include "PlayerAI.h"
#include "WorldSession.h"
#include "Companion/Policy.h"
#include "Companion/Combat.h"
#include "Companion/Tank.h"
#include "Companion/Healer.h"
#include "Companion/Damage.h"
#include "Companion/PlannerTransport.h"
#include "Companion/ConversationTransport.h"
#include "Companion/Personality.h"
#include "Companion/Quest.h"
#include "Companion/Equipment.h"
#include "Companion/Inventory.h"
#include <utility>
#include <vector>

struct PlayerBotEntry;
class WorldSession;
class PlayerBotAI;
class Creature;
class Item;

PlayerBotAI* CreatePlayerBotAI(std::string ainame);

// PORT-012 (KAP-558): the combat engagement request, source taxonomy,
// target validator, pursuit leash, target slots, capability model and
// cast-result vocabulary live in Companion/Combat.h (value-only); the
// executor re-resolves the target from the world each tick and never
// stores engine pointers.

class PlayerBotAI: public PlayerAI
{
    public:
        explicit PlayerBotAI(Player* pPlayer = nullptr) : PlayerAI(pPlayer), botEntry(nullptr), _wanderTimer(0), _combatCheckTimer(0), _abilityTimer(0) {}
        virtual ~PlayerBotAI() {}
        void Remove() override;

        virtual bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess);
        virtual void OnBotEntryLoad(PlayerBotEntry* entry) {}
        virtual void OnPacketReceived(WorldPacket const* /*packet*/) {} // server has sent a packet to this session
        virtual void SendFakePacket(uint16 /*opcode*/) {} // ai has scheduled delayed response to opcode
        virtual void UpdateAI(const uint32 /*diff*/) override; // Handle delayed teleports
        virtual void OnPlayerLogin();
        // TW-014 (KAP-557): owner-directed follow/stop goal. While active
        // the follow state machine preempts normal behavior; a goal whose
        // seq is at or below the current one is stale and never resumes.
        void FollowGoal(uint32 leaderGuid, uint32 seq);
        void FollowStop();
        void Hold(uint32 seq); // invalidate prior orders; persist until a new order
        // PORT-005 (KAP-558): owner-selected assist target. The manager
        // validated ownership, party membership and the target before
        // calling this; the AI re-validates at execution time. A newer
        // order (seq above the current one) re-arms the companion against
        // the named hostile and suspends the follow goal, which resumes
        // once the target is gone.
        void AssistTarget(uint64_t targetGuid, uint32 seq);
        virtual void OnLevelUp();
        virtual void BeforeAddToMap(Player* player) {} // me=nullptr at call
        // Helpers
        bool SpawnNewPlayer(WorldSession* sess, uint8 _class, uint32 _race, uint32 mapId, uint32 instanceId, float dx, float dy, float dz, float o);
        PlayerBotEntry* botEntry;
    protected:
        uint32 _wanderTimer;
        uint32 _combatCheckTimer;
        uint32 _abilityTimer;
        // Hardening item 4 / PORT-012: the live combat target and the
        // dead corpse pending loot are explicit slots with named transitions
        // (Companion::Combat::TargetSlots); owner orders never live here.
        Companion::Combat::TargetSlots _targets;
        uint8 _lootRetryCount = 0;
        uint32 _lootWindowMs = 0; // PORT-007: remaining (ms) of the bounded corpse-loot attempt; 0 = armed
        uint8 _inventoryPressure = 0; // PORT-024: bounded (<=8) full-bag loot events pending PORT-025 vendor handling
        uint64_t _vendorTargetGuid = 0; // PORT-025: resolved vendor within the declared radius (0 = none)
        uint32_t _vendorScanTimer = 0;  // PORT-025: re-scan pace accumulation (ms)
        uint32_t _vendorFailTimer = 0;  // PORT-025: no-vendor report pace (ms)
        bool _vendorFailReported = false; // PORT-025: first no-vendor report emitted
        bool _pressureReported = false;   // PORT-025: episode status line emitted
        bool _vendorExhausted = false;    // PORT-025: no sellable junk left this episode
        Companion::Combat::Leash _pursuitLeash; // PORT-008/012: pursuit reach budget
        bool _recoveryDead = false; // PORT-009: recovery state armed (dead with an active order)
        uint32 _recoveryReportMs = 0; // PORT-009: bounded report pace remaining (ms)
        uint32 _recoveryWalkMs = 0; // PORT-009: corpse walk re-issue window remaining (ms)
        bool _recoveryDeathAck = false; // PORT-009: dead-ack (BuildPlayerRepop) issued for the current death
        float _followPathX = 0.0f; // PORT-008: last issued follow path target (throttle)
        float _followPathY = 0.0f;
        float _followPathZ = 0.0f;
        uint32 _followPathAgeMs = 0; // PORT-008: age (ms) of the last issued follow path
        bool _obsAlive = true;
        uint32 _obsTimer = 0;
        // MVP-006: one declared supported quest progressed through the
        // normal quest APIs (accept, objective credit, turn-in). Phase:
        // 0 off, 1 seek giver (accept), 2 await objective, 3 seek
        // finisher (turn-in), 4 done.
        uint32 _questId = 0;
        uint8 _questPhase = 0;
        ObjectGuid _questGiverGuid;
        ObjectGuid _questObjectiveGuid;
        uint32 _questScanTimer = 0;
        uint32 _questDebugTimer = 0;
        uint8 _questDenyCount = 0;
        // PORT-023 (KAP-558): cooperative quest accept/turn-in
        // denial backoff (ms); a persistent denial re-checks the
        // world at a bounded pace instead of every tick.
        uint32 _coopQuestDenyTimer = 0;
        // TW-014 (KAP-557): active follow goal (leader guid + monotonic seq).
        bool _following = false;
        bool _held = false; // PORT-004: owner-directed hold; persists until new order
        uint64_t _assistTargetGuid = 0; // PORT-005: raw assist target guid (0 = none)
        uint64_t _defendTargetGuid = 0; // PORT-006: current defend candidate (0 = none)
        uint32 _defendProbeTimer = 0; // PORT-006: debug probe pacing (2000 ms)
        uint32 _defendTargetGrace = 0; // PORT-006: grace remaining (ms) for a locked defend target
        uint32 _damageWaitTimer = 0; // PORT-016: [Damage] wait-line pacing (2000 ms)
        uint32 _followSeq = 0;
        uint32 _followLeaderGuid = 0;
        uint32 _followGroupId = 0; // zero preserves legacy ungrouped follow
        bool _followReached = false;
        uint32 _followDebugTimer = 0;
        // PORT-018 (KAP-558): shared party planner round state (the
        // transport lives in PlayerBotMgr; disabled unless a service
        // URL is configured).
        uint32 _plannerLeaderGuid = 0;
        uint32 _plannerGroupId = 0;
        uint32 _plannerLastSubmitMs = 0;
        uint32 _plannerReqId = 1;
        Companion::Planner::Step _plannerOffer = {};
        bool _plannerOfferValid = false;
        // PORT-019 (KAP-558): bounded personality runtime state.
        // Effects of validated planner Preference steps only; owner
        // orders, hold, recovery and role policy always win over
        // them and Reset() returns the deterministic baseline.
        float _personalityChaseDist = kOwnerFollowChaseDist;
        uint32 _personalityLastExprMs = 0;
        uint8 _lastLevel = 0;
        bool TryLootDefeatedTarget();
        void RememberCombatTarget(Unit* unit); // Hardening item 4: remembers the live combat target
        bool CorpseLootStep(Creature* creature);
        void ExecuteLoot(Creature* corpse, uint32 diff);
        bool UpdateFollow(uint32 diff);
        bool PursuitLeashTick(Unit* target, uint32 diff); // PORT-008: one tick of the pursuit reach budget
        // Hardening item 3 / PORT-012 (KAP-558): shared combat executor
        // for the Assist, ContinueCombat and Defend intents (see
        // Companion::Combat::Request); the per-source debug lines are
        // the fixture contract.
        bool ExecuteCombat(Companion::Combat::Request const& req, uint32 diff);
        void LogAssistProbe(Creature* target); // PORT-009 diagnostic, assist path only
        bool UpdateRecovery(uint32 diff); // PORT-009: dead companion corpse reclaim
        bool UpdateCompanion(uint32 diff);
        void PlannerRoundStep(uint32 diff); // PORT-018: one shared planner round per party (record-only offers)
        void ConversationRoundStep(); // PORT-022: consume one bounded reply (world thread never waits)
        void CooperativeQuestStep(uint32 diff); // PORT-023: one owner-driven cooperative quest action (value policy + authoritative quest APIs)
        void ApplyPlannerPreference(uint32_t nowMs); // PORT-019: validated preference -> bounded effect
        bool IsFollowOwnerAvailable() const;
        // KAP-558 hardening: an owned companion without an active order
        // follows its owner instead of running the legacy auto-hunt
        // (autonomous acquisition + wander stay for ambient bots only).
        bool IsOwnedCompanion() const;
        // PORT-024 (KAP-558): equipment progression from loot the owned
        // companion legitimately received (value policy in
        // Companion/Equipment.h + authoritative inventory APIs).
        void EvaluateReceivedEquipment(std::vector<std::pair<uint32, uint32>> const& itemCounts);
        void EvaluateReceivedInstance(Item* item, uint8 bag, uint8 slot);
        // PORT-025 (KAP-558): bag-pressure report + bounded vendor
        // cleanup (value policy in Companion/Inventory.h; the sale
        // mirrors the vendor packet handler's guards and APIs).
        uint8_t CountFreeSlots() const;
        Companion::Inventory::ItemInfo FillItemInfo(Item* item) const;
        bool VendorPressureStep(uint32 diff);
        void ExecuteVendor(Companion::Intent const& intent, uint32 diff);
        void SellJunkToVendor(Creature* vendor);
        Player* FindOwnerByAccount() const;
        static constexpr float kOwnerFollowChaseDist = 25.0f;
        void ExecuteCompanion(Companion::Intent const& intent, uint32 diff);
        // PORT-006 (KAP-558): reactive defend (owner-enabled via
        // .botdefend). SelectDefendTarget scans for a creature actually
        // attacking the owner or this companion; the Defend intent runs
        // the engagement through the shared combat executor; the state
        // marker is diagnostic and is cleared when the owner is safe,
        // held, assisted, or the goal is withdrawn.
        Creature* SelectDefendTarget() const;
        void SetDefendTarget(uint64_t guid);
        void ClearDefendTarget(const char* reason);
        Creature* GetAliveHeldTarget() const;
        void ClearTarget();
        void InitQuestState();
        bool UpdateQuestPhases(uint32 diff);
        Creature* FindQuestGiver() const;
        Creature* FindQuestObjectiveTarget() const;
        void AutoLearnSpellsForLevel();
        uint32 SelectOffensiveSpell(Unit* target) const;
        // Hardening (KAP-558): cast-or-attack step; arms _abilityTimer only
        // on a successful cast (see TryOffensiveCastOrAttack).
        bool TryOffensiveCastOrAttack(Unit* target);
        // PORT-014 (KAP-558): declared tank threat policy (see
        // Companion/Tank.h): the pinned matrix gate, the per-evaluation
        // observation fill and the taunt step (a rejected taunt falls
        // back to the ordinary attack in the same evaluation).
        bool IsDeclaredTank() const;
        Companion::Tank::Observation FillTankObservation(Unit* target) const;
        void TankTauntStep(Unit* target);
        // PORT-015 (KAP-558): declared healer triage policy (see
        // Companion/Healer.h): the pinned matrix gate, the per-tick
        // triage observation filled from the live world, and the one
        // cast step with the PORT-012 cast outcome diagnostics.
        bool IsDeclaredHealer() const;
        Companion::Healer::Observation FillHealerObservation() const;
        void HealerTriageStep(Companion::Healer::Observation const& obs,
                              Companion::Healer::Decision const& decision);
        // PORT-016 (KAP-558): declared damage tank-pull policy (see
        // Companion/Damage.h): the pinned matrix gate, the per-tick
        // observation filled from the live world, the crowd-control
        // preservation set, and the established-target
        // revalidation for the shared executor.
        bool IsDeclaredDamage() const;
        Unit* FindDeclaredTank() const;
        bool TargetUnderCC(Unit* unit) const;
        Companion::Damage::Observation FillDamageObservation() const;
        bool IsEstablishedTankTarget(Creature* target) const;
        void AutoEquipForLevel();
        uint32 _gearMaxDiff = 9; // default similar to sample
        uint32 GetHighestKnownSpell(uint32 spellId) const;
        bool TargetHasAuraFromChain(Unit* target, uint32 spellId) const;
};

class PlayerCreatorAI: public PlayerBotAI
{
    public:
        explicit PlayerCreatorAI(Player* pPlayer, uint8 _race_, uint8 _class_, uint32 mapId, uint32 instanceId, float x, float y, float z, float o) :
            PlayerBotAI(pPlayer), _race(_race_), _class(_class_), _mapId(mapId), _instanceId(instanceId), _x(x), _y(y), _z(z), _o(o) { }
        virtual ~PlayerCreatorAI() {}
        bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override
        {
            return SpawnNewPlayer(sess, _class, _race, _mapId, _instanceId, _x, _y, _z, _o);
        }
    protected:
        uint8 _race;
        uint8 _class;
        uint32 _mapId;
        uint32 _instanceId;
        float _x;
        float _y;
        float _z;
        float _o;
};

class PlayerBotFleeingAI : public PlayerBotAI
{
    public:
        PlayerBotFleeingAI() : PlayerBotAI() {}
        void OnPlayerLogin() override;
};

class MageOrgrimmarAttackerAI: public PlayerBotAI
{
    public:
        explicit MageOrgrimmarAttackerAI(Player* pPlayer = nullptr) : PlayerBotAI(pPlayer) {}
        virtual ~MageOrgrimmarAttackerAI() {}
        bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override;
        void UpdateAI(const uint32 /*diff*/) override;
};

class PopulateAreaBotAI: public PlayerBotAI
{
    public:
        explicit PopulateAreaBotAI(uint32 map, float x, float y, float z, uint32 team, float radius, Player* pPlayer = nullptr) : PlayerBotAI(pPlayer), _map(map), _x(x), _y(y), _z(z), _radius(radius), _team(team) {}
        virtual ~PopulateAreaBotAI() {}
        void BeforeAddToMap(Player* player) override; // me=nullptr at call
        void OnPlayerLogin() override;
    protected:
        uint32 _map;
        float _x, _y, _z;
        float _radius;
        uint32 _team;
};
#endif
