#include "Common.h"
#include "Policies/SingletonImp.h"
#include "PlayerBotMgr.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"
#include "AccountMgr.h"
#include "Auth/BigNumber.h"
#include "Opcodes.h"
#include "Config/Config.h"
#include "Chat.h"
#include "Player.h"
#include "PlayerBotAI.h"
#include "Anticheat.h"

PlayerBotMgr sPlayerBotMgr;

PlayerBotMgr::PlayerBotMgr()
{
    totalChance = 0;
    _maxAccountId = 0;

    /* Config */
    confMinBots = 4;
    confMaxBots = 8;
    confBotsRefresh = 30000;
    confUpdateDiff = 10000;
    enable = false;
    confDebug = false;
    forceLogoutDelay = true;

    /* Time */
    m_elapsedTime = 0;
    m_lastBotsRefresh = 0;
    m_lastUpdate = 0;
}

PlayerBotMgr::~PlayerBotMgr()
{

}

void PlayerBotMgr::LoadConfig()
{
    enable = sConfig.GetBoolDefault("PlayerBot.Enable", false);
    confMinBots = sConfig.GetIntDefault("PlayerBot.MinBots", 3);
    confMaxBots = sConfig.GetIntDefault("PlayerBot.MaxBots", 10);
    confBotsRefresh = sConfig.GetIntDefault("PlayerBot.Refresh", 60000);
    confDebug = sConfig.GetBoolDefault("PlayerBot.Debug", false);
    confUpdateDiff = sConfig.GetIntDefault("PlayerBot.UpdateMs", 10000);
    forceLogoutDelay = sConfig.GetBoolDefault("PlayerBot.ForceLogoutDelay", true);
    confProvisionName = sConfig.GetStringDefault("PlayerBot.Provision", "");
    confTestLoginGuids = sConfig.GetStringDefault("PlayerBot.TestLogin", "");
    if (!forceLogoutDelay)
        m_tempBots.clear();
}

