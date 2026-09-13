#ifndef MANGOS_PLAYERBOTAI_H
#define MANGOS_PLAYERBOTAI_H

#include "PlayerAI.h"
#include "WorldSession.h"

struct PlayerBotEntry;
class WorldSession;
class PlayerBotAI;
class Creature;

PlayerBotAI* CreatePlayerBotAI(std::string ainame);

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
        virtual void OnLevelUp();
        virtual void BeforeAddToMap(Player* player) {} // me=nullptr at call
        // Helpers
        bool SpawnNewPlayer(WorldSession* sess, uint8 _class, uint32 _race, uint32 mapId, uint32 instanceId, float dx, float dy, float dz, float o);
        PlayerBotEntry* botEntry;
    protected:
        uint32 _wanderTimer;
        uint32 _combatCheckTimer;
        uint32 _abilityTimer;
        ObjectGuid _lootTargetGuid;
        uint8 _lootRetryCount = 0;
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
        // TW-014 (KAP-557): active follow goal (leader guid + monotonic seq).
        bool _following = false;
        uint32 _followSeq = 0;
        uint32 _followLeaderGuid = 0;
        bool _followReached = false;
        uint32 _followDebugTimer = 0;
        uint8 _lastLevel = 0;
        bool TryLootDefeatedTarget();
        void RememberLootTarget(Unit* unit);
        bool UpdateFollow(uint32 diff);
        Creature* GetAliveHeldTarget() const;
        void ClearTarget();
        void InitQuestState();
        bool UpdateQuestPhases(uint32 diff);
        Creature* FindQuestGiver() const;
        Creature* FindQuestObjectiveTarget() const;
        void AutoLearnSpellsForLevel();
        uint32 SelectOffensiveSpell(Unit* target) const;
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
