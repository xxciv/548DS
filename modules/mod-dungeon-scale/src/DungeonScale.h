/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* mod-dungeon-scale: scales dungeon / raid creatures to the number of players
* inside the instance, and awards honor for dungeon kills based on mob rank.
*/

#ifndef MOD_DUNGEON_SCALE_H
#define MOD_DUNGEON_SCALE_H

#include "Define.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Map;
class Player;
class Unit;

namespace DungeonScale
{
    // How a creature is treated for scaling and honor purposes.
    enum class Rank : uint8
    {
        Normal   = 0,
        Elite    = 1,
        MiniBoss = 2,
        EndBoss  = 3,
    };
    constexpr std::size_t RANK_COUNT = 4;

    // Which family of multipliers an instance uses.
    enum class Content : uint8
    {
        Dungeon  = 0,
        Raid     = 1,
        Scenario = 2,
    };
    constexpr std::size_t CONTENT_COUNT = 3;

    enum class Stat : uint8
    {
        Health = 0,
        Damage = 1,
    };
    constexpr std::size_t STAT_COUNT = 2;

    char const* RankName(Rank rank);
    char const* ContentName(Content content);

    // Per-map overrides from DungeonScale.Map.<mapId>.* keys. A negative value means "not set".
    struct MapOverride
    {
        int8 enable = -1;
        std::array<float, STAT_COUNT> allRanks = { { -1.0f, -1.0f } };
        std::array<std::array<float, RANK_COUNT>, STAT_COUNT> perRank = { { { { -1.0f, -1.0f, -1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f, -1.0f } } } };
    };

    // Immutable snapshot of DungeonScale.conf. A new one is built on every (re)load.
    struct ScaleConfig
    {
        bool enable = false;
        std::array<bool, CONTENT_COUNT> contentEnabled = { { true, true, false } };
        bool challengeModeEnabled = false;
        uint32 minPlayers = 1;
        bool summonsInheritBossScaling = true;
        std::unordered_set<uint32> ignoredEntries;

        // soloValue[content][stat][rank] = multiplier when the instance holds MinPlayers players.
        std::array<std::array<std::array<float, RANK_COUNT>, STAT_COUNT>, CONTENT_COUNT> soloValue{};

        std::unordered_map<uint32, MapOverride> mapOverrides;
        std::unordered_map<uint32, Rank> rankOverrides;

        bool honorEnable = false;
        std::array<uint32, RANK_COUNT> honor = { { 1, 5, 10, 25 } };
        bool honorIgnoreSummons = true;
        bool honorIgnoreGrayMobs = false;
    };

    // Everything the module knows about one instance. Only ever touched by the thread updating that map.
    struct InstanceState
    {
        std::unordered_set<Creature*> creatures;
        uint32 appliedPlayers = 0;
        uint32 appliedGeneration = 0;
    };

    class Manager
    {
    public:
        static Manager* instance();

        void LoadConfig(bool reload);
        ScaleConfig const& GetConfig() const { return *_config.load(std::memory_order_acquire); }

        // Instance / creature queries
        bool IsScaledMap(Map const* map) const;
        bool IsScalable(Creature const* creature) const;
        Rank GetBaseRank(Creature const* creature) const;       // rank used for honor
        Rank GetScalingRank(Creature const* creature) const;    // rank used for stats (summons may inherit)
        uint32 GetMaxPlayers(Map const* map) const;
        uint32 GetEffectivePlayers(Map const* map) const;
        float GetMultiplier(Map const* map, Rank rank, Stat stat, uint32 players) const;
        float GetMultiplier(Creature const* creature, Stat stat) const;

        // Hooks
        void OnCreatureAddWorld(Creature* creature);
        void OnCreatureRemoveWorld(Creature* creature);
        void OnCreatureSelectLevel(Creature* creature);
        void OnCreatureKilled(Creature* killed, Player* rewardedPlayer);
        uint32 ScaleDamage(Unit* target, Unit* attacker, uint32 damage);
        uint32 ScaleHeal(Unit* healer, Unit* receiver, uint32 heal);

        // Rescales the instance if its player count (or the config) changed since the last check.
        // Must run on the thread updating that map; called from the damage / heal hooks and GM commands.
        void RefreshInstance(Map* map);
        void RefreshInstance(Unit* first, Unit* second);

        // Applies (or re-applies) health scaling to one creature, keeping its health percentage.
        void ApplyHealth(Creature* creature) const;

    private:
        Manager();

        bool IsEndBoss(Creature const* creature) const;
        void RescaleInstance(Map* map, InstanceState& state, uint32 players);
        void GrantHonor(Player* player, uint32 amount) const;

        InstanceState* FindState(Map const* map);
        InstanceState& GetOrCreateState(Map const* map);
        void EraseState(Map const* map);

        std::atomic<ScaleConfig const*> _config;
        std::vector<std::unique_ptr<ScaleConfig>> _configs;          // old snapshots stay alive: readers never lock
        std::atomic<uint32> _generation;

        std::mutex _stateLock;                                  // guards the _states container, not its values
        std::unordered_map<uint64, InstanceState> _states;
    };
}

#define sDungeonScale DungeonScale::Manager::instance()

#endif
