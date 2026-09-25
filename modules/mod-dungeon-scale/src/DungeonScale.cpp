/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* mod-dungeon-scale: scaling rules, config loading and honor rewards.
*/

#include "DungeonScale.h"

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace DungeonScale
{
    namespace
    {
        char const* const CONFIG_FILE = "DungeonScale.conf";
        std::string const MAP_PREFIX = "DungeonScale.Map.";

        char const* const RANK_NAMES[RANK_COUNT] = { "Normal", "Elite", "MiniBoss", "EndBoss" };
        char const* const CONTENT_NAMES[CONTENT_COUNT] = { "Dungeon", "Raid", "Scenario" };
        char const* const STAT_NAMES[STAT_COUNT] = { "Health", "Damage" };

        // Defaults for DungeonScale.<Content>.<Stat>.<Rank>, used when a key is missing from the conf file.
        float const DEFAULT_SOLO[CONTENT_COUNT][STAT_COUNT][RANK_COUNT] =
        {
            //  Normal Elite  MiniBoss EndBoss
            { { 0.20f, 0.20f, 0.25f, 0.30f },       // Dungeon  Health
              { 0.15f, 0.15f, 0.20f, 0.20f } },     // Dungeon  Damage
            { { 0.08f, 0.08f, 0.10f, 0.12f },       // Raid     Health
              { 0.15f, 0.15f, 0.15f, 0.15f } },     // Raid     Damage
            { { 0.45f, 0.45f, 0.50f, 0.50f },       // Scenario Health
              { 0.50f, 0.50f, 0.50f, 0.50f } },     // Scenario Damage
        };

        std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return value;
        }

        std::string Trim(std::string const& value)
        {
            std::size_t first = value.find_first_not_of(" \t");
            if (first == std::string::npos)
                return "";
            std::size_t last = value.find_last_not_of(" \t");
            return value.substr(first, last - first + 1);
        }

        std::vector<std::string> Split(std::string const& value, char separator)
        {
            std::vector<std::string> parts;
            std::stringstream stream(value);
            std::string part;
            while (std::getline(stream, part, separator))
                parts.push_back(Trim(part));
            return parts;
        }

        bool ParseRank(std::string const& text, Rank& rank)
        {
            std::string const name = ToLower(Trim(text));
            for (uint8 i = 0; i < RANK_COUNT; ++i)
            {
                if (name == ToLower(RANK_NAMES[i]) || name == std::to_string(i))
                {
                    rank = Rank(i);
                    return true;
                }
            }
            return false;
        }

        bool ParseStat(std::string const& text, Stat& stat)
        {
            std::string const name = ToLower(text);
            for (uint8 i = 0; i < STAT_COUNT; ++i)
            {
                if (name == ToLower(STAT_NAMES[i]))
                {
                    stat = Stat(i);
                    return true;
                }
            }
            return false;
        }

        bool ParseMultiplier(std::string const& text, float& value)
        {
            char* end = nullptr;
            value = std::strtof(text.c_str(), &end);
            return end != text.c_str() && value > 0.0f && value <= 10.0f;
        }

        std::string ConfigDirectory()
        {
            std::string const& main = sConfigMgr->GetFilename();
            std::size_t slash = main.find_last_of("/\\");
            return slash == std::string::npos ? std::string() : main.substr(0, slash + 1);
        }

        uint64 StateKey(Map const* map)
        {
            return (uint64(map->GetId()) << 32) | uint64(map->GetInstanceId());
        }

        Content GetContent(Map const* map)
        {
            if (map->IsRaid())
                return Content::Raid;
            if (map->IsScenario())
                return Content::Scenario;
            return Content::Dungeon;
        }
    }

    char const* RankName(Rank rank) { return RANK_NAMES[uint8(rank)]; }
    char const* ContentName(Content content) { return CONTENT_NAMES[uint8(content)]; }

    Manager* Manager::instance()
    {
        static Manager instance;
        return &instance;
    }

    Manager::Manager() : _generation(1)
    {
        _configs.push_back(std::make_unique<ScaleConfig>());         // disabled until the conf file is loaded
        _config.store(_configs.back().get(), std::memory_order_release);
    }

    // ------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------

    void Manager::LoadConfig(bool reload)
    {
        std::string const path = ConfigDirectory() + CONFIG_FILE;
        if (!sConfigMgr->LoadMore(path.c_str()))
        {
            std::string const dist = path + ".dist";
            if (!sConfigMgr->LoadMore(dist.c_str()))
            {
                SF_LOG_ERROR("modules", "[DungeonScale] Could not open %s (or %s). Dungeon scaling is DISABLED.", path.c_str(), dist.c_str());
                _configs.push_back(std::make_unique<ScaleConfig>());
                _config.store(_configs.back().get(), std::memory_order_release);
                ++_generation;
                return;
            }
            SF_LOG_INFO("modules", "[DungeonScale] %s not found, using %s. Copy it to %s to customize.", path.c_str(), dist.c_str(), CONFIG_FILE);
        }

        std::unique_ptr<ScaleConfig> cfg = std::make_unique<ScaleConfig>();

        cfg->enable = sConfigMgr->GetBoolDefault("DungeonScale.Enable", false);
        for (uint8 c = 0; c < CONTENT_COUNT; ++c)
        {
            std::string const key = std::string("DungeonScale.") + CONTENT_NAMES[c] + ".Enable";
            cfg->contentEnabled[c] = sConfigMgr->GetBoolDefault(key.c_str(), Content(c) != Content::Scenario);
        }
        cfg->challengeModeEnabled = sConfigMgr->GetBoolDefault("DungeonScale.ChallengeMode.Enable", false);
        cfg->minPlayers = uint32(std::max(1, sConfigMgr->GetIntDefault("DungeonScale.MinPlayers", 1)));
        cfg->summonsInheritBossScaling = sConfigMgr->GetBoolDefault("DungeonScale.Summons.InheritBossScaling", true);

        for (std::string const& entry : Split(sConfigMgr->GetStringDefault("DungeonScale.IgnoreEntries", ""), ','))
            if (uint32 id = uint32(std::strtoul(entry.c_str(), nullptr, 10)))
                cfg->ignoredEntries.insert(id);

        // DungeonScale.<Content>.<Stat>.<Rank>
        for (uint8 c = 0; c < CONTENT_COUNT; ++c)
        {
            for (uint8 s = 0; s < STAT_COUNT; ++s)
            {
                for (uint8 r = 0; r < RANK_COUNT; ++r)
                {
                    std::string const key = std::string("DungeonScale.") + CONTENT_NAMES[c] + "." + STAT_NAMES[s] + "." + RANK_NAMES[r];
                    float value = DEFAULT_SOLO[c][s][r];
                    std::string const text = sConfigMgr->GetStringDefault(key.c_str(), "");
                    if (!text.empty() && !ParseMultiplier(text, value))
                    {
                        SF_LOG_ERROR("modules", "[DungeonScale] %s = '%s' is not a multiplier in (0, 10]; using %.2f.", key.c_str(), text.c_str(), DEFAULT_SOLO[c][s][r]);
                        value = DEFAULT_SOLO[c][s][r];
                    }
                    cfg->soloValue[c][s][r] = value;
                }
            }
        }

        // DungeonScale.Map.<mapId>.Enable | .<Stat> | .<Stat>.<Rank>
        for (std::string const& key : sConfigMgr->GetKeysByString(MAP_PREFIX))
        {
            if (key.compare(0, MAP_PREFIX.size(), MAP_PREFIX) != 0)
                continue;

            std::vector<std::string> const parts = Split(key.substr(MAP_PREFIX.size()), '.');
            std::string const text = sConfigMgr->GetStringDefault(key.c_str(), "");
            uint32 const mapId = parts.empty() ? 0 : uint32(std::strtoul(parts[0].c_str(), nullptr, 10));
            if (parts.size() < 2 || (!mapId && parts[0] != "0") || text.empty())
            {
                SF_LOG_ERROR("modules", "[DungeonScale] Ignoring malformed override '%s'.", key.c_str());
                continue;
            }

            MapOverride& entry = cfg->mapOverrides[mapId];
            Stat stat;
            Rank rank;
            float value;
            if (parts.size() == 2 && ToLower(parts[1]) == "enable")
                entry.enable = sConfigMgr->GetBoolDefault(key.c_str(), true) ? 1 : 0;
            else if (!ParseStat(parts[1], stat) || (parts.size() == 3 && !ParseRank(parts[2], rank)) || parts.size() > 3)
                SF_LOG_ERROR("modules", "[DungeonScale] Unknown override key '%s' (expected Enable, Health, Damage, Health.<Rank> or Damage.<Rank>).", key.c_str());
            else if (!ParseMultiplier(text, value))
                SF_LOG_ERROR("modules", "[DungeonScale] %s = '%s' is not a multiplier in (0, 10]; ignored.", key.c_str(), text.c_str());
            else if (parts.size() == 2)
                entry.allRanks[uint8(stat)] = value;
            else
                entry.perRank[uint8(stat)][uint8(rank)] = value;
        }

        // DungeonScale.RankOverride = "entry:Rank, entry:Rank"
        for (std::string const& pair : Split(sConfigMgr->GetStringDefault("DungeonScale.RankOverride", ""), ','))
        {
            if (pair.empty())
                continue;
            std::vector<std::string> const fields = Split(pair, ':');
            Rank rank;
            uint32 const entry = fields.size() == 2 ? uint32(std::strtoul(fields[0].c_str(), nullptr, 10)) : 0;
            if (!entry || !ParseRank(fields[1], rank))
            {
                SF_LOG_ERROR("modules", "[DungeonScale] Ignoring malformed RankOverride entry '%s' (expected entry:Normal|Elite|MiniBoss|EndBoss).", pair.c_str());
                continue;
            }
            cfg->rankOverrides[entry] = rank;
        }

        cfg->honorEnable = sConfigMgr->GetBoolDefault("DungeonScale.Honor.Enable", true);
        for (uint8 r = 0; r < RANK_COUNT; ++r)
        {
            std::string const key = std::string("DungeonScale.Honor.") + RANK_NAMES[r];
            cfg->honor[r] = uint32(std::max(0, sConfigMgr->GetIntDefault(key.c_str(), int32(cfg->honor[r]))));
        }
        cfg->honorIgnoreSummons = sConfigMgr->GetBoolDefault("DungeonScale.Honor.IgnoreSummons", true);
        cfg->honorIgnoreGrayMobs = sConfigMgr->GetBoolDefault("DungeonScale.Honor.IgnoreGrayMobs", false);

        SF_LOG_INFO("modules", "[DungeonScale] %s config: %s, %u map override(s), %u rank override(s), honor %s.",
            reload ? "Reloaded" : "Loaded", cfg->enable ? "ENABLED" : "disabled", uint32(cfg->mapOverrides.size()),
            uint32(cfg->rankOverrides.size()), cfg->honorEnable ? "on" : "off");

        // Publish the new snapshot. Old ones are kept so that no reader ever sees freed memory.
        _configs.push_back(std::move(cfg));
        _config.store(_configs.back().get(), std::memory_order_release);
        ++_generation;                                          // forces every instance to re-apply health
    }

    // ------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------

    bool Manager::IsScaledMap(Map const* map) const
    {
        ScaleConfig const& cfg = GetConfig();
        if (!cfg.enable || !map || !map->IsInstance())
            return false;

        auto itr = cfg.mapOverrides.find(map->GetId());
        if (itr != cfg.mapOverrides.end() && itr->second.enable >= 0)
            return itr->second.enable == 1;

        if (map->GetDifficulty() == DIFFICULTY_CHALLENGE && !cfg.challengeModeEnabled)
            return false;

        return cfg.contentEnabled[uint8(GetContent(map))];
    }

    bool Manager::IsScalable(Creature const* creature) const
    {
        if (!creature || creature->IsPet() || creature->IsTotem())
            return false;

        // Player pets, guardians, charmed mobs and vehicles players can ride are left alone.
        if (creature->IsControlledByPlayer() || creature->GetCharmerOrOwnerPlayerOrPlayerItself())
            return false;
        if (creature->IsVehicle() && !creature->IsHostileToPlayers())
            return false;

        if (GetConfig().ignoredEntries.count(creature->GetEntry()))
            return false;

        return IsScaledMap(creature->GetMap());
    }

    bool Manager::IsEndBoss(Creature const* creature) const
    {
        Map const* map = creature->GetMap();
        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), map->GetDifficulty());
        if (!encounters)
            return false;

        for (DungeonEncounter const* encounter : *encounters)
            if (encounter->creditType == EncounterCreditType::ENCOUNTER_CREDIT_KILL_CREATURE
                && encounter->creditEntry == creature->GetEntry() && encounter->lastEncounterDungeon)
                return true;

        return false;
    }

    Rank Manager::GetBaseRank(Creature const* creature) const
    {
        ScaleConfig const& cfg = GetConfig();
        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();

        auto itr = cfg.rankOverrides.find(creature->GetEntry());
        if (itr == cfg.rankOverrides.end())
            itr = cfg.rankOverrides.find(cinfo->Entry);          // difficulty entry
        if (itr != cfg.rankOverrides.end())
            return itr->second;

        if (creature->IsDungeonBoss() || cinfo->rank == CREATURE_ELITE_WORLDBOSS || (cinfo->type_flags & CREATURE_TYPEFLAGS_BOSS))
            return IsEndBoss(creature) ? Rank::EndBoss : Rank::MiniBoss;

        if (cinfo->rank == CREATURE_ELITE_ELITE || cinfo->rank == CREATURE_ELITE_RAREELITE)
            return Rank::Elite;

        return Rank::Normal;
    }

    Rank Manager::GetScalingRank(Creature const* creature) const
    {
        Rank const rank = GetBaseRank(creature);
        if (!GetConfig().summonsInheritBossScaling || !creature->IsSummon())
            return rank;

        // Adds and triggers summoned by a boss are scaled like the boss itself.
        if (Unit* summoner = creature->ToTempSummon()->GetSummoner())
            if (Creature* boss = summoner->ToCreature())
                if (boss != creature && !boss->IsControlledByPlayer())
                {
                    Rank const bossRank = GetBaseRank(boss);
                    if (bossRank >= Rank::MiniBoss && bossRank > rank)
                        return bossRank;
                }

        return rank;
    }

    uint32 Manager::GetMaxPlayers(Map const* map) const
    {
        if (InstanceMap const* instance = map->ToInstanceMap())
            if (uint32 maxPlayers = instance->GetMaxPlayers())
                return maxPlayers;

        switch (GetContent(map))
        {
            case Content::Raid:     return 10;
            case Content::Scenario: return 3;
            default:                return 5;
        }
    }

    uint32 Manager::GetEffectivePlayers(Map const* map) const
    {
        uint32 const maxPlayers = GetMaxPlayers(map);
        uint32 const minPlayers = std::min(GetConfig().minPlayers, maxPlayers);
        return std::max(minPlayers, std::min(map->GetPlayersCountExceptGMs(), maxPlayers));
    }

    float Manager::GetMultiplier(Map const* map, Rank rank, Stat stat, uint32 players) const
    {
        ScaleConfig const& cfg = GetConfig();
        uint8 const s = uint8(stat);
        uint8 const r = uint8(rank);

        float solo = cfg.soloValue[uint8(GetContent(map))][s][r];
        auto itr = cfg.mapOverrides.find(map->GetId());
        if (itr != cfg.mapOverrides.end())
        {
            if (itr->second.perRank[s][r] > 0.0f)
                solo = itr->second.perRank[s][r];
            else if (itr->second.allRanks[s] > 0.0f)
                solo = itr->second.allRanks[s];
        }

        // Straight line from the solo value (1 player) to 1.0 (full group).
        uint32 const maxPlayers = GetMaxPlayers(map);
        if (players >= maxPlayers || maxPlayers <= 1)
            return 1.0f;

        float const progress = float(std::max(players, 1u) - 1) / float(maxPlayers - 1);
        return solo + (1.0f - solo) * progress;
    }

    float Manager::GetMultiplier(Creature const* creature, Stat stat) const
    {
        Map const* map = creature->GetMap();
        return GetMultiplier(map, GetScalingRank(creature), stat, GetEffectivePlayers(map));
    }

    // ------------------------------------------------------------------
    // Health
    // ------------------------------------------------------------------

    void Manager::ApplyHealth(Creature* creature) const
    {
        float const base = float(creature->GetCreateHealth());
        if (base <= 0.0f)
            return;

        float const multiplier = IsScalable(creature) ? GetMultiplier(creature, Stat::Health) : 1.0f;
        float const wanted = std::max(1.0f, std::floor(base * multiplier));
        if (std::fabs(creature->GetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE) - wanted) < 0.5f)
            return;

        float const pct = creature->GetMaxHealth() ? creature->GetHealthPct() : 100.0f;

        // Scale the base value (not max health directly) so health auras keep stacking on top of it.
        creature->SetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE, wanted);
        creature->UpdateMaxHealth();

        if (creature->IsAlive())
        {
            uint32 const maxHealth = creature->GetMaxHealth();
            uint32 const health = uint32(std::ceil(float(maxHealth) * pct / 100.0f));
            creature->SetHealth(std::max(1u, std::min(health, maxHealth)));
        }
    }

    void Manager::RescaleInstance(Map* map, InstanceState& state, uint32 players)
    {
        state.appliedPlayers = players;
        state.appliedGeneration = _generation.load(std::memory_order_relaxed);

        for (Creature* creature : state.creatures)
            ApplyHealth(creature);

        SF_LOG_DEBUG("modules", "[DungeonScale] Map %u instance %u rescaled for %u/%u player(s), %u creature(s).",
            map->GetId(), map->GetInstanceId(), players, GetMaxPlayers(map), uint32(state.creatures.size()));
    }

    // ------------------------------------------------------------------
    // Instance state (one entry per instance; values are owned by that map's update thread)
    // ------------------------------------------------------------------

    InstanceState* Manager::FindState(Map const* map)
    {
        std::lock_guard<std::mutex> guard(_stateLock);
        auto itr = _states.find(StateKey(map));
        return itr == _states.end() ? nullptr : &itr->second;
    }

    InstanceState& Manager::GetOrCreateState(Map const* map)
    {
        std::lock_guard<std::mutex> guard(_stateLock);
        return _states[StateKey(map)];
    }

    void Manager::EraseState(Map const* map)
    {
        std::lock_guard<std::mutex> guard(_stateLock);
        _states.erase(StateKey(map));
    }

    // ------------------------------------------------------------------
    // Hooks
    // ------------------------------------------------------------------

    void Manager::OnCreatureAddWorld(Creature* creature)
    {
        if (!IsScalable(creature))
            return;

        InstanceState& state = GetOrCreateState(creature->GetMap());
        state.creatures.insert(creature);
        ApplyHealth(creature);
    }

    void Manager::OnCreatureRemoveWorld(Creature* creature)
    {
        Map const* map = creature->GetMap();
        if (!map || !map->IsInstance())
            return;

        if (InstanceState* state = FindState(map))
        {
            state->creatures.erase(creature);
            if (state->creatures.empty())
                EraseState(map);
        }
    }

    void Manager::OnCreatureSelectLevel(Creature* creature)
    {
        // Spawning creatures are handled by OnCreatureAddWorld once they are fully set up;
        // this catches respawns and entry changes of creatures already in the world.
        if (!creature->IsInWorld())
            return;

        if (IsScalable(creature))
        {
            GetOrCreateState(creature->GetMap()).creatures.insert(creature);
            ApplyHealth(creature);
        }
    }

    void Manager::RefreshInstance(Map* map)
    {
        if (!map || !map->IsInstance())
            return;

        InstanceState* state = FindState(map);
        if (!state)
            return;

        uint32 const players = GetEffectivePlayers(map);
        if (state->appliedPlayers != players || state->appliedGeneration != _generation.load(std::memory_order_relaxed))
            RescaleInstance(map, *state, players);
    }

    void Manager::RefreshInstance(Unit* first, Unit* second)
    {
        if (Unit* unit = first ? first : second)
            if (unit->IsInWorld())
                RefreshInstance(unit->GetMap());
    }

    uint32 Manager::ScaleDamage(Unit* target, Unit* attacker, uint32 damage)
    {
        // Players joining / leaving are picked up here, before any damage lands.
        RefreshInstance(target, attacker);

        if (!damage || !attacker || attacker->GetTypeId() != TypeID::TYPEID_UNIT)
            return damage;

        Creature* creature = attacker->ToCreature();
        if (!IsScalable(creature))
            return damage;

        return uint32(float(damage) * GetMultiplier(creature, Stat::Damage));
    }

    uint32 Manager::ScaleHeal(Unit* healer, Unit* receiver, uint32 heal)
    {
        RefreshInstance(receiver, healer);

        // Only NPC-on-NPC healing is scaled, so a mob healing a scaled ally does not out-heal the scaling.
        if (!heal || !healer || !receiver || receiver->GetTypeId() != TypeID::TYPEID_UNIT)
            return heal;
        if (healer->GetTypeId() == TypeID::TYPEID_PLAYER || healer->IsControlledByPlayer())
            return heal;

        Creature* target = receiver->ToCreature();
        if (!IsScalable(target))
            return heal;

        return uint32(float(heal) * GetMultiplier(target, Stat::Health));
    }

    // ------------------------------------------------------------------
    // Honor
    // ------------------------------------------------------------------

    void Manager::GrantHonor(Player* player, uint32 amount) const
    {
        CurrencyTypesEntry const* currency = sCurrencyTypesStore.LookupEntry(CURRENCY_TYPE_HONOR_POINTS);
        if (!currency)
            return;

        // Honor may be stored with x100 precision; convert so the player sees exactly `amount`.
        int32 const precision = (currency->Flags & CURRENCY_FLAG_HIGH_PRECISION) ? CURRENCY_PRECISION : 1;
        player->ModifyCurrency(CURRENCY_TYPE_HONOR_POINTS, int32(amount) * precision);
    }

    void Manager::OnCreatureKilled(Creature* killed, Player* rewardedPlayer)
    {
        ScaleConfig const& cfg = GetConfig();
        if (!cfg.enable || !cfg.honorEnable || !rewardedPlayer || !IsScaledMap(killed->GetMap()))
            return;

        if (killed->IsPet() || killed->IsControlledByPlayer() || killed->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;
        if (killed->GetCreatureType() == CREATURE_TYPE_CRITTER)
            return;
        if (cfg.honorIgnoreSummons && killed->IsSummon())
            return;

        Rank const rank = GetBaseRank(killed);
        uint32 const amount = cfg.honor[uint8(rank)];
        if (!amount)
            return;

        auto reward = [&](Player* player)
        {
            if (!player->IsAtGroupRewardDistance(killed))
                return;
            if (cfg.honorIgnoreGrayMobs && killed->getLevel() <= Skyfire::XP::GetGrayLevel(player->getLevel()))
                return;

            GrantHonor(player, amount);
            SF_LOG_DEBUG("modules", "[DungeonScale] %s earned %u honor for killing %s (%u, %s).",
                player->GetName().c_str(), amount, killed->GetName().c_str(), killed->GetEntry(), RankName(rank));
        };

        if (Group* group = rewardedPlayer->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* member = itr->GetSource())
                    reward(member);
        }
        else
            reward(rewardedPlayer);
    }
}
