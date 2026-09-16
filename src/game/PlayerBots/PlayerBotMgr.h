#ifndef _PLAYERBOTMGR_H
#define _PLAYERBOTMGR_H

#include "Common.h"
#include "Policies/Singleton.h"
#include "Database/DatabaseEnv.h"
#include "Companion/PlannerTransport.h"

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
    uint32 followSeq; // TW-014: monotonic follow-goal sequence (0 = no goal yet)
    uint32 ownerAccountId; // TW-014: human account allowed to command this bot (0 = unowned)
    bool defendEnabled; // PORT-006: owner-enabled reactive defend (session-scoped)
    uint32 partySeq; // CMP-010: invalidates pending recruit/recall work
    uint32 pendingPartySeq; // sequence captured by an asynchronous recall
    uint32 pendingPartyLeaderGuid; // human leader to revalidate after login

    PlayerBotEntry(uint64 guid, uint32 account, uint32 _chance): playerGUID(guid), accountId(account), chance(_chance), state(PB_STATE_OFFLINE), isChatBot(false), customBot(false), ai(nullptr), loadingSinceMs(0), persistent(false), session(nullptr), loginQueued(false), loginGeneration(0), followSeq(0), ownerAccountId(0), defendEnabled(false), partySeq(0), pendingPartySeq(0), pendingPartyLeaderGuid(0)
    {}
    PlayerBotEntry(): playerGUID(0), accountId(0), chance(100.0f), state(PB_STATE_OFFLINE), isChatBot(false), customBot(false), ai(nullptr), loadingSinceMs(0), persistent(false), session(nullptr), loginQueued(false), loginGeneration(0), followSeq(0), ownerAccountId(0), defendEnabled(false), partySeq(0), pendingPartySeq(0), pendingPartyLeaderGuid(0)
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
        bool IsDebugEnabled() const { return confDebug; }
        float GetWanderRadius() const { return confWanderRadius; }
        uint32 GetQuestId() const { return confQuestId; }
        bool ForceLogoutDelay() const { return forceLogoutDelay; }

        // PORT-018 (KAP-558): the bounded nonblocking party-planner
        // transport (disabled when PlayerBot.PlannerServiceURL is empty).
        Companion::Planner::PlannerTransport& PlannerTransport() { return m_plannerTransport; }
        // PORT-018 (KAP-558): live bot lookup by low GUID (world thread,
        // no allocation).
        PlayerBotEntry* FindBotByGuid(uint32 guid) const;

        // TW-007 (contract C4): only verified persistent (roster) bots may save,
        // and only through a session that uses their approved bound identity.
        bool IsSaveableBot(PlayerBotEntry* e, uint32 sessionAccountId) const;

        // TW-014 (KAP-557): owner-only follow/stop for one owned companion.
        // Ownership is the bot_ownership binding (the entry accountId);
        // every outcome, accepted or rejected, is logged.
        bool BotFollow(Player* issuer, const std::string& botName);
        bool BotStop(Player* issuer, const std::string& botName);
        bool BotHold(Player* issuer, const std::string& botName); // PORT-004
        // PORT-005 (KAP-558): owner-selected assist. The companion must be
        // in the issuer's party; the target must be a legal hostile
        // creature in the companion's vicinity (never a player or friendly).
        // Every outcome is logged; the AI re-validates target and order
        // generation at execution time.
        bool BotAssist(Player* issuer, const std::string& botName, const std::string& targetName);
        bool BotDefend(Player* issuer, const std::string& botName, bool enable); // PORT-006

        // NEXT-002 (post-MVP): deterministic party-invite handling for
        // socketless companion sessions. A bot session never answers the
        // queued SMSG_GROUP_INVITE, so the invite would stick forever and
        // block every later invite ("already in a group"). Called from
        // WorldSession::HandleGroupInviteOpcode right after the invite
        // packet is queued: an owned companion accepts an invite from its
        // owner (mirroring the client accept path) and declines every
        // other invite (mirroring the decline path, including
        // SMSG_GROUP_DECLINE to the inviter). Non-roster players are
        // unaffected. Every outcome is logged.
        void HandlePartyInvite(Player* issuer, Player* invitee);
        // CMP-010: normal Group membership for an owned companion. Recruit
        // requires an online bot; recall may queue its login. Dismiss keeps
        // durable ownership and invalidates pending recall work.
        bool BotRecruit(Player* issuer, const std::string& botName);
        bool BotDismiss(Player* issuer, const std::string& botName);
        bool BotRecall(Player* issuer, const std::string& botName);

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
        Companion::Planner::PlannerTransport m_plannerTransport;
        std::map<uint32 /*account*/, uint32> m_tempBots;
        PlayerBotStats m_stats;

        uint32 confMinBots;
        uint32 confMaxBots;
        uint32 confBotsRefresh;
        uint32 confUpdateDiff;
        bool confDebug;
        std::string confProvisionName; // TW-010: stable identity (name) provisioned at load
        std::string confTestLoginGuids; // R3 probe: comma-separated guids temp-logged-in at load (lab only)
        uint32 confQuestId; // MVP-006: one declared supported quest (0 = disabled)
        float confWanderRadius; // PORT-007 lab: 0 = legacy frand(8,20); >0 = max idle-wander radius (yd)
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
        // PORT-008 (KAP-558) lab-only owner logout/relogin probe (default
        // off): at logoutMs after the probed bot's first ONLINE state the
        // session is deleted (DeleteBot, the normal logout path); at
        // reloginMs it is queued back (AddBot). Offsets in ms from that
        // baseline, so fixtures reason in the follow-script clock.
        void UpdateTestLogoutScript();
        uint32 m_logoutProbeGuid;
        uint32 m_logoutProbeLogoutMs;
        uint32 m_logoutProbeReloginMs;
        uint32 m_logoutProbeLoginMs;
        int m_logoutProbeStage;

        // TW-014 (KAP-557) lab-only deterministic follow/stop script, armed
        // from PlayerBot.FollowScript (default empty = disabled). Event
        // formats (semicolon-separated): chat
        // <delayMs>:<issuerGuid>:<command text without leading dot>; stale
        // <delayMs>:stale:<botName>:<leaderGuid>:<seq>. The clock starts
        // when every chat-driven issuer is online; stale events deliver a
        // directly expired follow goal to prove the seq guard rejects it.
        struct FollowScriptEvent
        {
            uint32 delayMs;
            uint32 issuerGuid; // chat events: issuer player guid
            uint32 leaderGuid; // stale events: leader named by the expired goal
            uint32 seq;        // stale events: seq carried by the expired goal
            bool stale;
            std::string text;  // chat: command text (no dot); stale: bot name
        };
        void UpdateFollowScript();
        PlayerBotEntry* FindBotByName(const std::string& name) const;
        bool CompletePartyRecruit(Player* issuer, PlayerBotEntry* entry, uint32 sequence, Player* knownBot = nullptr);
        bool ValidatePartyOwner(Player* issuer, PlayerBotEntry* entry, const char* action) const;
        std::vector<FollowScriptEvent> m_followScript;
        uint32 m_followScriptStartMs;
        size_t m_followScriptIdx;

        // NEXT-002 (post-MVP) lab-only deterministic party-invite script,
        // armed from PlayerBot.PartyInviteScript (default empty = disabled).
        // Events are semicolon-separated
        // <delayMs>:<inviterGuid>:<inviteeName>; the clock starts when
        // every inviter is online and each delivery sends a real
        // CMSG_GROUP_INVITE through the inviter's session, so the full
        // invite setup and the HandlePartyInvite settlement both run the
        // normal path.
        struct PartyInviteScriptEvent
        {
            uint32 delayMs;
            uint32 inviterGuid;
            std::string inviteeName;
        };
        void UpdatePartyInviteScript();
        std::vector<PartyInviteScriptEvent> m_partyInviteScript;
        uint32 m_partyInviteScriptStartMs;
        size_t m_partyInviteScriptIdx;

        bool enable;
        uint32 AllocateReservedBotAccount(); // TW-010: fresh id in reserved range (>= 1e9)
};

// MVP-002 (KAP-552) lab-only hook (defined in CharacterHandler.cpp): queues a
// synthetic login completion stamped with a stale generation so the guard in
// CharacterHandler::HandlePlayerLoginCallback can be exercised deterministically.
void TestDeliverStaleBotLoginCompletion(uint32 accountId, uint32 guid, uint32 staleGeneration);

extern PlayerBotMgr sPlayerBotMgr;

#endif
