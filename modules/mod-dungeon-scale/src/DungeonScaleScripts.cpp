/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* mod-dungeon-scale: script hooks and GM commands.
*/

#include "DungeonScale.h"

#include "Chat.h"
#include "Creature.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace DungeonScale;

class DungeonScale_WorldScript : public WorldScript
{
public:
    DungeonScale_WorldScript() : WorldScript("DungeonScale_WorldScript") { }

    void OnConfigLoad(bool reload) OVERRIDE
    {
        sDungeonScale->LoadConfig(reload);
    }
};

class DungeonScale_AllCreatureScript : public AllCreatureScript
{
public:
    DungeonScale_AllCreatureScript() : AllCreatureScript("DungeonScale_AllCreatureScript") { }

    void OnCreatureAddWorld(Creature* creature) OVERRIDE
    {
        sDungeonScale->OnCreatureAddWorld(creature);
    }

    void OnCreatureRemoveWorld(Creature* creature) OVERRIDE
    {
        sDungeonScale->OnCreatureRemoveWorld(creature);
    }

    void OnCreatureSelectLevel(Creature* creature) OVERRIDE
    {
        sDungeonScale->OnCreatureSelectLevel(creature);
    }

    void OnCreatureKilled(Creature* killed, Player* rewardedPlayer) OVERRIDE
    {
        sDungeonScale->OnCreatureKilled(killed, rewardedPlayer);
    }
};

class DungeonScale_UnitScript : public UnitScript
{
public:
    DungeonScale_UnitScript() : UnitScript("DungeonScale_UnitScript") { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) OVERRIDE
    {
        damage = sDungeonScale->ScaleDamage(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage) OVERRIDE
    {
        if (damage > 0)
            damage = int32(sDungeonScale->ScaleDamage(target, attacker, uint32(damage)));
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) OVERRIDE
    {
        damage = sDungeonScale->ScaleDamage(target, attacker, damage);
    }

    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) OVERRIDE
    {
        gain = sDungeonScale->ScaleHeal(healer, receiver, gain);
    }
};

class DungeonScale_CommandScript : public CommandScript
{
public:
    DungeonScale_CommandScript() : CommandScript("DungeonScale_CommandScript") { }

    std::vector<ChatCommand> GetCommands() const OVERRIDE
    {
        static std::vector<ChatCommand> dungeonScaleCommandTable =
        {
            { "info",     rbac::RBAC_PERM_COMMAND_INSTANCE_STATS, false, &HandleInfoCommand,     "" },
            { "creature", rbac::RBAC_PERM_COMMAND_INSTANCE_STATS, false, &HandleCreatureCommand, "" },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "dungeonscale", rbac::RBAC_PERM_COMMAND_INSTANCE_STATS, false, NULL, "", dungeonScaleCommandTable },
        };
        return commandTable;
    }

    // .dungeonscale info - scaling state of the instance you are in
    static bool HandleInfoCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Map* map = player->GetMap();
        ScaleConfig const& cfg = sDungeonScale->GetConfig();
        sDungeonScale->RefreshInstance(map);

        handler->PSendSysMessage("[DungeonScale] Module: %s | Honor: %s", cfg.enable ? "ENABLED" : "disabled", cfg.honorEnable ? "on" : "off");
        handler->PSendSysMessage("Map %u (%s), difficulty %u, instance %u", map->GetId(), map->GetMapName(), uint32(map->GetDifficulty()), map->GetInstanceId());

        if (!sDungeonScale->IsScaledMap(map))
        {
            handler->PSendSysMessage("This map is NOT scaled (not an instance, content type disabled, challenge mode, or DungeonScale.Map.%u.Enable = 0).", map->GetId());
            return true;
        }

        uint32 const players = sDungeonScale->GetEffectivePlayers(map);
        handler->PSendSysMessage("Scaling for %u of %u player(s) (%u non-GM player(s) inside).", players, sDungeonScale->GetMaxPlayers(map), map->GetPlayersCountExceptGMs());
        for (uint8 r = 0; r < RANK_COUNT; ++r)
        {
            Rank const rank = Rank(r);
            handler->PSendSysMessage("  %-8s health x%.3f  damage x%.3f  honor %u", RankName(rank),
                sDungeonScale->GetMultiplier(map, rank, Stat::Health, players),
                sDungeonScale->GetMultiplier(map, rank, Stat::Damage, players),
                cfg.honor[r]);
        }
        return true;
    }

    // .dungeonscale creature - how the selected creature is classified and scaled
    static bool HandleCreatureCommand(ChatHandler* handler, char const* /*args*/)
    {
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->SendSysMessage("[DungeonScale] Select a creature first.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        sDungeonScale->RefreshInstance(creature->GetMap());
        handler->PSendSysMessage("[DungeonScale] %s (entry %u, template rank %u)", creature->GetName().c_str(), creature->GetEntry(), creature->GetCreatureTemplate()->rank);
        if (!sDungeonScale->IsScalable(creature))
        {
            handler->SendSysMessage("Not scaled (player-owned, friendly vehicle, ignored entry, or map not scaled).");
            return true;
        }

        handler->PSendSysMessage("Rank: %s (scaled as %s)", RankName(sDungeonScale->GetBaseRank(creature)), RankName(sDungeonScale->GetScalingRank(creature)));
        handler->PSendSysMessage("Health x%.3f: %u / %u (unscaled base %u)", sDungeonScale->GetMultiplier(creature, Stat::Health),
            creature->GetHealth(), creature->GetMaxHealth(), creature->GetCreateHealth());
        handler->PSendSysMessage("Damage x%.3f", sDungeonScale->GetMultiplier(creature, Stat::Damage));
        return true;
    }
};

void AddSC_mod_dungeon_scale()
{
    new DungeonScale_WorldScript();
    new DungeonScale_AllCreatureScript();
    new DungeonScale_UnitScript();
    new DungeonScale_CommandScript();
}