void PlayerBotMgr::Load()
{
    // 1- clean
    DeleteAll();
    m_bots.clear();
    m_tempBots.clear();
    totalChance = 0;

    // 2- Configuration
    LoadConfig();

    // 3- Load usable account ID
    QueryResult *result = LoginDatabase.PQuery("SELECT MAX(id) FROM account");
    if (!result)
    {
        sLog.outError("Playerbot: unable to load max account id.");
        return;
    }
    Field *fields = result->Fetch();
    _maxAccountId = fields[0].GetUInt32() + 10000;
    delete result;

    // 3b- Runtime provisioning (TW-010, contract C6 / section 5a): idempotent and
    // resumable; runs before the roster load so a newly provisioned bot is picked up.
    if (!confProvisionName.empty())
        ProvisionPersistentBot(confProvisionName);


    // 4- LoadFromDB with persisted ownership bindings (TW-006, contract C2/C6).
    // Roster rows without a valid bot_ownership binding are quarantined: logged and skipped.
    result = CharacterDatabase.PQuery(
        "SELECT p.char_guid, p.chance, p.ai, b.account_id, c.account "
        "FROM playerbot p "
        "LEFT JOIN bot_ownership b ON b.char_guid = p.char_guid "
        "LEFT JOIN characters c ON c.guid = p.char_guid");
    if (!result)
        sLog.outString("Loading playerbots...");
    else
    {
        do
        {
            fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint32 chance = fields[1].GetUInt32();
            bool hasBinding = !fields[3].IsNULL();
            uint32 boundAccount = hasBinding ? fields[3].GetUInt32() : 0;
            bool hasCharacter = !fields[4].IsNULL();
            uint32 charOwner = hasCharacter ? fields[4].GetUInt32() : 0;

            if (!hasCharacter)
            {
                sLog.outError("Playerbot: roster entry %u references a missing character; quarantined (skipped)", guid);
                continue;
            }
            if (!hasBinding || boundAccount != charOwner || charOwner < 1000000000)
            {
                sLog.outError("Playerbot: roster entry %u has no valid ownership binding (bound=%u owner=%u); quarantined (skipped)", guid, boundAccount, charOwner);
                continue;
            }

            PlayerBotEntry* entry = new PlayerBotEntry(guid, boundAccount, chance);
            entry->ai = CreatePlayerBotAI(fields[2].GetCppString());
            entry->ai->botEntry = entry;
            if (!sObjectMgr.GetPlayerNameByGUID(guid, entry->name))
                entry->name = "<Unknown>";
            entry->ai->OnBotEntryLoad(entry);
            entry->persistent = true;
            m_bots[entry->playerGUID] = entry;
            totalChance += chance;
        } while (result->NextRow());

        delete result;
        sLog.outString("%u bots charges", m_bots.size());
    }

    // 4b- R3 (review 2026-09-11) runtime probe: config-gated exercise of the
    // public temporary-login overload (never set in the personal server config).
    // Each listed guid is issued twice: the second call must be rejected as a
    // duplicate, and a guid without a character must fail cleanly.
    if (!confTestLoginGuids.empty())
    {
        size_t pos = 0;
        while (pos <= confTestLoginGuids.size())
        {
            size_t comma = confTestLoginGuids.find(',', pos);
            if (comma == std::string::npos)
                comma = confTestLoginGuids.size();
            std::string token = confTestLoginGuids.substr(pos, comma - pos);
            if (!token.empty())
            {
                uint32 probeGuid = 0;
                for (size_t k = 0; k < token.size(); ++k)
                {
                    char c = token[k];
                    if (c < '0' || c > '9')
                    {
                        probeGuid = 0;
                        break;
                    }
                    probeGuid = probeGuid * 10 + (uint32)(c - '0');
                }
                if (probeGuid != 0)
                {
                    bool first = AddBot(probeGuid, false);
                    bool second = AddBot(probeGuid, false);
                    sLog.outString("Playerbot: test-login %u first=%d second=%d (R3 probe)", probeGuid, (int)first, (int)second);
                }
            }
            pos = comma + 1;
        }
    }

    // 5- Check config/DB
    if (confMinBots >= m_bots.size() && !m_bots.empty())
        confMinBots = m_bots.size() - 1;
    if (confMaxBots > m_bots.size())
        confMaxBots = m_bots.size();
    if (confMaxBots <= confMinBots)
        confMaxBots = confMinBots + 1;

    // 6- Start initial bots
    if (enable)
    {
        for (uint32 i = 0; i < confMinBots; i++)
            AddRandomBot();
    }

    //7 - Remplir les stats
    m_stats.confMaxOnline = confMaxBots;
    m_stats.confMinOnline = confMinBots;
    m_stats.totalBots = m_bots.size();
    m_stats.confBotsRefresh = confBotsRefresh;
    m_stats.confUpdateDiff = confUpdateDiff;

    //8- Afficher les stats si débug
    if (confDebug)
    {
        sLog.outString("[PlayerBotMgr] Between %u and %u bots online", confMinBots, confMaxBots);
        sLog.outString("[PlayerBotMgr] %u now loading", m_stats.loadingCount);
    }
}

void PlayerBotMgr::DeleteAll()
{
    m_stats.onlineCount = 0;
    m_stats.loadingCount = 0;

    std::map<uint32, PlayerBotEntry*>::iterator i;
    for (i = m_bots.begin(); i != m_bots.end(); i++)
    {
        if (i->second->state != PB_STATE_OFFLINE)
        {
            OnBotLogout(i->second);
            totalChance += i->second->chance;
        }
    }

    m_tempBots.clear();

    if (confDebug)
        sLog.outString("[PlayerBotMgr] Deleting all bots [OK]");
}

