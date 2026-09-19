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
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Group.h"
#include "Player.h"
#include "Group.h"
#include "MasterPlayer.h"
#include "PlayerBotAI.h"
#include "Anticheat.h"
#include <cctype>
#include <cstdlib>

namespace
{
struct BotIdentitySpec
{
    std::string name;
    uint8 race = RACE_HUMAN;
    uint8 playerClass = CLASS_WARRIOR;
    uint8 gender = GENDER_MALE;
    uint8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
};

bool ParseBotIdentitySpec(std::string const& value, BotIdentitySpec& spec)
{
    std::vector<std::string> fields;
    size_t pos = 0;
    while (pos <= value.size())
    {
        size_t comma = value.find(',', pos);
        if (comma == std::string::npos)
            comma = value.size();
        fields.push_back(value.substr(pos, comma - pos));
        pos = comma + 1;
    }
    if (fields.size() != 1 && fields.size() != 9)
        return false;
    spec.name = fields[0];
    if (fields.size() == 1)
        return true;
    uint8* outputs[] = {&spec.race, &spec.playerClass, &spec.gender, &spec.skin,
                        &spec.face, &spec.hairStyle, &spec.hairColor, &spec.facialHair};
    for (size_t i = 1; i < fields.size(); ++i)
    {
        if (fields[i].empty())
            return false;
        for (size_t k = 0; k < fields[i].size(); ++k)
            if (fields[i][k] < '0' || fields[i][k] > '9')
                return false;
        unsigned long parsed = strtoul(fields[i].c_str(), nullptr, 10);
        if (parsed > 255)
            return false;
        *outputs[i - 1] = (uint8)parsed;
    }
    return true;
}
}

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
    confQuestId = 0;
    forceLogoutDelay = true;

    /* Time */
    m_elapsedTime = 0;
    m_lastBotsRefresh = 0;
    m_lastUpdate = 0;
    m_staleProbeGuid = 0;
    m_staleProbeStage = 0;
    m_staleProbeOldGen = 0;
    m_logoutProbeGuid = 0;
    m_logoutProbeLogoutMs = 0;
    m_logoutProbeReloginMs = 0;
    m_logoutProbeLoginMs = 0;
    m_logoutProbeStage = 0;
    m_followScriptStartMs = 0;
    m_followScriptIdx = 0;
    m_partyInviteScriptStartMs = 0;
    m_partyInviteScriptIdx = 0;
}

PlayerBotMgr::~PlayerBotMgr()
{
    // PORT-018 (KAP-558): bounded shutdown; the worker join cannot outlive
    // the per-round I/O deadline.
    m_plannerTransport.Shutdown();
    // PORT-022 (KAP-558): same bounded shutdown for conversation.
    m_conversationTransport.Shutdown();
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
    // MVP-006: one declared quest the companion progresses through the
    // normal quest APIs (accept, objective credit, turn-in).
    confQuestId = (uint32)sConfig.GetIntDefault("PlayerBot.QuestId", 0);
    confCooperativeQuestId = (uint32)sConfig.GetIntDefault("PlayerBot.CooperativeQuestId", 0);
    // PORT-030 (KAP-558): dynamic mirror mode - no per-quest config;
    // the companion mirrors any kill-only quest the owner holds.
    confMirrorOwnerQuests = sConfig.GetBoolDefault("PlayerBot.MirrorOwnerQuests", false);
    // PORT-007 (KAP-558): lab-only idle-wander clamp. Default 0 keeps
    // the legacy frand(8,20) radius; fixtures set a small value so
    // seeded bots stay geometrically stable before scripted holds land.
    confWanderRadius = sConfig.GetFloatDefault("PlayerBot.WanderRadius", 0.0f);
    // PORT-023 (KAP-558) lab-only gate: ambient (unowned) bots may
    // autonomously acquire nearby targets (default on; the lab
    // owner fixture disables it so a temp-logged owner idles at
    // spawn instead of hunting the fixture pack).
    confAmbientAcquire = sConfig.GetBoolDefault("PlayerBot.AmbientAcquire", true);
    if (confQuestId)
        sLog.outString("Playerbot: declared quest %u enabled (MVP-006)", confQuestId);
    // TW-014 (KAP-557) lab-only deterministic follow/stop script (default
    // off). Events are relative to the moment every chat-driven issuer is
    // online; see the FollowScriptEvent doc in PlayerBotMgr.h for formats.
    m_followScript.clear();
    m_followScriptStartMs = 0;
    m_followScriptIdx = 0;
    {
        std::string scriptToken = sConfig.GetStringDefault("PlayerBot.FollowScript", "");
        size_t pos = 0;
        while (pos <= scriptToken.size())
        {
            size_t semi = scriptToken.find(';', pos);
            if (semi == std::string::npos)
                semi = scriptToken.size();
            std::string ev = scriptToken.substr(pos, semi - pos);
            pos = semi + 1;
            if (ev.empty())
                continue;
            size_t c1 = ev.find(':');
            if (c1 == std::string::npos)
            {
                sLog.outError("Playerbot: follow-script event malformed; skipped: %s", ev.c_str());
                continue;
            }
            uint32 delay = (uint32)atoi(ev.substr(0, c1).c_str());
            std::string rest = ev.substr(c1 + 1);
            FollowScriptEvent fe;
            fe.delayMs = delay;
            fe.issuerGuid = 0;
            fe.leaderGuid = 0;
            fe.seq = 0;
            if (rest.compare(0, 6, "stale:") == 0)
            {
                size_t c2 = rest.find(':', 6);
                size_t c3 = (c2 == std::string::npos) ? std::string::npos : rest.find(':', c2 + 1);
                if (c2 == std::string::npos || c3 == std::string::npos)
                {
                    sLog.outError("Playerbot: follow-script stale event malformed; skipped: %s", ev.c_str());
                    continue;
                }
                fe.stale = true;
                fe.text = rest.substr(6, c2 - 6);
                fe.leaderGuid = (uint32)atoi(rest.substr(c2 + 1, c3 - c2 - 1).c_str());
                fe.seq = (uint32)atoi(rest.substr(c3 + 1).c_str());
                if (fe.text.empty() || fe.leaderGuid == 0)
                {
                    sLog.outError("Playerbot: follow-script stale event incomplete; skipped: %s", ev.c_str());
                    continue;
                }
            }
            else
            {
                size_t c2 = rest.find(':');
                if (c2 == std::string::npos)
                {
                    sLog.outError("Playerbot: follow-script chat event malformed; skipped: %s", ev.c_str());
                    continue;
                }
                fe.stale = false;
                fe.issuerGuid = (uint32)atoi(rest.substr(0, c2).c_str());
                fe.text = rest.substr(c2 + 1);
                if (fe.issuerGuid == 0 || fe.text.empty())
                {
                    sLog.outError("Playerbot: follow-script chat event incomplete; skipped: %s", ev.c_str());
                    continue;
                }
            }
            m_followScript.push_back(fe);
        }
        if (!m_followScript.empty() && confDebug)
            sLog.outString("[PlayerBot][FollowScript] armed events:%u (TW-014 lab script)", (uint32)m_followScript.size());
    }
    // PORT-023 (KAP-558) lab-only deterministic owner quest script
    // (default off). Events are semicolon-separated
    // <delayMs>:<issuerGuid>:<questId>:<phase> with phase "accept"
    // or "turnin"; see QuestScriptEvent in PlayerBotMgr.h.
    m_questScript.clear();
    m_questScriptStartMs = 0;
    m_questScriptIdx = 0;
    {
        std::string scriptToken = sConfig.GetStringDefault("PlayerBot.QuestScript", "");
        size_t pos = 0;
        while (pos <= scriptToken.size())
        {
            size_t semi = scriptToken.find(';', pos);
            if (semi == std::string::npos)
                semi = scriptToken.size();
            std::string ev = scriptToken.substr(pos, semi - pos);
            pos = semi + 1;
            if (ev.empty())
                continue;
            size_t c1 = ev.find(':');
            size_t c2 = (c1 == std::string::npos) ? std::string::npos : ev.find(':', c1 + 1);
            size_t c3 = (c2 == std::string::npos) ? std::string::npos : ev.find(':', c2 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos)
            {
                sLog.outError("Playerbot: quest-script event malformed; skipped: %s", ev.c_str());
                continue;
            }
            QuestScriptEvent qe;
            qe.delayMs = (uint32)atoi(ev.substr(0, c1).c_str());
            qe.issuerGuid = (uint32)atoi(ev.substr(c1 + 1, c2 - c1 - 1).c_str());
            qe.questId = (uint32)atoi(ev.substr(c2 + 1, c3 - c2 - 1).c_str());
            std::string phase = ev.substr(c3 + 1);
            qe.turnin = (phase == "turnin");
            if (!qe.turnin && phase != "accept")
            {
                sLog.outError("Playerbot: quest-script phase invalid; skipped: %s", ev.c_str());
                continue;
            }
            if (qe.issuerGuid == 0 || qe.questId == 0)
            {
                sLog.outError("Playerbot: quest-script event incomplete; skipped: %s", ev.c_str());
                continue;
            }
            m_questScript.push_back(qe);
        }
        if (!m_questScript.empty() && confDebug)
            sLog.outString("[PlayerBot][QuestScript] armed events:%u (PORT-023 lab script)", (uint32)m_questScript.size());
    }
    // NEXT-002 (post-MVP) lab-only deterministic party-invite script
    // (default off). Events are semicolon-separated
    // <delayMs>:<inviterGuid>:<inviteeName>; the clock starts when every
    // inviter is online. Each event delivers a real CMSG_GROUP_INVITE
    // through the inviter's session so the full invite path runs.
    m_partyInviteScript.clear();
    m_partyInviteScriptStartMs = 0;
    m_partyInviteScriptIdx = 0;
    {
        std::string scriptToken = sConfig.GetStringDefault("PlayerBot.PartyInviteScript", "");
        size_t pos = 0;
        while (pos <= scriptToken.size())
        {
            size_t semi = scriptToken.find(';', pos);
            if (semi == std::string::npos)
                semi = scriptToken.size();
            std::string ev = scriptToken.substr(pos, semi - pos);
            pos = semi + 1;
            if (ev.empty())
                continue;
            size_t c1 = ev.find(':');
            size_t c2 = (c1 == std::string::npos) ? std::string::npos : ev.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos)
            {
                sLog.outError("Playerbot: party-invite-script event malformed; skipped: %s", ev.c_str());
                continue;
            }
            PartyInviteScriptEvent pe;
            pe.delayMs = (uint32)atoi(ev.substr(0, c1).c_str());
            pe.inviterGuid = (uint32)atoi(ev.substr(c1 + 1, c2 - c1 - 1).c_str());
            pe.inviteeName = ev.substr(c2 + 1);
            if (pe.inviterGuid == 0 || pe.inviteeName.empty())
            {
                sLog.outError("Playerbot: party-invite-script event incomplete; skipped: %s", ev.c_str());
                continue;
            }
            m_partyInviteScript.push_back(pe);
        }
        if (!m_partyInviteScript.empty() && confDebug)
            sLog.outString("[PlayerBot][PartyInviteScript] armed events:%u (NEXT-002 lab script)", (uint32)m_partyInviteScript.size());
    }
    // PORT-018 (KAP-558): bounded nonblocking planner transport. An empty
    // PlayerBot.PlannerServiceURL leaves it disabled: no thread, no I/O, every
    // call a no-op (the deterministic regression path).
    m_plannerTransport.Init(sConfig.GetStringDefault("PlayerBot.PlannerServiceURL", ""),
                            WorldTimer::getMSTime(), confDebug);
    // PORT-022 (KAP-558): bounded nonblocking companion-conversation
    // transport. An empty PlayerBot.ConversationServiceURL leaves it
    // disabled: no thread, no I/O, no reply (the deterministic path).
    m_conversationTransport.Init(sConfig.GetStringDefault("PlayerBot.ConversationServiceURL", ""),
                                 WorldTimer::getMSTime(), confDebug);
    // PORT-019 (KAP-558): declared personality profile for owned
    // companions. The profile is operator-declared (config now,
    // per-character persistence in PORT-020); the model never
    // chooses it and only proposes within its allowlist.
    m_personalityProfile = Companion::Personality::ProfileFromName(
        sConfig.GetStringDefault("PlayerBot.PersonalityProfile", "none").c_str());
    if (confDebug)
        sLog.outString("[PlayerBotMgr] personality profile:%s",
                       Companion::Personality::ProfileName(m_personalityProfile));
    // MVP-002 (KAP-552) lab-only probe (default off): deterministic stale
    // login-completion delivery; never set outside the Docker lab.
    m_staleProbeGuid = 0;
    m_staleProbeStage = 0;
    m_staleProbeOldGen = 0;
    std::string staleToken = sConfig.GetStringDefault("PlayerBot.TestStaleLogin", "");
    bool staleTokenValid = !staleToken.empty();
    uint32 staleProbeGuid = 0;
    for (size_t k = 0; k < staleToken.size(); ++k)
    {
        char c = staleToken[k];
        if (c < '0' || c > '9')
        {
            staleTokenValid = false;
            break;
        }
        staleProbeGuid = staleProbeGuid * 10 + (uint32)(c - '0');
    }
    m_staleProbeGuid = (staleTokenValid && staleProbeGuid != 0) ? staleProbeGuid : 0;
    if (m_staleProbeGuid)
        sLog.outString("Playerbot: stale-probe armed for %u (MVP-002 lab probe)", m_staleProbeGuid);
    // PORT-008 (KAP-558) lab-only probe (default off): deterministic owner
    // logout/relogin at fixed offsets after the owner's own login. Format:
    // <guid>,<logoutMs>,<reloginMs>. Never set outside the Docker lab.
    m_logoutProbeGuid = 0;
    m_logoutProbeLogoutMs = 0;
    m_logoutProbeReloginMs = 0;
    m_logoutProbeLoginMs = 0;
    m_logoutProbeStage = 0;
    {
        std::string logoutToken = sConfig.GetStringDefault("PlayerBot.TestLogoutScript", "");
        size_t c1 = logoutToken.find(',');
        size_t c2 = (c1 == std::string::npos) ? std::string::npos : logoutToken.find(',', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos)
        {
            m_logoutProbeGuid = (uint32)atoll(logoutToken.substr(0, c1).c_str());
            m_logoutProbeLogoutMs = (uint32)atoll(logoutToken.substr(c1 + 1, c2 - c1 - 1).c_str());
            m_logoutProbeReloginMs = (uint32)atoll(logoutToken.substr(c2 + 1).c_str());
        }
        else if (!logoutToken.empty())
            sLog.outError("Playerbot: test-logout-script malformed; skipped: %s", logoutToken.c_str());
        if (!(m_logoutProbeGuid && m_logoutProbeReloginMs > m_logoutProbeLogoutMs))
            m_logoutProbeGuid = 0;
        if (m_logoutProbeGuid)
            sLog.outString("Playerbot: test-logout armed guid:%u out:%u re:%u (PORT-008 lab probe)",
                           m_logoutProbeGuid, m_logoutProbeLogoutMs, m_logoutProbeReloginMs);
    }
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
        "SELECT p.char_guid, p.chance, p.ai, b.account_id, c.account, b.owner_account_id, "
        "per.schema_version, per.profile_id "
        "FROM playerbot p "
        "LEFT JOIN bot_ownership b ON b.char_guid = p.char_guid "
        "LEFT JOIN characters c ON c.guid = p.char_guid "
        "LEFT JOIN bot_personality per ON per.char_guid = p.char_guid");
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
            // TW-014: optional human-owner binding (NULL = unowned).
            uint32 ownerAccount = !fields[5].IsNULL() ? fields[5].GetUInt32() : 0;
            // PORT-020 (KAP-558): versioned personality identity.
            // A missing row stays (0,0) and is seeded from the
            // declared config profile at first login; a rejected
            // row (unknown schema or profile id) fails closed to
            // the baseline and is never overwritten.
            uint8 perSchema = 0;
            uint8 perProfile = 0;
            if (!fields[6].IsNULL())
            {
                perSchema = (uint8)fields[6].GetUInt32();
                perProfile = (uint8)fields[7].GetUInt32();
                if (!Companion::Personality::AcceptPersisted(perSchema, perProfile, perProfile))
                {
                    sLog.outError("Playerbot: personality row for %u rejected (schema=%u profile=%u); deterministic baseline",
                                   guid, perSchema, perProfile);
                    perProfile = 0;
                }
            }

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
            entry->ownerAccountId = ownerAccount;
            entry->personalitySchemaVersion = perSchema;
            entry->personalityProfile = perProfile;
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
    // TW-012: clamp both ends to real roster capacity while preserving exact
    // boundaries. The old >= / +1 normalization changed a one-bot 1..1
    // request into 0..1 and could produce a target above capacity.
      uint32 capacity = 0;
    for (auto const& entry : m_bots)
        if (!entry.second->customBot && !entry.second->isChatBot && !entry.second->ownerAccountId)
            ++capacity;
    confMinBots = std::min(confMinBots, capacity);
    confMaxBots = std::min(confMaxBots, capacity);
    if (confMaxBots < confMinBots)
        confMaxBots = confMinBots;

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

    // PORT-018 (KAP-558): bounded transport shutdown with the world; the
    // worker join cannot outlive one in-flight round. A disabled
    // transport (no URL) is a no-op here.
    m_plannerTransport.Shutdown();

    if (confDebug)
        sLog.outString("[PlayerBotMgr] Deleting all bots [OK]");
}

