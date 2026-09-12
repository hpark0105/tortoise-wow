#ifndef _PLAYERBOTMGR_H
#define _PLAYERBOTMGR_H

#include "Common.h"
#include "Policies/Singleton.h"
#include "Database/DatabaseEnv.h"

#include <vector>

class PlayerBotAI;
class WorldSession;
class Player;

enum PlayerBotState
{
    PB_STATE_OFFLINE,
    PB_STATE_LOADING,
    PB_STATE_ONLINE
};

struct PlayerBotEntry
{
    uint64 playerGUID;
    std::string name;
    uint32 accountId;

    uint32 chance;
    uint8 state; //Online, in queue or offline
    bool isChatBot; // bot des joueurs en discussion via le site.
    bool customBot; // Enabled even if PlayerBot system disabled (AutoTesting system for example)
    PlayerBotAI* ai;
    uint32 loadingSinceMs; // WorldTimer ms when PB_STATE_LOADING started (0 = not loading)
    bool persistent; // true only for verified roster entries (TW-007, contract C4)
    WorldSession* session; // current login session (TW-009, AC2); null when not logging in
    bool loginQueued; // login queued for the current session (TW-009, AC1)
    uint32 loginGeneration; // increments on every session creation (TW-009, AC2)

    PlayerBotEntry(uint64 guid, uint32 account, uint32 _chance): playerGUID(guid), accountId(account), chance(_chance), state(PB_STATE_OFFLINE), isChatBot(false), customBot(false), ai(nullptr), loadingSinceMs(0), persistent(false), session(nullptr), loginQueued(false), loginGeneration(0)
    {}
    PlayerBotEntry(): playerGUID(0), accountId(0), chance(100.0f), state(PB_STATE_OFFLINE), isChatBot(false), customBot(false), ai(nullptr), loadingSinceMs(0), persistent(false), session(nullptr), loginQueued(false), loginGeneration(0)
    {}
};

struct PlayerBotStats
{
    /* Stats */
    uint32 onlineCount;
    uint32 loadingCount;
    uint32 totalBots;
    uint32 onlineChat;

    /* Config */
    uint32 confMaxOnline;
    uint32 confMinOnline;
    uint32 confBotsRefresh;
    uint32 confUpdateDiff;

    PlayerBotStats() 
    : onlineCount(0), loadingCount(0), totalBots(0), onlineChat(0),
    confMaxOnline(0), confMinOnline(0), confBotsRefresh(0), confUpdateDiff(0) {}
};


class PlayerBotMgr
{
    public:
        PlayerBotMgr();
        ~PlayerBotMgr();

        void LoadConfig();
        void Load();
        // TW-010 (contract C6 / section 5a): idempotent, resumable runtime
        // provisioning of one persistent test bot, keyed on its stable identity
        // (character name). Safe to run more than once; completes an interrupted
        // run without touching unrelated records.
        void ProvisionPersistentBot(const std::string& name);

        void Update(uint32 diff);
        bool AddOrRemoveBot();

        bool AddBot(PlayerBotAI* ai);
        bool AddBot(uint32 playerGuid, bool chatBot=false);
        bool DeleteBot(std::map<uint32, PlayerBotEntry*>::iterator iter);
        bool DeleteBot(uint32 playerGuid);

        bool AddRandomBot();
        bool DeleteRandomBot();

        void DeleteAll();
        void AddAllBots();

        void OnBotLogout(PlayerBotEntry *e);
        void OnBotLogin(PlayerBotEntry *e);
        void OnPlayerInWorld(Player* pPlayer);
        void AddTempBot(uint32 account, uint32 time);
        void RefreshTempBot(uint32 account);

        bool ForceAccountConnection(WorldSession* sess);
        bool IsPermanentBot(uint32 playerGuid);
        bool IsChatBot(uint32 playerGuid);
        bool ForceLogoutDelay() const { return forceLogoutDelay; }

        // TW-007 (contract C4): only verified persistent (roster) bots may save,
        // and only through a session that uses their approved bound identity.
        bool IsSaveableBot(PlayerBotEntry* e, uint32 sessionAccountId) const;

        uint32 GenBotAccountId() { return ++_maxAccountId; }
        PlayerBotStats& GetStats(){ return m_stats; }
        void Start() { enable = true; }
    protected:
        /* Combien de temps depuis la derniere MaJ ?*/
        uint32 m_elapsedTime;
        uint32 m_lastBotsRefresh;
        uint32 m_lastUpdate;
        uint32 totalChance;
        uint32 _maxAccountId;

        std::map<uint32 /*pl guid*/, PlayerBotEntry*> m_bots;
        std::map<uint32 /*account*/, uint32> m_tempBots;
        PlayerBotStats m_stats;

        uint32 confMinBots;
        uint32 confMaxBots;
        uint32 confBotsRefresh;
        uint32 confUpdateDiff;
        bool confDebug;
        std::string confProvisionName; // TW-010: stable identity (name) provisioned at load
        std::string confTestLoginGuids; // R3 probe: comma-separated guids temp-logged-in at load (lab only)
        bool forceLogoutDelay;

        // MVP-002 (KAP-552) lab-only stale-completion probe, armed from
        // PlayerBot.TestStaleLogin (default empty = disabled). Logs the probe
        // bot out (generation N), re-logs it in (generation N+1), then delivers
        // a synthetic generation-N completion. Stages: 0 wait gen-N online,
        // 1 wait old session dropped, 2 wait gen-(N+1) online, 3 delivered.
        void UpdateStaleLoginProbe();
        uint32 m_staleProbeGuid;
        int m_staleProbeStage;
        uint32 m_staleProbeOldGen;

        bool enable;
        uint32 AllocateReservedBotAccount(); // TW-010: fresh id in reserved range (>= 1e9)
};

// MVP-002 (KAP-552) lab-only hook (defined in CharacterHandler.cpp): queues a
// synthetic login completion stamped with a stale generation so the guard in
// CharacterHandler::HandlePlayerLoginCallback can be exercised deterministically.
void TestDeliverStaleBotLoginCompletion(uint32 accountId, uint32 guid, uint32 staleGeneration);

extern PlayerBotMgr sPlayerBotMgr;

#endif