void PlayerBotMgr::OnBotLogin(PlayerBotEntry *e)
{
    e->state = PB_STATE_ONLINE;
    e->loadingSinceMs = 0;
    if (confDebug)
        sLog.outString("[PlayerBot][Login]  '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
}

void PlayerBotMgr::OnBotLogout(PlayerBotEntry *e)
{
    e->state = PB_STATE_OFFLINE;
    // TW-009 (AC2): the session is being dropped; clear the identity so a
    // later login starts from a clean state.
    e->session = nullptr;
    e->loginQueued = false;
    if (confDebug)
    {
        sLog.outString("[PlayerBot][Logout] '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
    }
}

void PlayerBotMgr::OnPlayerInWorld(Player* player)
{
    WorldSession* sess = player->GetSession();
    if (!sess)
        return;

    PlayerBotEntry* e = sess->GetBot();
    if (!e)
        return;

    // TW-009 (AC2): only the entry's current session can complete its login.
    // A stale completion (an old session's load finishing after a timeout and
    // retry) is rejected; the current session is preserved.
    if (e->session != sess)
    {
        sLog.outError("Playerbot: stale in-world entry for %u (session mismatch); rejected", e->playerGUID);
        return;
    }
    if (e->state == PB_STATE_ONLINE)
    {
        // Re-entry after a map transfer: re-attachment is idempotent (setAI is
        // a plain assignment); no state or counter change.
        player->setAI(e->ai);
        return;
    }
    if (e->state != PB_STATE_LOADING)
    {
        sLog.outError("Playerbot: unexpected in-world entry for %u (state %d); rejected", e->playerGUID, (int)e->state);
        return;
    }

    // TW-009: online means successful player entry, not merely queued SQL.
    OnBotLogin(e);
    m_stats.loadingCount--;
    if (e->isChatBot)
        m_stats.onlineChat++;
    else
        m_stats.onlineCount++;

    player->setAI(e->ai);
    e->ai->SetPlayer(player);
    e->ai->OnPlayerLogin();
}

void PlayerBotMgr::Update(uint32 diff)
{
    // Bots temporaires
    std::map<uint32, uint32>::iterator it;
    for (it = m_tempBots.begin(); it != m_tempBots.end(); ++it)
    {
        if (it->second < diff)
            it->second = 0;
        else
            it->second -= diff;
    }

    it = m_tempBots.begin();
    while (it != m_tempBots.end())
    {
        if (!it->second)
        {
            // Update des "chatBot" aussi.
            for (std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.begin(); iter != m_bots.end(); ++iter)
                if (iter->second->accountId == it->first)
                {
                    iter->second->state = PB_STATE_OFFLINE; // Will get logged out at next WorldSession::Update call
                    m_bots.erase(iter);
                    break;
                }
            m_tempBots.erase(it);
            it = m_tempBots.begin();
        }
        else
            ++it;
    }

    m_elapsedTime += diff;
    if (!((m_elapsedTime - m_lastUpdate) > confUpdateDiff))
        return; //Pas besoin d'update

    m_lastUpdate = m_elapsedTime;

    /* Connection des bots en attente */
    std::map<uint32, PlayerBotEntry*>::iterator iter;
    for (iter = m_bots.begin(); iter != m_bots.end(); ++iter)
    {
        if (!enable && !iter->second->customBot)
            continue;
        if (iter->second->state != PB_STATE_LOADING)
            continue;

        // TW-006 (contract C7): bound the loading wait instead of waiting forever.
        if (iter->second->loadingSinceMs
            && WorldTimer::getMSTimeDiff(iter->second->loadingSinceMs, WorldTimer::getMSTime()) > confBotsRefresh * 2)
        {
            sLog.outError("Playerbot: session load timeout for %u (account %u); returning to offline",
                          iter->second->playerGUID, iter->second->accountId);
            iter->second->state = PB_STATE_OFFLINE;
            iter->second->loadingSinceMs = 0;
            // TW-009 (AC2): the in-flight session is stale from now on.
            iter->second->session = nullptr;
            iter->second->loginQueued = false;
            m_stats.loadingCount--;
            continue;
        }

        WorldSession* sess = sWorld.FindSession(iter->second->accountId);

        if (!sess)
        {
            // This may happen : just wait for the World to add the session.
            //sLog.outString("/!\\ PlayerBot in queue but Session not in World ... Account : %u, GUID : %u", iter->second->accountId, iter->second->playerGUID);
            continue;
        }

        // TW-009 (AC1): queue the login exactly once per session. The entry
        // stays LOADING until the player is actually in-world; OnPlayerInWorld
        // performs the single ONLINE transition with the counter updates.
        if (iter->second->loginQueued)
            continue;

        if (iter->second->ai->OnSessionLoaded(iter->second, sess))
        {
            iter->second->loginQueued = true;
            if (confDebug)
                sLog.outString("[PlayerBotMgr] login queued for %u (account %u); awaiting in-world entry",
                               iter->second->playerGUID, iter->second->accountId);
        }
        else
            sLog.outError("PLAYERBOT: Unable to load session id %u", iter->second->accountId);
    }

    if (!enable)
        return;

    uint32 updatesCount = (m_elapsedTime - m_lastBotsRefresh) / confBotsRefresh;
    for (uint32 i = 0; i < updatesCount; ++i)
    {
        AddOrRemoveBot();
        m_lastBotsRefresh += confBotsRefresh;
    }
}

/*
Toutes les X minutes, ajoute ou enleve un bot.
*/
bool PlayerBotMgr::AddOrRemoveBot()
{
    uint32 alea = urand(confMinBots, confMaxBots);
    /*
    10 --- --- --- --- --- --- --- --- --- --- 20 bots
                NumActuel
    [alea ici : remove    ][    ici, add    ]
    */
    if (alea > m_stats.onlineCount)
        return AddRandomBot();
    else
        return DeleteRandomBot();

}

bool PlayerBotMgr::AddBot(PlayerBotAI* ai)
{
    // Find a correct accountid ?
    PlayerBotEntry* e = new PlayerBotEntry();
    e->ai = ai;
    e->accountId = GenBotAccountId();
    e->playerGUID = sObjectMgr.GeneratePlayerLowGuid();
    e->customBot = true;
    ai->botEntry = e;
    m_bots[e->playerGUID] = e;
    // R3 (review 2026-09-11): report the delegated login result instead of
    // always claiming success.
    return AddBot(e->playerGUID, false);
}

bool PlayerBotMgr::AddBot(uint32 playerGUID, bool chatBot)
{
    uint32 accountId = 0;
    PlayerBotEntry *e = nullptr;
    std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.find(playerGUID);
    if (iter == m_bots.end())
        accountId = sObjectMgr.GetPlayerAccountIdByGUID(playerGUID);
    else
        accountId = iter->second->accountId;
    if (!accountId)
    {
        DETAIL_LOG("Compte du joueur %u introuvable ...", playerGUID);
        return false;
    }

    bool freshEntry = (iter == m_bots.end());
    if (iter != m_bots.end())
    {
        e = iter->second;
        // TW-006 (contract C2): never stack a second login on an active entry.
        // R3 (review 2026-09-11): pre-existing entries only; a freshly created
        // temporary entry has no prior state or session, so it cannot stack a
        // login on itself.
        if (e->state != PB_STATE_OFFLINE)
        {
            sLog.outError("Playerbot: duplicate login rejected for %u (session already active)", playerGUID);
            return false;
        }
    }
    else
    {
        DETAIL_LOG("Adding temporary PlayerBot.");
        e = new PlayerBotEntry();
        // R3 (review 2026-09-11): start OFFLINE; the entry flips to LOADING only
        // after every guard below passes, so a fresh entry is not rejected by
        // its own just-set state.
        e->state        = PB_STATE_OFFLINE;
        e->playerGUID   = playerGUID;
        e->chance       = 10;
        e->accountId    = accountId;
        e->isChatBot    = chatBot;
        e->ai           = new PlayerBotAI(nullptr);
        m_bots[playerGUID] = e;
    }


    // TW-006 (contract C2): session account must equal the stored character owner.
    // R3 (review 2026-09-11): custom bots are exempt - their character is created
    // later by the AI under this entry's synthetic account (the upstream
    // auto-testing contract), so no character row exists at queue time.
    if (!e->customBot)
    {
        QueryResult *ownerResult = CharacterDatabase.PQuery("SELECT account FROM characters WHERE guid = %u", playerGUID);
        uint32 charOwner = 0;
        if (ownerResult)
        {
            charOwner = ownerResult->Fetch()[0].GetUInt32();
            delete ownerResult;
        }
        if (!charOwner || charOwner != accountId)
        {
            sLog.outError("Playerbot: ownership mismatch for %u (session account %u, character owner %u); login rejected", playerGUID, accountId, charOwner);
            if (freshEntry)
            {
                m_bots.erase(playerGUID);
                delete e->ai;
                delete e;
            }
            return false;
        }
    }

    // TW-006 (contract C2): never replace an existing session for the same account.
    if (sWorld.FindSession(accountId))
    {
        sLog.outError("Playerbot: conflicting session for account %u (character %u); login rejected", accountId, playerGUID);
        if (freshEntry)
        {
            m_bots.erase(playerGUID);
            delete e->ai;
            delete e;
        }
        return false;
    }

    // TW-006 (contract C2): never log a bot in on top of a character already in the world.
    if (sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, playerGUID)))
    {
        sLog.outError("Playerbot: character %u is already in the world; login rejected", playerGUID);
        if (freshEntry)
        {
            m_bots.erase(playerGUID);
            delete e->ai;
            delete e;
        }
        return false;
    }

    e->state = PB_STATE_LOADING;
    e->loadingSinceMs = WorldTimer::getMSTime();
    WorldSession *session = new WorldSession(accountId, nullptr, sAccountMgr.GetSecurity(accountId), 0, LOCALE_enUS, "<BOT>", 0);
    // Bots skip the normal auth handshake; create a dummy anticheat session so hooks are valid.
    BigNumber dummyKey(0);
    session->InitAntiCheatSession(&dummyKey);
    session->SetBot(e);
    // TW-009 (AC2): stamp the session identity and generation so a stale async
    // completion (an old session's login finishing after a retry) is rejected.
    e->session = session;
    e->loginQueued = false;
    ++e->loginGeneration;
    sWorld.AddSession(session);
    m_stats.loadingCount++;

    if (chatBot)
        AddTempBot(accountId, 20000);

    return true;
}