void PlayerBotMgr::OnBotLogin(PlayerBotEntry *e)
{
    e->state = PB_STATE_ONLINE;
    e->loadingSinceMs = 0;
    if (confDebug)
        sLog.outString("[PlayerBot][Login]  '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
    // PORT-020 (KAP-558): stable personality identity. A missing
    // row is seeded from the declared config profile; an existing
    // row (any schema) always wins, so re-inviting the same bot
    // restores the same personality. A failed write never fails the
    // login: the deterministic baseline remains.
    SyncPersonality(e);
}

void PlayerBotMgr::SyncPersonality(PlayerBotEntry *e)
{
    // Only owned companions carry a personality identity; the
    // row is written at most once (no-row -> config seed) and a
    // persisted or rejected row is never rewritten here.
    if (!e || !e->ownerAccountId || e->personalitySchemaVersion != 0)
        return;
    uint8 const profile = (uint8)m_personalityProfile;
    if (profile == 0)
        return; // declared baseline: no identity row, missing = baseline
    if (!CharacterDatabase.PExecute(
        "INSERT INTO bot_personality (char_guid, schema_version, profile_id, assigned_at) "
        "VALUES (%u, %u, %u, %u) "
        "ON DUPLICATE KEY UPDATE char_guid = char_guid",
        (uint32)e->playerGUID, (uint32)Companion::Personality::kPersonalitySchemaVersion,
        profile, (uint32)WorldTimer::getMSTime()))
        return; // failed write never fails the login: baseline stays
    e->personalitySchemaVersion = (uint8)Companion::Personality::kPersonalitySchemaVersion;
    e->personalityProfile = profile;
    if (confDebug)
        sLog.outString("[Personality] assigned GUID:%u profile:%s (config seed)",
                       e->playerGUID,
                       Companion::Personality::ProfileName(m_personalityProfile));
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

    // Preserve saved death state; recovery must use normal game paths.

    // CMP-010: a recall may have queued this login. Revalidate every mutable
    // condition after the bot is actually in-world; a newer dismiss/recruit
    // changes partySeq and makes this completion stale.
    if (e->pendingPartyLeaderGuid)
    {
        uint32 const leaderGuid = e->pendingPartyLeaderGuid;
        uint32 const sequence = e->pendingPartySeq;
        e->pendingPartyLeaderGuid = 0;
        e->pendingPartySeq = 0;
        Player* leader = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, leaderGuid));
        if (!CompletePartyRecruit(leader, e, sequence, player))
            sLog.outError("party recall completion rejected bot:%s guid:%u leader:%u seq:%u",
                          e->name.c_str(), e->playerGUID, leaderGuid, sequence);
    }
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
    // MVP-002 (KAP-552): deterministic stale-completion probe (lab-only,
    // config-gated; cheap state check when disabled).
    UpdateStaleLoginProbe();
    // PORT-008 (KAP-558): deterministic owner logout/relogin probe
    // (lab-only, config-gated; cheap state check when disabled).
    UpdateTestLogoutScript();
    // TW-014 (KAP-557): deterministic follow/stop script (lab-only,
    // config-gated; cheap state check when disabled).
    UpdateFollowScript();
    // NEXT-002 (post-MVP): deterministic party-invite script (lab-only,
    // config-gated; cheap state check when disabled).
    UpdatePartyInviteScript();
    // PORT-023 (KAP-558): deterministic owner quest script
    // (lab-only, config-gated; cheap state check when disabled).
    UpdateQuestScript();
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