bool PlayerBotMgr::AddRandomBot()
{
    uint32 alea = urand(0, totalChance);
    std::map<uint32, PlayerBotEntry*>::iterator it;
    bool done = false;
    for (it = m_bots.begin(); it != m_bots.end() && !done; it++)
    {
        if (it->second->state != PB_STATE_OFFLINE)
            continue;

        if (it->second->customBot)
            continue;

        uint32 chance = it->second->chance;

        if (chance >= alea)
        {
            AddBot(it->first);
            done = true;
        }

        alea -= chance;
    }

    return done;
}

void PlayerBotMgr::AddTempBot(uint32 account, uint32 time)
{
    m_tempBots[account] = time;
}

void PlayerBotMgr::RefreshTempBot(uint32 account)
{
    if (m_tempBots.find(account) != m_tempBots.end())
    {
        uint32& delay = m_tempBots[account];
        if (delay < 1000)
            delay = 1000;
    }
}

bool PlayerBotMgr::DeleteBot(uint32 playerGUID)
{
    std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.find(playerGUID);
    if (iter == m_bots.end())
        return false;

    if (iter->second->state == PB_STATE_LOADING)
        m_stats.loadingCount--;
    else if (iter->second->state == PB_STATE_ONLINE)
        m_stats.onlineCount--;

    OnBotLogout(iter->second);
    return true;
}