void PlayerBotMgr::UpdateStaleLoginProbe()
{
    if (!m_staleProbeGuid || m_staleProbeStage >= 3)
        return;

    std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.find(m_staleProbeGuid);
    if (iter == m_bots.end())
    {
        sLog.outError("Playerbot: stale-probe %u has no entry; probe aborted (MVP-002)", m_staleProbeGuid);
        m_staleProbeStage = 3;
        return;
    }
    PlayerBotEntry* e = iter->second;

    switch (m_staleProbeStage)
    {
        case 0:
            if (e->state != PB_STATE_ONLINE)
                return; // wait for the original (generation-N) login to finish
            m_staleProbeOldGen = e->loginGeneration;
            sLog.outString("Playerbot: stale-probe %u: logging out gen %u (MVP-002)",
                           m_staleProbeGuid, m_staleProbeOldGen);
            DeleteBot(m_staleProbeGuid);
            m_staleProbeStage = 1;
            break;
        case 1:
            if (sWorld.FindSession(e->accountId))
                return; // wait for the old session to be dropped by WorldSession::Update
            if (!AddBot(m_staleProbeGuid, false))
            {
                sLog.outError("Playerbot: stale-probe %u re-login rejected; probe aborted (MVP-002)", m_staleProbeGuid);
                m_staleProbeStage = 3;
                return;
            }
            sLog.outString("Playerbot: stale-probe %u: re-login queued, generation now %u (MVP-002)",
                           m_staleProbeGuid, e->loginGeneration);
            m_staleProbeStage = 2;
            break;
        case 2:
            if (e->state != PB_STATE_ONLINE)
                return; // wait for the generation-(N+1) login to complete in-world
            TestDeliverStaleBotLoginCompletion(e->accountId, m_staleProbeGuid, m_staleProbeOldGen);
            sLog.outString("Playerbot: stale-probe %u: delivered stale gen %u against current gen %u (MVP-002)",
                           m_staleProbeGuid, m_staleProbeOldGen, e->loginGeneration);
            m_staleProbeStage = 3;
            break;
        default:
            break;
    }
}

// PORT-008 (KAP-558): lab-only owner logout/relogin probe (default off).
// At logoutMs after the probed bot's first observed ONLINE state its
// session is deleted (DeleteBot - the normal logout path), and at
// reloginMs it is queued back with AddBot. Offsets are relative to that
// login, so fixtures can reason in the same clock as the follow script.
// The probe is a pure driver; all companion behavior under owner loss is
// the AI's existing gate (hold safe) and the manager's order handling.
// ---------------------------------------------------------------------------
void PlayerBotMgr::UpdateTestLogoutScript()
{
    if (!m_logoutProbeGuid || m_logoutProbeStage >= 2)
        return;

    std::map<uint32, PlayerBotEntry*>::iterator iter = m_bots.find(m_logoutProbeGuid);
    if (iter == m_bots.end())
    {
        sLog.outError("Playerbot: test-logout %u has no entry; probe aborted (PORT-008)", m_logoutProbeGuid);
        m_logoutProbeStage = 2;
        return;
    }
    PlayerBotEntry* e = iter->second;

    switch (m_logoutProbeStage)
    {
        case 0:
            if (e->state != PB_STATE_ONLINE)
                return; // wait for the owner's login to finish
            if (m_logoutProbeLoginMs == 0)
            {
                m_logoutProbeLoginMs = m_elapsedTime;
                if (confDebug)
                    sLog.outString("Playerbot: test-logout %u baseline at %u (PORT-008)",
                                   m_logoutProbeGuid, m_elapsedTime);
                return;
            }
            if (m_elapsedTime < m_logoutProbeLoginMs + m_logoutProbeLogoutMs)
                return;
            sLog.outString("Playerbot: test-logout %u at %u (PORT-008)",
                           m_logoutProbeGuid, m_elapsedTime);
            DeleteBot(m_logoutProbeGuid);
            m_logoutProbeStage = 1;
            break;
        case 1:
            if (sWorld.FindSession(e->accountId))
                return; // wait for the old session to be dropped by WorldSession::Update
            if (m_elapsedTime < m_logoutProbeLoginMs + m_logoutProbeReloginMs)
                return;
            if (!AddBot(m_logoutProbeGuid, false))
            {
                sLog.outError("Playerbot: test-logout %u re-login rejected; probe aborted (PORT-008)", m_logoutProbeGuid);
                m_logoutProbeStage = 2;
                return;
            }
            sLog.outString("Playerbot: test-relogin %u at %u (PORT-008)",
                           m_logoutProbeGuid, m_elapsedTime);
            m_logoutProbeStage = 2;
            break;
        default:
            break;
    }
}
/*
Toutes les X minutes, ajoute ou enleve un bot.
*/
// ---------------------------------------------------------------------------
bool PlayerBotMgr::AddOrRemoveBot()
{
    uint32 const target = confMinBots == confMaxBots
        ? confMinBots : urand(confMinBots, confMaxBots);
    uint32 active = 0;
    for (auto const& entry : m_bots)
        if (!entry.second->customBot && !entry.second->isChatBot && !entry.second->ownerAccountId &&
            (entry.second->state == PB_STATE_ONLINE || entry.second->state == PB_STATE_LOADING))
            ++active;
    /*
    10 --- --- --- --- --- --- --- --- --- --- 20 bots
                NumActuel
    [alea ici : remove    ][    ici, add    ]
    */
    if (target > active)
        return AddRandomBot();
    if (target < active)
        return DeleteRandomBot();
    return false;
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
    uint32 availableChance = 0;
    for (std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.begin(); it != m_bots.end(); ++it)
        if (it->second->state == PB_STATE_OFFLINE && !it->second->customBot && it->second->ownerAccountId == 0)  // PORT-002
            availableChance += it->second->chance;
    if (!availableChance)
        return false;

    uint32 alea = urand(1, availableChance);
    std::map<uint32, PlayerBotEntry*>::iterator it;
    bool done = false;
    for (it = m_bots.begin(); it != m_bots.end() && !done; it++)
    {
        if (it->second->state != PB_STATE_OFFLINE)
            continue;

        if (it->second->customBot || it->second->ownerAccountId != 0)
            continue;  // PORT-002: owned companions managed by recruit/recall only

        uint32 chance = it->second->chance;

        if (chance < alea)
            alea -= chance;
        else
        {
            done = AddBot(it->first);
            // A selected bot whose login is rejected must not trigger an
            // unbounded same-tick retry; the next paced refresh may retry.
            break;
        }
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
    // PORT-002: count only ambient (non-owned, non-custom, non-chat) bots
    // that are online. Owned companions are managed exclusively by recruit/recall.
    uint32 eligibleCount = 0;
    for (std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.begin(); it != m_bots.end(); ++it)
        if (!it->second->customBot && !it->second->isChatBot && it->second->ownerAccountId == 0 && it->second->state == PB_STATE_ONLINE)
            eligibleCount++;
    if (eligibleCount < 1)
        return false;

    uint32 idDelete = urand(1, eligibleCount);
    uint32 onlinePassed = 0;
    std::map<uint32, PlayerBotEntry*>::iterator iter;
    for (iter = m_bots.begin(); iter != m_bots.end(); iter++)
    {
        if (!iter->second->customBot && !iter->second->isChatBot && iter->second->ownerAccountId == 0 && iter->second->state == PB_STATE_ONLINE)
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
// synthetic account (>= 1e9; no account row - C5) and a core-allocated character
// guid, creates a native Human Warrior through Player::Create,
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
    uint32 maxProvision = 0;
    QueryResult *provisionResult = CharacterDatabase.PQuery("SELECT COALESCE(MAX(account_id), 0) FROM bot_provision_state");
    if (provisionResult)
    {
        maxProvision = provisionResult->Fetch()[0].GetUInt32();
        delete provisionResult;
    }
    uint32 candidate = kReservedBase;
    if (maxReal >= candidate)
        candidate = maxReal + 1;
    if (maxBot >= candidate)
        candidate = maxBot + 1;
    if (maxOwner >= candidate)
        candidate = maxOwner + 1;
    if (maxProvision >= candidate)
        candidate = maxProvision + 1;
    return candidate;
}

void PlayerBotMgr::ProvisionPersistentBot(const std::string& name)
{
    BotIdentitySpec spec;
    if (!ParseBotIdentitySpec(name, spec))
    {
        sLog.outError("Playerbot provisioning: invalid identity spec; expected Name or Name,race,class,gender,skin,face,hairStyle,hairColor,facialHair");
        return;
    }
    std::string const& characterName = spec.name;
    if (!sObjectMgr.GetPlayerInfo(spec.race, spec.playerClass) ||
        (spec.gender != GENDER_MALE && spec.gender != GENDER_FEMALE))
    {
        sLog.outError("Playerbot provisioning: identity spec for '%s' has an invalid race/class/gender combination", characterName.c_str());
        return;
    }
    // Identity sanity: WoW 1.x character names are letters-only (2..12). Digits
    // would later be rejected by CheckPlayerName at login (the fixture bug fixed
    // for TW-006/TW-007), so provisioning enforces the same rule up front.
    if (characterName.size() < 2 || characterName.size() > 12)
    {
        sLog.outError("Playerbot provisioning: name '%s' has invalid length; not provisioned", characterName.c_str());
        return;
    }
    for (size_t i = 0; i < characterName.size(); ++i)
    {
        char c = characterName[i];
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z'))
        {
            sLog.outError("Playerbot provisioning: name '%s' is not letters-only; not provisioned", characterName.c_str());
            return;
        }
    }

    // --- Resolve the character (idempotency / resumability key) ---
    uint32 guid = 0;
    uint32 account = 0;
    QueryResult *qr = CharacterDatabase.PQuery("SELECT guid, account FROM characters WHERE name = '%s' LIMIT 1", characterName.c_str());
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

    bool hasProvisionState = false;
    uint32 provisionGuid = 0;
    uint32 provisionAccount = 0;
    uint32 provisionPhase = 0;
    BotIdentitySpec markerSpec;
    QueryResult *pq = CharacterDatabase.PQuery(
        "SELECT char_guid, account_id, phase, race_id, class_id, gender_id, skin_id, face_id, hair_style_id, hair_color_id, facial_hair_id FROM bot_provision_state WHERE character_name = '%s'",
        characterName.c_str());
    if (pq)
    {
        Field *f = pq->Fetch();
        hasProvisionState = true;
        provisionGuid = f[0].GetUInt32();
        provisionAccount = f[1].GetUInt32();
        provisionPhase = f[2].GetUInt32();
        markerSpec.name = characterName;
        markerSpec.race = f[3].GetUInt8();
        markerSpec.playerClass = f[4].GetUInt8();
        markerSpec.gender = f[5].GetUInt8();
        markerSpec.skin = f[6].GetUInt8();
        markerSpec.face = f[7].GetUInt8();
        markerSpec.hairStyle = f[8].GetUInt8();
        markerSpec.hairColor = f[9].GetUInt8();
        markerSpec.facialHair = f[10].GetUInt8();
        delete pq;
    }

    if (hasProvisionState && (markerSpec.race != spec.race ||
        markerSpec.playerClass != spec.playerClass || markerSpec.gender != spec.gender ||
        markerSpec.skin != spec.skin || markerSpec.face != spec.face ||
        markerSpec.hairStyle != spec.hairStyle || markerSpec.hairColor != spec.hairColor ||
        markerSpec.facialHair != spec.facialHair))
    {
        sLog.outError("Playerbot provisioning: identity spec for '%s' conflicts with its existing marker; rejected, nothing modified", characterName.c_str());
        return;
    }

    if (hasProvisionState && guid && (provisionGuid != guid || provisionAccount != account))
    {
        sLog.outError("Playerbot provisioning: marker for '%s' disagrees with character identity; rejected, nothing modified", characterName.c_str());
        return;
    }

    // C3: never adopt a character owned by a real (non-bot) account.
    if (guid != 0 && account < 1000000000u)
    {
        sLog.outError("Playerbot provisioning: name '%s' maps to character guid %u owned by a non-bot account %u; not adopted (C3)", characterName.c_str(), guid, account);
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
            sLog.outError("Playerbot provisioning: '%s' (guid %u) has an inconsistent binding (bound=%u owner=%u version=%u); rejected, nothing modified (fail closed)", characterName.c_str(), guid, boundAccount, account, provisionVersion);
            return;
        }
        if (hasRoster)
        {
            sLog.outString("Playerbot provisioning: '%s' (guid %u account %u) already provisioned; idempotent no-op", characterName.c_str(), guid, account);
            return;
        }
        // Consistent binding but a missing roster row: complete the roster only.
        if (!CharacterDatabase.PExecute("INSERT INTO playerbot (char_guid, chance, ai) VALUES (%u, 100, 'Default')", guid))
        {
            sLog.outError("Playerbot provisioning: roster completion failed for '%s' (guid %u); not provisioned", characterName.c_str(), guid);
            return;
        }
        if (!sObjectMgr.GetPlayerDataByGUID(guid))
            sObjectMgr.LoadPlayerCacheData(guid);
        sLog.outString("Playerbot provisioning: '%s' (guid %u account %u) completed (roster row added to existing binding)", characterName.c_str(), guid, account);
        return;
    }

    bool cleanedIncompleteCharacter = false;

    // A phase-1 marker proves this reserved character was created by this
    // provisioner but did not finish native persistence. Permanently remove
    // only that tightly validated partial identity, verify deletion, then
    // retry with the same reserved guid/account. This also clears partial
    // inventory/action rows left by the mixed MyISAM/InnoDB save path.
    if (guid != 0 && hasProvisionState && provisionPhase == 1)
    {
        if (hasBinding || hasRoster || account < 1000000000u)
        {
            sLog.outError("Playerbot provisioning: incomplete marker for '%s' has published ownership state; rejected, nothing modified", characterName.c_str());
            return;
        }
        Player::DeleteFromDB(ObjectGuid(HIGHGUID_PLAYER, guid), account, false, true);
        QueryResult *remaining = CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters WHERE guid = %u", guid);
        bool stillExists = remaining && remaining->Fetch()[0].GetUInt32() != 0;
        delete remaining;
        if (stillExists)
        {
            sLog.outError("Playerbot provisioning: cleanup of incomplete native character '%s' (guid %u) failed; retry stopped", characterName.c_str(), guid);
            return;
        }
        sLog.outString("Playerbot provisioning: cleaned incomplete native character '%s' (guid %u); retrying reserved identity", characterName.c_str(), guid);
        cleanedIncompleteCharacter = true;
        guid = 0;
        account = 0;
    }

    // An unbound character is resumable only when this provisioner recorded a
    // completed native save. Legacy raw fixtures and unrelated characters are
    // never upgraded implicitly.
    if (guid != 0 && (!hasProvisionState || provisionPhase != 2))
    {
        sLog.outError("Playerbot provisioning: unbound character '%s' (guid %u) has no completed native provision marker; rejected, nothing modified", characterName.c_str(), guid);
        return;
    }

    // --- No binding: native fresh create or completed-native orphan resume ---
    if (guid == 0)
    {
        if (hasProvisionState)
        {
            guid = provisionGuid;
            account = provisionAccount;
            if (provisionPhase != 1)
            {
                sLog.outError("Playerbot provisioning: ready marker for '%s' has no character; rejected, nothing modified", characterName.c_str());
                return;
            }
            // If no character ever reached MyISAM, the core generator did not
            // learn this marker's guid on restart. Consume its next guid and
            // move the pre-character marker when necessary, preventing a later
            // human character from receiving the same guid. A cleaned partial
            // character was present during SetHighestGuids, so its old guid is
            // already below the generator frontier and remains safe to reuse.
            if (!cleanedIncompleteCharacter)
            {
                uint32 coreGuid = sObjectMgr.GeneratePlayerLowGuid();
                if (!coreGuid || (coreGuid != guid && !CharacterDatabase.DirectPExecute(
                        "UPDATE bot_provision_state SET char_guid = %u WHERE char_guid = %u AND account_id = %u AND phase = 1",
                        coreGuid, guid, account)))
                {
                    sLog.outError("Playerbot provisioning: could not reserve a core guid for retrying '%s'; not provisioned", characterName.c_str());
                    return;
                }
                guid = coreGuid;
            }
        }
        else
        {
            account = AllocateReservedBotAccount();
            guid = sObjectMgr.GeneratePlayerLowGuid();
            if (!guid || !CharacterDatabase.DirectPExecute(
                    "INSERT INTO bot_provision_state (char_guid, account_id, character_name, phase, race_id, class_id, gender_id, skin_id, face_id, hair_style_id, hair_color_id, facial_hair_id) VALUES (%u, %u, '%s', 1, %u, %u, %u, %u, %u, %u, %u, %u)",
                    guid, account, characterName.c_str(), spec.race, spec.playerClass,
                    spec.gender, spec.skin, spec.face, spec.hairStyle, spec.hairColor,
                    spec.facialHair))
            {
                sLog.outError("Playerbot provisioning: could not reserve native identity for '%s'; not provisioned", characterName.c_str());
                return;
            }
        }

        PlayerBotEntry provisionEntry(guid, account, 100);
        provisionEntry.persistent = true;
        WorldSession provisionSession(account, nullptr, SEC_PLAYER, 0, LOCALE_enUS, "<BOT-PROVISION>", 0);
        provisionSession.SetBot(&provisionEntry);
        Player nativePlayer(&provisionSession);
        if (!nativePlayer.Create(guid, characterName, spec.race, spec.playerClass, spec.gender, spec.skin, spec.face, spec.hairStyle, spec.hairColor, spec.facialHair))
        {
            sLog.outError("Playerbot provisioning: native Player::Create failed for '%s'; marker retained for retry", characterName.c_str());
            return;
        }
        nativePlayer.SetCinematic(1);
        nativePlayer.SetAtLoginFlag(AT_LOGIN_FIRST);
        MasterPlayer nativeMaster(&provisionSession);
        nativeMaster.Create(&nativePlayer);
        if (!nativePlayer.SaveToDB(false, true, true))
        {
            sLog.outError("Playerbot provisioning: native save failed for '%s'; incomplete marker retained, no roster published", characterName.c_str());
            return;
        }
        if (!CharacterDatabase.BeginTransaction())
        {
            sLog.outError("Playerbot provisioning: cannot begin native action save for '%s'; incomplete marker retained", characterName.c_str());
            return;
        }
        nativeMaster.SaveActions();
        if (!CharacterDatabase.CommitTransactionDirect())
        {
            sLog.outError("Playerbot provisioning: native action save failed for '%s'; incomplete marker retained", characterName.c_str());
            return;
        }
        PlayerInfo const* info = sObjectMgr.GetPlayerInfo(spec.race, spec.playerClass);
        if (!info || !CharacterDatabase.DirectPExecute(
                "INSERT INTO character_homebind (guid,map,zone,position_x,position_y,position_z) VALUES (%u,%u,%u,%f,%f,%f)",
                guid, info->mapId, info->areaId, info->positionX, info->positionY, info->positionZ) ||
            !CharacterDatabase.DirectPExecute("UPDATE bot_provision_state SET phase = 2 WHERE char_guid = %u AND account_id = %u", guid, account))
        {
            sLog.outError("Playerbot provisioning: native home/action state incomplete for '%s'; marker retained, no roster published", characterName.c_str());
            return;
        }

        // Publish ownership only after native state is complete. MyISAM means
        // this remains resumable rather than cross-table atomic.
        if (!CharacterDatabase.DirectPExecute("INSERT INTO bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES (%u, %u, 1, 2)", guid, account) ||
            !CharacterDatabase.DirectPExecute("INSERT INTO playerbot (char_guid, chance, ai) VALUES (%u, 100, 'Default')", guid))
        {
            sLog.outError("Playerbot provisioning: native character '%s' saved but roster publication failed; retry will resume", characterName.c_str());
            return;
        }
        sObjectMgr.InsertPlayerInCache(&nativePlayer);
        sObjectMgr.UpdatePlayerCachedPosition(&nativePlayer);
        sLog.outString("Playerbot provisioning: created native character '%s' (guid %u account %u); bound (provision_version=2)", characterName.c_str(), guid, account);
        return;
    }

    // Native-ready orphan resume: complete it in place (same identity).
    if (!CharacterDatabase.DirectPExecute("INSERT INTO bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES (%u, %u, 1, 2)", guid, account) ||
        (!hasRoster && !CharacterDatabase.DirectPExecute("INSERT INTO playerbot (char_guid, chance, ai) VALUES (%u, 100, 'Default')", guid)))
    {
        sLog.outError("Playerbot provisioning: native-ready orphan completion failed for '%s' (guid %u); retryable", characterName.c_str(), guid);
        return;
    }
    if (!sObjectMgr.GetPlayerDataByGUID(guid))
        sObjectMgr.LoadPlayerCacheData(guid);
    sLog.outString("Playerbot provisioning: '%s' (guid %u account %u) completed (native-ready character); bound (provision_version=2)", characterName.c_str(), guid, account);
}

// ---------------------------------------------------------------------------
// TW-014 (KAP-557): deterministic owner-only follow/stop for one owned
// companion.
//
// Commands (SEC_PLAYER, owner-checked): .botfollow <botname>, .botstop
// <botname>. Ownership is the bot_ownership binding loaded with the roster
// entry (entry accountId must equal the issuer's session account); party
// membership is not required and not checked.
//
// Lab-only FollowScript: replays owner chat events (through the real chat
// path) and one stale follow-goal delivery (directly, with an expired seq)
// so the sequence guard is exercised deterministically.
// ---------------------------------------------------------------------------
PlayerBotEntry* PlayerBotMgr::FindBotByName(const std::string& name) const
{
    for (std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.begin(); it != m_bots.end(); ++it)
    {
        std::string const& n = it->second->name;
        if (n.size() != name.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < name.size(); ++i)
            if (tolower((unsigned char)n[i]) != tolower((unsigned char)name[i]))
            {
                same = false;
                break;
            }
        if (same)
            return it->second;
    }
    return nullptr;
}

PlayerBotEntry* PlayerBotMgr::FindBotByGuid(uint32 guid) const
{
    std::map<uint32, PlayerBotEntry*>::const_iterator const it = m_bots.find(guid);
    return (it != m_bots.end()) ? it->second : nullptr;
}

bool PlayerBotMgr::BotFollow(Player* issuer, const std::string& botName)
{
    if (!issuer || !issuer->GetSession() || botName.empty())
        return false;
    PlayerBotEntry* e = FindBotByName(botName);
    if (!e)
    {
        sLog.outError("follow rejected unknown bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    uint32 const issuerAcc = issuer->GetSession()->GetAccountId();
    if (!e->ownerAccountId)
    {
        sLog.outError("follow rejected unowned bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->ownerAccountId != issuerAcc)
    {
        sLog.outError("follow rejected not-owner bot:%s issuer:%u acc:%u owner:%u",
                      botName.c_str(), issuer->GetGUIDLow(), issuerAcc, e->ownerAccountId);
        return false;
    }
    if (e->state != PB_STATE_ONLINE || !e->ai)
    {
        sLog.outError("follow rejected offline bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    ++e->followSeq;
    e->ai->FollowGoal(issuer->GetGUIDLow(), e->followSeq);
    sLog.outString("follow accepted bot:%s guid:%u leader:%u seq:%u",
                   botName.c_str(), e->playerGUID, issuer->GetGUIDLow(), e->followSeq);
    return true;
}

bool PlayerBotMgr::BotStop(Player* issuer, const std::string& botName)
{
    if (!issuer || !issuer->GetSession() || botName.empty())
        return false;
    PlayerBotEntry* e = FindBotByName(botName);
    if (!e)
    {
        sLog.outError("stop rejected unknown bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    uint32 const issuerAcc = issuer->GetSession()->GetAccountId();
    if (!e->ownerAccountId)
    {
        sLog.outError("stop rejected unowned bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->ownerAccountId != issuerAcc)
    {
        sLog.outError("stop rejected not-owner bot:%s issuer:%u acc:%u owner:%u",
                      botName.c_str(), issuer->GetGUIDLow(), issuerAcc, e->ownerAccountId);
        return false;
    }
    if (e->state != PB_STATE_ONLINE || !e->ai)
    {
        sLog.outError("stop rejected offline bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    e->ai->FollowStop();
    sLog.outString("stop accepted bot:%s guid:%u issuer:%u seq:%u",
                   botName.c_str(), e->playerGUID, issuer->GetGUIDLow(), e->followSeq);
    return true;
}

bool PlayerBotMgr::BotHold(Player* issuer, const std::string& botName)
{
    if (!issuer || !issuer->GetSession() || botName.empty())
        return false;
    PlayerBotEntry* e = FindBotByName(botName);
    if (!e)
    {
        sLog.outError("hold rejected unknown bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    uint32 const issuerAcc = issuer->GetSession()->GetAccountId();
    if (!e->ownerAccountId)
    {
        sLog.outError("hold rejected unowned bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->ownerAccountId != issuerAcc)
    {
        sLog.outError("hold rejected not-owner bot:%s issuer:%u acc:%u owner:%u",
                      botName.c_str(), issuer->GetGUIDLow(), issuerAcc, e->ownerAccountId);
        return false;
    }
    if (e->state != PB_STATE_ONLINE || !e->ai)
    {
        sLog.outError("hold rejected offline bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    e->ai->Hold(++e->followSeq);
    sLog.outString("hold accepted bot:%s guid:%u issuer:%u",
                   botName.c_str(), e->playerGUID, issuer->GetGUIDLow());
    return true;
}

// ---------------------------------------------------------------------------
// PORT-005 (KAP-558): owner-selected assist (.botassist <botname> <target>).
// The companion must be in the issuer's party; the target is looked up by
// name around the companion (grid visit; a legal live hostile wins over a
// closer invalid live match, and a dead match is kept as fallback so the
// caller can report target-dead when no live match exists). Every outcome,
// accepted or rejected, is logged. The AI re-validates the target and the order
// generation at execution time.
// ---------------------------------------------------------------------------
namespace
{
// Search radius for the .botassist name lookup, centered on the companion.
float const kBotAssistSearchRange = 30.0f;

// PORT-005/PORT-023 (KAP-558): an explicitly named assist target is legal
// when the bot can attack it the way the core attack path allows, or when
// the neutral-faction evidence rule from defend applies (the target is
// actively fighting the issuer or the bot). The core player melee path
// (Unit::Attack) performs no faction hostility check: this client's player
// faction templates carry no hostile masks for "attacker"-style creatures
// (Westfall Nightsabers/Thistle Boars, faction templates 7/189), so the
// non-forced CanAttack is effectively always false for a player and would
// reject every pre-combat assist; the player gate mirrors Unit::Attack.
static bool IsAssistLegalTarget(Unit const* me, Unit const* issuer, Unit const* u)
{
    if (u->IsPlayer() || me->IsFriendlyTo(u))
        return false;
    if (u->IsCreature() && ((Creature const*)u)->IsInEvadeMode())
        return false;
    if (me->IsPlayer())
        return u->IsTargetable(true, me->IsCharmerOrOwnerPlayerOrPlayerItself());
    if (me->CanAttack(u))
        return true;
    if (!u->IsTargetable(true, me->IsCharmerOrOwnerPlayerOrPlayerItself()))
        return false;
    Unit const* const victim = u->GetVictim();
    if (victim == me || (issuer && victim == issuer))
        return true;
    if (u->GetAttackers().count(const_cast<Unit*>(me)) != 0)
        return true;
    return issuer != nullptr && u->GetAttackers().count(const_cast<Unit*>(issuer)) != 0;
}

class BotAssistNameCheck
{
public:
    BotAssistNameCheck(Unit const* source, Unit const* issuer, const std::string& name)
        : me(source), issuer(issuer), name(name),
          m_legal(nullptr), m_legalDist(0.0f),
          m_invalid(nullptr), m_invalidDist(0.0f),
          m_dead(nullptr), m_deadDist(0.0f) {}

    bool operator()(Unit* u)
    {
        if (me == u || u->GetName() != name)
            return false;
        if (!u->IsWithinDistInMap(me, kBotAssistSearchRange, false, SizeFactor::None))
            return false;
        float const dist = me->GetDistance(u);
        // Rank by legality, not raw distance: the companion farms its own
        // corpses (a dead-first selection would make assist unusable right
        // after a kill), and a closer invalid same-name match (a friendly
        // or unattackable unit, a player) must not mask a farther legal
        // hostile the owner explicitly named. Keep the nearest invalid live
        // match for a precise rejection reason and the nearest dead match
        // for target-dead.
        if (u->IsAlive())
        {
            if (IsAssistLegalTarget(me, issuer, u))
            {
                if (!m_legal || dist < m_legalDist)
                {
                    m_legal = u;
                    m_legalDist = dist;
                }
            }
            else if (!m_invalid || dist < m_invalidDist)
            {
                m_invalid = u;
                m_invalidDist = dist;
            }
        }
        else if (!m_dead || dist < m_deadDist)
        {
            m_dead = u;
            m_deadDist = dist;
        }
        return true;
    }

    // Legal live hostile first, then the nearest invalid live match, then
    // the nearest dead match.
    Unit* Best() const { return m_legal ? m_legal : (m_invalid ? m_invalid : m_dead); }

private:
    Unit const* me;
    Unit const* issuer;
    std::string const& name;
    Unit* m_legal;
    float m_legalDist;
    Unit* m_invalid;
    float m_invalidDist;
    Unit* m_dead;
    float m_deadDist;
};
}

bool PlayerBotMgr::BotAssist(Player* issuer, const std::string& botName, const std::string& targetName)
{
    if (!issuer || !issuer->GetSession() || botName.empty() || targetName.empty())
        return false;
    PlayerBotEntry* e = FindBotByName(botName);
    if (!e)
    {
        sLog.outError("assist rejected unknown bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    uint32 const issuerAcc = issuer->GetSession()->GetAccountId();
    if (!e->ownerAccountId)
    {
        sLog.outError("assist rejected unowned bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->ownerAccountId != issuerAcc)
    {
        sLog.outError("assist rejected not-owner bot:%s issuer:%u acc:%u owner:%u",
                      botName.c_str(), issuer->GetGUIDLow(), issuerAcc, e->ownerAccountId);
        return false;
    }
    if (e->state != PB_STATE_ONLINE || !e->ai)
    {
        sLog.outError("assist rejected offline bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    Player* bot = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, uint32(e->playerGUID)));
    if (!bot || bot->GetSession() != e->session || !bot->GetMap())
    {
        sLog.outError("assist rejected not-in-world bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (!issuer->GetGroup() || bot->GetGroup() != issuer->GetGroup())
    {
        sLog.outError("assist rejected not-in-party bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    Unit* target = nullptr;
    {
        CellPair const p(MaNGOS::ComputeCellPair(bot->GetPositionX(), bot->GetPositionY()));
        Cell cell(p);
        cell.SetNoCreate();
        BotAssistNameCheck check(bot, issuer, targetName);
        MaNGOS::UnitLastSearcher<BotAssistNameCheck> searcher(target, check);
        TypeContainerVisitor<MaNGOS::UnitLastSearcher<BotAssistNameCheck>, WorldTypeMapContainer> world_searcher(searcher);
        TypeContainerVisitor<MaNGOS::UnitLastSearcher<BotAssistNameCheck>, GridTypeMapContainer> grid_searcher(searcher);
        cell.Visit(p, world_searcher, *bot->GetMap(), *bot, kBotAssistSearchRange);
        cell.Visit(p, grid_searcher, *bot->GetMap(), *bot, kBotAssistSearchRange);
        target = check.Best();
    }
    if (!target)
    {
        sLog.outError("assist rejected target-not-found bot:%s target:%s issuer:%u",
                      botName.c_str(), targetName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (target->IsPlayer())
    {
        sLog.outError("assist rejected target-is-player bot:%s target:%s issuer:%u",
                      botName.c_str(), targetName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (!target->IsAlive())
    {
        sLog.outError("assist rejected target-dead bot:%s target:%s issuer:%u",
                      botName.c_str(), targetName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (bot->IsFriendlyTo(target))
    {
        sLog.outError("assist rejected target-friendly bot:%s target:%s issuer:%u",
                      botName.c_str(), targetName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (!IsAssistLegalTarget(bot, issuer, target))
    {
        sLog.outError("assist rejected target-invalid bot:%s target:%s issuer:%u",
                      botName.c_str(), targetName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (!bot->IsWithinLOSInMap(target))
    {
        sLog.outError("assist rejected target-not-in-sight bot:%s target:%s issuer:%u",
                      botName.c_str(), targetName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    // PORT-014 (KAP-558): bounded pull for the declared tank matrix only
    // (see Companion/Tank.h). The engaged count is derived from the world
    // at command time, with no bookkeeping: the live hostile victim, the
    // live hostile attackers of the bot, and nearby live hostiles that
    // still tie the bot in - as their victim or through stored threat in
    // their threat list (a threat entry survives a victim switch until the
    // fight ends, which is exactly the "the tank still owns this target"
    // evidence). At or above the cap a new assist is refused: the tank
    // does not pull an unrelated creature while it already holds two.
    if (bot->GetClass() == Companion::Tank::kDeclaredTankClass &&
        bot->GetLevel() >= Companion::Tank::kDeclaredTankMinLevel &&
        bot->HasSpell(Companion::Tank::kDeclaredTankTaunt))
    {
        class BotPullCapCheck
        {
        public:
            explicit BotPullCapCheck(Player* bot) : bot(bot) { }
            void AddDirect(Unit const* u)
            {
                if (u && u->IsAlive() && u->IsCreature() && !bot->IsFriendlyTo(u))
                    counted.insert(u->GetGUIDLow());
            }
            bool operator()(Unit* u)
            {
                if (!u || !u->IsCreature() || !u->IsAlive() || bot->IsFriendlyTo(u))
                    return true;
                Creature* c = u->ToCreature();
                bool tied = c->GetVictim() == bot;
                if (!tied && c->CanHaveThreatList())
                    tied = c->GetThreatManager().getThreat(bot, false) > 0.0f;
                if (tied)
                    counted.insert(u->GetGUIDLow());
                return true;
            }
            uint32 Count() const { return (uint32)counted.size(); }
        private:
            Player* bot;
            std::set<uint32> counted;
        };
        BotPullCapCheck cap(bot);
        cap.AddDirect(bot->GetVictim());
        for (Unit const* u : bot->GetAttackers())
            cap.AddDirect(u);
        {
            CellPair const p(MaNGOS::ComputeCellPair(bot->GetPositionX(), bot->GetPositionY()));
            Cell cell(p);
            cell.SetNoCreate();
            Unit* capIgnored = nullptr; // the searcher result is unused; the check counts
            MaNGOS::UnitLastSearcher<BotPullCapCheck> searcher(capIgnored, cap);
            TypeContainerVisitor<MaNGOS::UnitLastSearcher<BotPullCapCheck>, WorldTypeMapContainer> world_searcher(searcher);
            TypeContainerVisitor<MaNGOS::UnitLastSearcher<BotPullCapCheck>, GridTypeMapContainer> grid_searcher(searcher);
            cell.Visit(p, world_searcher, *bot->GetMap(), *bot, kBotAssistSearchRange);
            cell.Visit(p, grid_searcher, *bot->GetMap(), *bot, kBotAssistSearchRange);
        }
        if (cap.Count() >= Companion::Tank::kDeclaredTankPullCap)
        {
            sLog.outError("assist rejected pull-cap bot:%s target:%s engaged:%u cap:%u issuer:%u",
                          botName.c_str(), targetName.c_str(), cap.Count(),
                          Companion::Tank::kDeclaredTankPullCap, issuer->GetGUIDLow());
            return false;
        }
    }
    e->ai->AssistTarget(target->GetObjectGuid().GetRawValue(), ++e->followSeq);
    sLog.outString("assist accepted bot:%s target:%s guid:%u seq:%u",
                   e->name.c_str(), targetName.c_str(), target->GetGUIDLow(), e->followSeq);
    return true;
}

// ---------------------------------------------------------------------------
// PORT-006 (KAP-558): owner-toggled reactive defend (.botdefend <bot> on|off).
// While enabled, a following companion engages a legal creature that is
// actually attacking the owner or the companion (AI side, every tick).
// The flag is session-scoped: a world restart clears it. Every outcome is
// logged; the authorization ladder mirrors BotStop/BotHold.
// ---------------------------------------------------------------------------
bool PlayerBotMgr::BotDefend(Player* issuer, const std::string& botName, bool enable)
{
    if (!issuer || !issuer->GetSession() || botName.empty())
        return false;
    PlayerBotEntry* e = FindBotByName(botName);
    if (!e)
    {
        sLog.outError("defend rejected unknown bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    uint32 const issuerAcc = issuer->GetSession()->GetAccountId();
    if (!e->ownerAccountId)
    {
        sLog.outError("defend rejected unowned bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->ownerAccountId != issuerAcc)
    {
        sLog.outError("defend rejected not-owner bot:%s issuer:%u acc:%u owner:%u",
                      botName.c_str(), issuer->GetGUIDLow(), issuerAcc, e->ownerAccountId);
        return false;
    }
    if (e->state != PB_STATE_ONLINE || !e->ai)
    {
        sLog.outError("defend rejected offline bot:%s issuer:%u", botName.c_str(), issuer->GetGUIDLow());
        return false;
    }
    e->defendEnabled = enable;
    sLog.outString("defend %s bot:%s guid:%u issuer:%u acc:%u",
                   enable ? "enabled" : "disabled", botName.c_str(), e->playerGUID,
                   issuer->GetGUIDLow(), issuerAcc);
    return true;
}

// PORT-022 (KAP-558): bounded conversational party chat.
namespace
{
// Single-line, bounded, printable-ASCII-only: strips control characters
// (including newlines), trims, and caps the length. Matches the adapter's
// outbound sanitizer so a request is never reinterpreted as a command and
// never exceeds the transport byte budget.
std::string SanitizeConverseText(std::string const& in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char ch : in)
        if (ch >= 0x20 && ch <= 0x7e)
            out.push_back((char)ch);
    size_t b = out.find_first_not_of(" ");
    size_t e = out.find_last_not_of(" ");
    if (b == std::string::npos)
        return std::string();
    out = out.substr(b, e - b + 1);
    if (out.size() > Companion::Conversation::kMaxTextBytes)
        out = out.substr(0, Companion::Conversation::kMaxTextBytes);
    return out;
}
} // namespace

bool PlayerBotMgr::BotPartyMessage(Player* issuer, const std::string& rawText)
{
    if (!issuer || !issuer->GetSession() || rawText.empty())
        return false;
    std::string const text = SanitizeConverseText(rawText);
    if (text.empty())
    {
        sLog.outError("conversation rejected empty issuer:%u", issuer->GetGUIDLow());
        return false;
    }
    // Addressing: the first token must exactly (case-insensitive) name a bot.
    size_t const sp = text.find_first_of(" ");
    std::string const first = (sp == std::string::npos) ? text : text.substr(0, sp);
    PlayerBotEntry* e = FindBotByName(first);
    if (!e)
    {
        if (confDebug)
            sLog.outString("conversation unaddressed issuer:%u first:%s",
                           issuer->GetGUIDLow(), first.c_str());
        return false; // not clearly addressed to a companion
    }
    if (!ValidatePartyOwner(issuer, e, "converse"))
        return false;
    Group* group = issuer->GetGroup();
    if (!group || group->isBGGroup() ||
        !group->IsMember(ObjectGuid(HIGHGUID_PLAYER, uint32(e->playerGUID))))
    {
        sLog.outError("conversation rejected not-in-party bot:%s issuer:%u",
                      e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->state != PB_STATE_ONLINE)
    {
        sLog.outError("conversation rejected offline bot:%s issuer:%u",
                      e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    uint32 const leaderLow = group->GetLeaderGuid().GetCounter();
    uint32 const groupSig = group->GetId();
    uint32 const profileCode = (uint32)e->personalityProfile;
    // The companion hears the full addressed message; the dispatcher has
    // already confirmed it is clearly addressed to this companion.
    if (m_conversationTransport.Submit(uint32(e->playerGUID), groupSig, leaderLow,
                                       profileCode, text, WorldTimer::getMSTime()))
    {
        if (confDebug)
            sLog.outString("[Conversation] addressed bot:%s issuer:%u group:%u leader:%u profile:%u len:%u",
                           e->name.c_str(), issuer->GetGUIDLow(), groupSig, leaderLow,
                           profileCode, (uint32)text.size());
        return true;
    }
    sLog.outError("conversation submit refused bot:%s issuer:%u (busy or table full)",
                  e->name.c_str(), issuer->GetGUIDLow());
    return false;
}

bool PlayerBotMgr::ValidatePartyOwner(Player* issuer, PlayerBotEntry* e, const char* action) const
{
    if (!issuer || !issuer->GetSession() || !e)
        return false;
    uint32 const issuerAcc = issuer->GetSession()->GetAccountId();
    if (!e->ownerAccountId)
    {
        sLog.outError("party %s rejected unowned bot:%s issuer:%u", action, e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (e->ownerAccountId != issuerAcc)
    {
        sLog.outError("party %s rejected not-owner bot:%s issuer:%u acc:%u owner:%u",
                      action, e->name.c_str(), issuer->GetGUIDLow(), issuerAcc, e->ownerAccountId);
        return false;
    }
    return true;
}

bool PlayerBotMgr::CompletePartyRecruit(Player* issuer, PlayerBotEntry* e, uint32 sequence, Player* knownBot)
{
    if (!ValidatePartyOwner(issuer, e, "recruit"))
        return false;
    if (sequence != e->partySeq)
    {
        sLog.outError("party recruit rejected stale bot:%s seq:%u current:%u",
                      e->name.c_str(), sequence, e->partySeq);
        return false;
    }
    if (e->state != PB_STATE_ONLINE || !e->session)
    {
        sLog.outError("party recruit rejected offline bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    Player* bot = knownBot ? knownBot : sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, uint32(e->playerGUID)));
    if (!bot || bot->GetSession() != e->session)
    {
        sLog.outError("party recruit rejected missing in-world bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    // KAP-558 hardening: no combat gate. The UI-invite settlement path
    // (HandlePartyInvite) has no combat gate and Group::AddMember is safe
    // in combat; an owner must stay able to recruit or dismiss a
    // companion that is defending or in an ongoing fight.
    if (!sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_GROUP) && issuer->GetTeam() != bot->GetTeam())
    {
        sLog.outError("party recruit rejected faction bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (issuer->HandleHardcoreInteraction(bot, true) != Player::HardcoreInteractionResult::Allowed)
    {
        sLog.outError("party recruit rejected hardcore bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }

    Group* group = issuer->GetGroup();
    if (group && group->isBGGroup())
    {
        sLog.outError("party recruit rejected battleground bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    // KAP-558 hardening: owner authority, not party leadership. Classic
    // rules transfer leadership to the bot when the owner logs out, and
    // the owner must stay able to re-recruit while the bot leads.
    // Group::AddMember performs no authority check (chat handlers do).
    if (bot->GetGroup())
    {
        if (bot->GetGroup() == group)
        {
            sLog.outString("party recruit already-member bot:%s guid:%u leader:%u seq:%u",
                           e->name.c_str(), e->playerGUID, issuer->GetGUIDLow(), sequence);
            return true;
        }
        sLog.outError("party recruit rejected already-grouped bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    if (group && group->IsFull())
    {
        sLog.outError("party recruit rejected full bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }

    bool created = false;
    if (!group)
    {
        group = new Group;
        if (!group->Create(issuer->GetObjectGuid(), issuer->GetName()))
        {
            delete group;
            sLog.outError("party recruit rejected create-failed bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
            return false;
        }
        sObjectMgr.AddGroup(group);
        created = true;
    }
    if (!group->AddMember(bot->GetObjectGuid(), bot->GetName()))
    {
        if (created)
            group->Disband(true, issuer->GetObjectGuid());
        sLog.outError("party recruit rejected add-failed bot:%s issuer:%u", e->name.c_str(), issuer->GetGUIDLow());
        return false;
    }
    group->BroadcastGroupUpdate();
    sLog.outString("party recruit accepted bot:%s guid:%u leader:%u seq:%u group:%u",
                   e->name.c_str(), e->playerGUID, issuer->GetGUIDLow(), sequence, group->GetId());
    return true;
}

bool PlayerBotMgr::BotRecruit(Player* issuer, const std::string& botName)
{
    PlayerBotEntry* e = FindBotByName(botName);
    if (!ValidatePartyOwner(issuer, e, "recruit"))
        return false;
    ++e->partySeq;
    e->pendingPartyLeaderGuid = 0;
    e->pendingPartySeq = 0;
    return CompletePartyRecruit(issuer, e, e->partySeq);
}

bool PlayerBotMgr::BotRecall(Player* issuer, const std::string& botName)
{
    PlayerBotEntry* e = FindBotByName(botName);
    if (!ValidatePartyOwner(issuer, e, "recall"))
        return false;
    ++e->partySeq;
    e->pendingPartyLeaderGuid = issuer->GetGUIDLow();
    e->pendingPartySeq = e->partySeq;
    if (e->state == PB_STATE_ONLINE)
    {
        uint32 const sequence = e->pendingPartySeq;
        e->pendingPartyLeaderGuid = 0;
        e->pendingPartySeq = 0;
        return CompletePartyRecruit(issuer, e, sequence);
    }
    if (e->state == PB_STATE_OFFLINE && !AddBot(e->playerGUID))
    {
        e->pendingPartyLeaderGuid = 0;
        e->pendingPartySeq = 0;
        sLog.outError("party recall rejected login bot:%s issuer:%u seq:%u",
                      e->name.c_str(), issuer->GetGUIDLow(), e->partySeq);
        return false;
    }
    sLog.outString("party recall queued bot:%s guid:%u leader:%u seq:%u",
                   e->name.c_str(), e->playerGUID, issuer->GetGUIDLow(), e->partySeq);
    return true;
}

bool PlayerBotMgr::BotDismiss(Player* issuer, const std::string& botName)
{
    PlayerBotEntry* e = FindBotByName(botName);
    if (!ValidatePartyOwner(issuer, e, "dismiss"))
        return false;
    bool const recallPending = e->pendingPartyLeaderGuid != 0;
    uint32 const pendingSequence = e->pendingPartySeq;
    ++e->partySeq;
    Group* group = issuer->GetGroup();
    if (recallPending &&
        (!group || !group->IsMember(ObjectGuid(HIGHGUID_PLAYER, uint32(e->playerGUID)))))
    {
        // Keep the captured request until login completes so it is rejected
        // by the same sequence check used for every asynchronous recall.
        sLog.outString("party dismiss cancelled pending recall bot:%s guid:%u leader:%u pending:%u current:%u",
                       e->name.c_str(), e->playerGUID, issuer->GetGUIDLow(), pendingSequence, e->partySeq);
        return true;
    }
    e->pendingPartyLeaderGuid = 0;
    e->pendingPartySeq = 0;
    if (!group || group->isBGGroup() ||
        !group->IsMember(ObjectGuid(HIGHGUID_PLAYER, uint32(e->playerGUID))))
    {
        sLog.outError("party dismiss rejected membership bot:%s issuer:%u seq:%u",
                      e->name.c_str(), issuer->GetGUIDLow(), e->partySeq);
        return false;
    }
    ObjectGuid const botGuid(HIGHGUID_PLAYER, uint32(e->playerGUID));
    Player* bot = sObjectAccessor.FindPlayer(botGuid);
    // KAP-558 hardening: no combat gate (same rationale as recruit), and
    // dismiss no longer benches the bot: it stays online and returns to
    // its default owner-follow (the command reference promises "stays
    // online"). DeleteBot here forced a .botrecall after every dismiss.
    if (bot && e->ai)
        e->ai->FollowStop();
    // Owner authority covers both leadership cases: if the issuer leads,
    // the bot is kicked; if the bot holds leadership (classic rules
    // transfer it to the bot on owner logout), the bot leaves itself.
    // A 2-person party disbands on either path (vanilla rule).
    bool const issuerLeads = group->IsLeader(issuer->GetObjectGuid());
    if (issuerLeads)
        group->RemoveMember(botGuid, GROUP_KICK);
    else
        group->RemoveMember(botGuid, GROUP_LEAVE);
    sLog.outString("party dismiss accepted bot:%s guid:%u leader:%u seq:%u method:%s",
                   e->name.c_str(), e->playerGUID, issuer->GetGUIDLow(), e->partySeq,
                   issuerLeads ? "kick" : "leave");
    return true;
}

void PlayerBotMgr::UpdateFollowScript()
{
    if (m_followScript.empty() || m_followScriptIdx >= m_followScript.size())
        return;

    if (m_followScriptStartMs == 0)
    {
        // The clock starts only once every chat-driven issuer is online;
        // earlier, their sessions do not exist yet and the commands would
        // be delivered into nothing.
        for (size_t i = 0; i < m_followScript.size(); ++i)
        {
            FollowScriptEvent const& ev = m_followScript[i];
            if (ev.stale)
                continue;
            std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(ev.issuerGuid);
            if (it == m_bots.end() || it->second->state != PB_STATE_ONLINE)
                return;
        }
        m_followScriptStartMs = WorldTimer::getMSTime();
        if (confDebug)
            sLog.outString("[PlayerBot][FollowScript] started events:%u", (uint32)m_followScript.size());
        return;
    }

    uint32 const now = WorldTimer::getMSTime() - m_followScriptStartMs;
    while (m_followScriptIdx < m_followScript.size() &&
           m_followScript[m_followScriptIdx].delayMs <= now)
    {
        FollowScriptEvent const& ev = m_followScript[m_followScriptIdx];
        if (ev.stale)
        {
            PlayerBotEntry* target = FindBotByName(ev.text);
            if (!target || !target->ai || target->state != PB_STATE_ONLINE)
                sLog.outError("[PlayerBot][FollowScript] stale target %s missing or offline; event skipped", ev.text.c_str());
            else
            {
                if (confDebug)
                    sLog.outString("[PlayerBot][FollowScript] stale delivery at %u bot:%s leader:%u seq:%u",
                                   ev.delayMs, ev.text.c_str(), ev.leaderGuid, ev.seq);
                target->ai->FollowGoal(ev.leaderGuid, ev.seq);
            }
        }
        else
        {
            std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(ev.issuerGuid);
            WorldSession* sess = (it != m_bots.end()) ? it->second->session : nullptr;
            if (!sess)
                sLog.outError("[PlayerBot][FollowScript] issuer %u unavailable; event skipped", ev.issuerGuid);
            else
            {
                std::string msg = std::string(".") + ev.text;
                uint32 lang = LANG_UNIVERSAL;
                uint32 msgType = CHAT_MSG_SAY;
                sess->ProcessChatMessageAfterSecurityCheck(msg, lang, msgType);
                if (confDebug)
                    sLog.outString("[PlayerBot][FollowScript] chat at %u issuer:%u text:%s",
                                   ev.delayMs, ev.issuerGuid, ev.text.c_str());
            }
        }
        ++m_followScriptIdx;
    }
}
// ---------------------------------------------------------------------------
// NEXT-002 (post-MVP): party-invite handling for socketless companion
// sessions.
//
// A bot session never reads world packets, so an SMSG_GROUP_INVITE left in
// its queue sticks forever: the bot can never accept or decline, and every
// later invite fails with "already in a group". HandlePartyInvite is called
// from HandleGroupInviteOpcode right after the invite packet is queued and
// before the inviter's result, and settles the pending invite:
//
//   - owned companion + inviter is the owner: accept, mirroring the
//     client's HandleGroupAcceptOpcode path exactly (remove the invite,
//     create the group when it is new, add the companion as a member,
//     broadcast the group update);
//   - every other invite to a roster bot (intruder, unowned bot): decline,
//     mirroring HandleGroupDeclineOpcode exactly (the leader is fetched
//     first because UninviteFromGroup may delete the group, then
//     SMSG_GROUP_DECLINE is sent to the inviter).
//
// Non-roster players are not touched; every outcome is logged.
// ---------------------------------------------------------------------------
void PlayerBotMgr::HandlePartyInvite(Player* issuer, Player* invitee)
{
    if (!issuer || !invitee)
        return;
    std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(invitee->GetObjectGuid().GetCounter());
    if (it == m_bots.end())
        return; // not a roster bot: normal client behaviour applies
    PlayerBotEntry* const bot = it->second;
    Group* group = invitee->GetGroupInvite();
    if (!group)
        return; // no pending invite to settle

    uint32 const inviterAcc = issuer->GetSession() ? issuer->GetSession()->GetAccountId() : 0;

    if (bot->ownerAccountId && inviterAcc == bot->ownerAccountId)
    {
        // Accept: mirror of WorldSession::HandleGroupAcceptOpcode.
        if (group->GetLeaderGuid() == invitee->GetObjectGuid())
        {
            sLog.outError("party invite ignored self-invite bot:%s guid:%u",
                          bot->name.c_str(), invitee->GetObjectGuid().GetCounter());
            return;
        }
        // remove from invites in any case (same as the client path)
        group->RemoveInvite(invitee);
        if (group->IsFull())
        {
            sLog.outError("party invite declined full-group bot:%s guid:%u leader:%u",
                          bot->name.c_str(), invitee->GetObjectGuid().GetCounter(),
                          group->GetLeaderGuid().GetCounter());
            return;
        }
        if (!group->HandleHardcoreInteraction(invitee))
        {
            sLog.outError("party invite declined hardcore bot:%s guid:%u leader:%u",
                          bot->name.c_str(), invitee->GetObjectGuid().GetCounter(),
                          group->GetLeaderGuid().GetCounter());
            return;
        }
        Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
        // forming a new group, create it (persisted immediately)
        if (!group->IsCreated())
        {
            if (leader)
                group->RemoveInvite(leader);
            if (!group->Create(group->GetLeaderGuid(), group->GetLeaderName()))
            {
                sLog.outError("party invite failed group-create bot:%s guid:%u leader:%u",
                              bot->name.c_str(), invitee->GetObjectGuid().GetCounter(),
                              group->GetLeaderGuid().GetCounter());
                return;
            }
            sObjectMgr.AddGroup(group);
        }
        // the companion's group is set inside AddMember
        if (!group->AddMember(invitee->GetObjectGuid(), invitee->GetName()))
        {
            sLog.outError("party invite failed add-member bot:%s guid:%u leader:%u",
                          bot->name.c_str(), invitee->GetObjectGuid().GetCounter(),
                          group->GetLeaderGuid().GetCounter());
            return;
        }
        group->BroadcastGroupUpdate();
        sLog.outString("party invite accepted bot:%s guid:%u leader:%u group:%u inviter-acc:%u",
                       bot->name.c_str(), invitee->GetObjectGuid().GetCounter(),
                       group->GetLeaderGuid().GetCounter(), group->GetId(), inviterAcc);
        return;
    }

    // Decline: mirror of WorldSession::HandleGroupDeclineOpcode. The leader
    // must be fetched before UninviteFromGroup, which may delete the group.
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    invitee->UninviteFromGroup();
    if (leader && leader->GetSession())
    {
        WorldPacket data(SMSG_GROUP_DECLINE, 10);
        data << invitee->GetName();
        leader->GetSession()->SendPacket(&data);
    }
    sLog.outString("party invite declined not-owner bot:%s guid:%u inviter:%u acc:%u owner:%u",
                   bot->name.c_str(), invitee->GetObjectGuid().GetCounter(),
                   issuer->GetObjectGuid().GetCounter(), inviterAcc, bot->ownerAccountId);
}

void PlayerBotMgr::UpdatePartyInviteScript()
{
    if (m_partyInviteScript.empty() || m_partyInviteScriptIdx >= m_partyInviteScript.size())
        return;

    if (m_partyInviteScriptStartMs == 0)
    {
        // The clock starts only once every inviter is online; earlier, their
        // sessions do not exist yet and the invites would be delivered into
        // nothing.
        for (size_t i = 0; i < m_partyInviteScript.size(); ++i)
        {
            PartyInviteScriptEvent const& ev = m_partyInviteScript[i];
            std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(ev.inviterGuid);
            if (it == m_bots.end() || it->second->state != PB_STATE_ONLINE)
                return;
        }
        m_partyInviteScriptStartMs = WorldTimer::getMSTime();
        if (confDebug)
            sLog.outString("[PlayerBot][PartyInviteScript] started events:%u", (uint32)m_partyInviteScript.size());
        return;
    }

    uint32 const now = WorldTimer::getMSTime() - m_partyInviteScriptStartMs;
    while (m_partyInviteScriptIdx < m_partyInviteScript.size() &&
           m_partyInviteScript[m_partyInviteScriptIdx].delayMs <= now)
    {
        PartyInviteScriptEvent const& ev = m_partyInviteScript[m_partyInviteScriptIdx];
        std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(ev.inviterGuid);
        WorldSession* sess = (it != m_bots.end()) ? it->second->session : nullptr;
        if (!sess)
            sLog.outError("[PlayerBot][PartyInviteScript] inviter %u unavailable; event skipped", ev.inviterGuid);
        else
        {
            // Deliver a real invite through the inviter's session (the same
            // path a client packet takes): the full invite setup runs, the
            // SMSG_GROUP_INVITE is queued for the invitee, and the
            // HandlePartyInvite hook (GroupHandler.cpp) settles it.
            WorldPacket data(CMSG_GROUP_INVITE, 16);
            data << ev.inviteeName;
            sess->HandleGroupInviteOpcode(data);
            if (confDebug)
                sLog.outString("[PlayerBot][PartyInviteScript] invite at %u inviter:%u invitee:%s",
                               ev.delayMs, ev.inviterGuid, ev.inviteeName.c_str());
        }
        ++m_partyInviteScriptIdx;
    }
}

// ---------------------------------------------------------------------------
// PORT-023 (KAP-558): lab-only deterministic owner quest script.
//
// The fixture owner is a temp-logged bot session and cannot drive the
// normal CMSG_QUESTGIVER_* packet flow on its own. Each event
// resolves the issuer and the nearest live quest creature (HasQuest)
// within 30 yd and runs the same authoritative helpers the packet
// handlers use:
//   accept: CanInteractWithQuestGiver + CanTakeQuest + CanAddQuest
//           -> AddQuest(qInfo, creature)
//   turnin: CanCompleteQuest -> CompleteQuest -> CanRewardQuest ->
//           RewardQuest(qInfo, 0, creature, true)
// Lab-only and config-gated (default off); a real owner accepts and
// turns in through the client UI unchanged.
void PlayerBotMgr::UpdateQuestScript()
{
    if (m_questScript.empty() || m_questScriptIdx >= m_questScript.size())
        return;

    if (m_questScriptStartMs == 0)
    {
        // The clock starts only once every issuer is online; earlier,
        // their sessions do not exist yet.
        for (size_t i = 0; i < m_questScript.size(); ++i)
        {
            std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(m_questScript[i].issuerGuid);
            if (it == m_bots.end() || it->second->state != PB_STATE_ONLINE)
                return;
        }
        m_questScriptStartMs = WorldTimer::getMSTime();
        if (confDebug)
            sLog.outString("[PlayerBot][QuestScript] started events:%u", (uint32)m_questScript.size());
        return;
    }

    uint32 const now = WorldTimer::getMSTime() - m_questScriptStartMs;
    while (m_questScriptIdx < m_questScript.size() &&
           m_questScript[m_questScriptIdx].delayMs <= now)
    {
        QuestScriptEvent const& ev = m_questScript[m_questScriptIdx];
        std::map<uint32, PlayerBotEntry*>::const_iterator it = m_bots.find(ev.issuerGuid);
        WorldSession* sess = (it != m_bots.end()) ? it->second->session : nullptr;
        Player* issuer = sess ? sess->GetPlayer() : nullptr;
        if (!issuer || !issuer->IsAlive() || !issuer->GetMap())
        {
            sLog.outError("[PlayerBot][QuestScript] issuer %u unavailable; event skipped", ev.issuerGuid);
            ++m_questScriptIdx;
            continue;
        }
        Quest const* qInfo = sObjectMgr.GetQuestTemplate(ev.questId);
        if (!qInfo)
        {
            sLog.outError("[PlayerBot][QuestScript] quest %u unknown; event skipped", ev.questId);
            ++m_questScriptIdx;
            continue;
        }
        // Nearest live creature with the declared quest within 30 yd
        // (the same search discipline as MVP-006's FindQuestGiver).
        Creature* questNpc = nullptr;
        {
            class QuestScriptNpcCheck
            {
            public:
                QuestScriptNpcCheck(Player const* obj, uint32 questId)
                    : i_obj(obj), i_questId(questId) {}
                WorldObject const& GetFocusObject() const { return *i_obj; }
                bool operator()(Creature const* u)
                {
                    if (!u->IsAlive() || !u->HasQuest(i_questId))
                        return false;
                    return i_obj->IsWithinDistInMap(u, 30.0f);
                }
            private:
                Player const* const i_obj;
                uint32 i_questId;
                QuestScriptNpcCheck(QuestScriptNpcCheck const&);
            };
            QuestScriptNpcCheck check(issuer, ev.questId);
            MaNGOS::CreatureLastSearcher<QuestScriptNpcCheck> searcher(questNpc, check);
            Cell::VisitGridObjects(issuer, searcher, 30.0f);
        }
        if (!questNpc)
        {
            sLog.outError("[PlayerBot][QuestScript] no quest npc for quest %u near issuer %u; event skipped",
                          ev.questId, issuer->GetGUIDLow());
            ++m_questScriptIdx;
            continue;
        }
        if (!ev.turnin)
        {
            // The authoritative accept path (HandleQuestGiverAcceptQuest).
            if (issuer->CanInteractWithQuestGiver(questNpc) &&
                issuer->CanTakeQuest(qInfo, false) &&
                issuer->CanAddQuest(qInfo, false))
            {
                issuer->AddQuest(qInfo, questNpc);
                bool const ok = issuer->GetQuestStatus(ev.questId) != QUEST_STATUS_NONE;
                sLog.outString("[PlayerBot][QuestScript] accept issuer:%u quest:%u giver:%u ok:%u",
                               issuer->GetGUIDLow(), ev.questId, questNpc->GetEntry(), ok ? 1 : 0);
            }
            else
            {
                sLog.outString("[PlayerBot][QuestScript] accept denied issuer:%u quest:%u interact:%u take:%u add:%u",
                               issuer->GetGUIDLow(), ev.questId,
                               issuer->CanInteractWithQuestGiver(questNpc) ? 1 : 0,
                               issuer->CanTakeQuest(qInfo, false) ? 1 : 0,
                               issuer->CanAddQuest(qInfo, false) ? 1 : 0);
            }
        }
        else
        {
            // The authoritative turn-in path (CMSG_QUESTGIVER_REQUEST_REWARD
            // + CMSG_QUESTGIVER_CHOOSE_REWARD, choice index 0). When the
            // last credit lands the engine already marks the quest
            // COMPLETE, so CompleteQuest only runs while still
            // INCOMPLETE, exactly as the handler does; the reward step
            // only needs COMPLETE plus CanRewardQuest.
            uint32 xpBefore = issuer->GetUInt32Value(PLAYER_XP);
            if (issuer->CanCompleteQuest(ev.questId))
                issuer->CompleteQuest(ev.questId);
            if (issuer->GetQuestStatus(ev.questId) == QUEST_STATUS_COMPLETE &&
                issuer->CanRewardQuest(qInfo, false))
            {
                issuer->RewardQuest(qInfo, 0, questNpc, true);
                sLog.outString("[PlayerBot][QuestScript] turnin issuer:%u quest:%u xpBefore:%u xpAfter:%u",
                               issuer->GetGUIDLow(), ev.questId, xpBefore,
                               issuer->GetUInt32Value(PLAYER_XP));
            }
            else
            {
                sLog.outString("[PlayerBot][QuestScript] turnin not-complete issuer:%u quest:%u status:%u",
                               issuer->GetGUIDLow(), ev.questId,
                               (uint32)issuer->GetQuestStatus(ev.questId));
            }
        }
        ++m_questScriptIdx;
    }
}