bool PlayerBotMgr::DeleteRandomBot()
{
    if (m_stats.onlineCount < 1)
        return false;

    uint32 idDelete = urand(0, m_stats.onlineCount);
    uint32 onlinePassed = 0;
    std::map<uint32, PlayerBotEntry*>::iterator iter;
    for (iter = m_bots.begin(); iter != m_bots.end(); iter++)
    {
        if (!iter->second->customBot && !iter->second->isChatBot && iter->second->state == PB_STATE_ONLINE)
        {
            onlinePassed++;
            if (onlinePassed == idDelete)
            {
                OnBotLogout(iter->second);
                m_stats.onlineCount--;
                return true;
            }
        }
    }

    return false;
}

bool PlayerBotMgr::ForceAccountConnection(WorldSession* sess)
{
    if (sess->GetBot())
        return sess->GetBot()->state != PB_STATE_OFFLINE;

    // Bots temporaires
    return m_tempBots.find(sess->GetAccountId()) != m_tempBots.end();
}

bool PlayerBotMgr::IsSaveableBot(PlayerBotEntry* e, uint32 sessionAccountId) const
{
    if (!e || !e->persistent)
        return false;
    return sessionAccountId == e->accountId;
}

bool PlayerBotMgr::IsPermanentBot(uint32 playerGUID)
{
    std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.find(playerGUID);
    return iter != m_bots.end();
}

bool PlayerBotMgr::IsChatBot(uint32 playerGuid)
{
    std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.find(playerGuid);
    return iter != m_bots.end() && iter->second->isChatBot;
}

void PlayerBotMgr::AddAllBots()
{
    std::map<uint32, PlayerBotEntry*>::iterator it;
    for (it = m_bots.begin(); it != m_bots.end(); it++)
    {
        if (!it->second->isChatBot && it->second->state == PB_STATE_OFFLINE)
            AddBot(it->first);
    }
}


// ---------------------------------------------------------------------------
// TW-010 (KAP-553): idempotent, resumable runtime provisioning of one
// persistent test bot (contract C6 / section 5a).
//
// Identity is the character NAME. A fresh provision allocates a reserved-range
// synthetic account (>= 1e9; no account row - C5) and a reserved-band character
// guid (>= 4e9), creates a legal level-10 Human Warrior (non-copied values),
// then persists the roster row and the bot_ownership binding (provision_version
// = 2). Because the character is keyed by name, a second run is a no-op when the
// bot is already complete, and an interrupted run (an orphan character left
// without roster/binding) is completed in place and never duplicated. Unrelated
// records are never touched.
// ---------------------------------------------------------------------------
uint32 PlayerBotMgr::AllocateReservedBotAccount()
{
    const uint32 kReservedBase = 1000000000u;
    uint32 maxReal = 0;
    QueryResult *realResult = LoginDatabase.PQuery("SELECT COALESCE(MAX(id), 0) FROM account");
    if (realResult)
    {
        maxReal = realResult->Fetch()[0].GetUInt32();
        delete realResult;
    }
    uint32 maxBot = 0;
    QueryResult *botResult = CharacterDatabase.PQuery("SELECT COALESCE(MAX(account_id), 0) FROM bot_ownership");
    if (botResult)
    {
        maxBot = botResult->Fetch()[0].GetUInt32();
        delete botResult;
    }
    // R1 (review 2026-09-11): reserved owners already present in characters are
    // excluded too - an orphan bot owns a reserved account before any binding
    // exists, and reusing it would merge two bots' session identities.
    uint32 maxOwner = 0;
    QueryResult *ownerResult = CharacterDatabase.PQuery("SELECT COALESCE(MAX(account), 0) FROM characters WHERE account >= 1000000000");
    if (ownerResult)
    {
        maxOwner = ownerResult->Fetch()[0].GetUInt32();
        delete ownerResult;
    }
    uint32 candidate = kReservedBase;
    if (maxReal >= candidate)
        candidate = maxReal + 1;
    if (maxBot >= candidate)
        candidate = maxBot + 1;
    if (maxOwner >= candidate)
        candidate = maxOwner + 1;
    return candidate;
}

uint32 PlayerBotMgr::AllocateReservedBotGuid()
{
    const uint32 kGuidBandBase = 4000000000u;
    uint32 maxGuid = 0;
    QueryResult *g = CharacterDatabase.PQuery("SELECT COALESCE(MAX(guid), 0) FROM characters WHERE guid >= 4000000000");
    if (g)
    {
        maxGuid = g->Fetch()[0].GetUInt32();
        delete g;
    }
    if (maxGuid < kGuidBandBase)
        return kGuidBandBase;
    return maxGuid + 1;
}

void PlayerBotMgr::ProvisionPersistentBot(const std::string& name)
{
    // Identity sanity: WoW 1.x character names are letters-only (2..12). Digits
    // would later be rejected by CheckPlayerName at login (the fixture bug fixed
    // for TW-006/TW-007), so provisioning enforces the same rule up front.
    if (name.size() < 2 || name.size() > 12)
    {
        sLog.outError("Playerbot provisioning: name '%s' has invalid length; not provisioned", name.c_str());
        return;
    }
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z'))
        {
            sLog.outError("Playerbot provisioning: name '%s' is not letters-only; not provisioned", name.c_str());
            return;
        }
    }

    // --- Resolve the character (idempotency / resumability key) ---
    uint32 guid = 0;
    uint32 account = 0;
    QueryResult *qr = CharacterDatabase.PQuery("SELECT guid, account FROM characters WHERE name = '%s' LIMIT 1", name.c_str());
    if (qr)
    {
        Field *f = qr->Fetch();
        if (f)
        {
            guid = f[0].GetUInt32();
            account = f[1].GetUInt32();
        }
        delete qr;
    }

    // C3: never adopt a character owned by a real (non-bot) account.
    if (guid != 0 && account < 1000000000u)
    {
        sLog.outError("Playerbot provisioning: name '%s' maps to character guid %u owned by a non-bot account %u; not adopted (C3)", name.c_str(), guid, account);
        return;
    }

    // --- R2 (review 2026-09-11): read all existing state before any write ---
    bool hasBinding = false;
    uint32 boundAccount = 0;
    uint32 provisionVersion = 0;
    QueryResult *bq = CharacterDatabase.PQuery("SELECT account_id, provision_version FROM bot_ownership WHERE char_guid = %u", guid);
    if (bq)
    {
        Field *f = bq->Fetch();
        if (f)
        {
            hasBinding = true;
            boundAccount = f[0].GetUInt32();
            provisionVersion = f[1].GetUInt32();
        }
        delete bq;
    }
    bool hasRoster = false;
    QueryResult *rq = CharacterDatabase.PQuery("SELECT COUNT(*) FROM playerbot WHERE char_guid = %u", guid);
    if (rq)
    {
        hasRoster = rq->Fetch()[0].GetUInt32() > 0;
        delete rq;
    }

    // Any existing binding is validated before any write, regardless of roster
    // state. A mismatched or unsupported binding is rejected without touching
    // character, binding, or roster data (fail closed).
    if (hasBinding)
    {
        if (boundAccount != account || account < 1000000000u || provisionVersion != 2)
        {
            sLog.outError("Playerbot provisioning: '%s' (guid %u) has an inconsistent binding (bound=%u owner=%u version=%u); rejected, nothing modified (fail closed)", name.c_str(), guid, boundAccount, account, provisionVersion);
            return;
        }
        if (hasRoster)
        {
            sLog.outString("Playerbot provisioning: '%s' (guid %u account %u) already provisioned; idempotent no-op", name.c_str(), guid, account);
            return;
        }
        // Consistent binding but a missing roster row: complete the roster only.
        if (!CharacterDatabase.PExecute("INSERT INTO playerbot (char_guid, chance, ai) VALUES (%u, 100, 'Default')", guid))
        {
            sLog.outError("Playerbot provisioning: roster completion failed for '%s' (guid %u); not provisioned", name.c_str(), guid);
            return;
        }
        sLog.outString("Playerbot provisioning: '%s' (guid %u account %u) completed (roster row added to existing binding)", name.c_str(), guid, account);
        return;
    }

    // --- No binding: fresh create (one transaction) or orphan completion ---
    if (guid == 0)
    {
        account = AllocateReservedBotAccount();
        guid = AllocateReservedBotGuid();
        if (!CharacterDatabase.BeginTransaction())
        {
            sLog.outError("Playerbot provisioning: cannot begin fresh creation for '%s'; not provisioned", name.c_str());
            return;
        }
        bool ok = CharacterDatabase.PExecute(
            "INSERT INTO characters (guid, account, name, race, class, gender, level, money, "
            "position_x, position_y, position_z, map, orientation, zone, health, "
            "power1, power2, power3, power4, power5) "
            "VALUES (%u, %u, '%s', 1, 1, 0, 10, 100000, -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0)",
            guid, account, name.c_str());
        ok = ok && CharacterDatabase.PExecute("INSERT INTO bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES (%u, %u, 1, 2)", guid, account);
        ok = ok && CharacterDatabase.PExecute("INSERT INTO playerbot (char_guid, chance, ai) VALUES (%u, 100, 'Default')", guid);
        if (!ok)
        {
            CharacterDatabase.RollbackTransaction();
            sLog.outError("Playerbot provisioning: fresh creation failed for '%s'; rolled back, nothing persisted", name.c_str());
            return;
        }
        // PExecute above only queues statements. Report success only after the
        // database executes them; provisioning needs a synchronous result.
        if (!CharacterDatabase.CommitTransactionDirect())
        {
            sLog.outError("Playerbot provisioning: fresh creation transaction failed for '%s'; not provisioned", name.c_str());
            return;
        }
        sLog.outString("Playerbot provisioning: created character '%s' (guid %u account %u); bound (provision_version=2)", name.c_str(), guid, account);
        return;
    }

    // Orphan resume: the character exists on a reserved account without a
    // binding; complete it in place (same guid/account, never duplicated).
    if (!CharacterDatabase.BeginTransaction())
    {
        sLog.outError("Playerbot provisioning: cannot begin orphan completion for '%s'; not provisioned", name.c_str());
        return;
    }
    bool ok = CharacterDatabase.PExecute("INSERT INTO bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES (%u, %u, 1, 2)", guid, account);
    if (!hasRoster)
        ok = ok && CharacterDatabase.PExecute("INSERT INTO playerbot (char_guid, chance, ai) VALUES (%u, 100, 'Default')", guid);
    if (!ok)
    {
        CharacterDatabase.RollbackTransaction();
        sLog.outError("Playerbot provisioning: orphan completion failed for '%s' (guid %u); rolled back, nothing persisted", name.c_str(), guid);
        return;
    }
    if (!CharacterDatabase.CommitTransactionDirect())
    {
        sLog.outError("Playerbot provisioning: orphan completion transaction failed for '%s' (guid %u); not provisioned", name.c_str(), guid);
        return;
    }
    sLog.outString("Playerbot provisioning: '%s' (guid %u account %u) completed (existing character); bound (provision_version=2)", name.c_str(), guid, account);
}
