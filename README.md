# 548DS: Dungeon Scale for SkyFire 5.4.8

This module for [Project SkyFire](https://github.com/ProjectSkyfire/SkyFire_548) (WoW 5.4.8) scales dungeon, raid and scenario creatures to the number of players in the instance. It also awards honor for dungeon kills based on the mob's rank. It's a leaner take on AzerothCore's [mod-dungeon-scale](https://github.com/NathanHandley/mod-dungeon-scale).

## What it does

| Feature | Details |
|---|---|
| **Scales every creature in an instance** | Health and damage (melee, spells, DoTs) of all non-player creatures, including trash, bosses, adds and triggers. Player pets, totems, guardians and vehicles that players can ride are left alone. |
| **Linear, predictable curve** | You set the multiplier for **1 player**. It rises in a straight line to **1.0 at a full group**, so a full group always gets Blizzlike difficulty. |
| **Per-rank tuning** | Separate values for `Normal`, `Elite`, `MiniBoss` and `EndBoss`, for dungeons, raids and scenarios. |
| **Per-dungeon overrides** | `DungeonScale.Map.<mapId>.*` replaces any global value for one map, or turns the map off entirely. |
| **Live rescaling** | When players join or leave, every creature is rescaled and keeps its current health %. |
| **Honor per kill** | Normal = 1, Elite = 5, MiniBoss = 10, EndBoss = 25 (configurable). Each group member in range gets the full amount. |
| **GM tools** | `.dungeonscale info` and `.dungeonscale creature` show exactly what's being applied. |
| **Config file** | `DungeonScale.conf`, loaded at startup and on `.reload config`. |

## Repository layout

```
core-patches/
  0001-Scripting-add-AllCreatureScript-hooks.patch   # 4 small, additive core hooks (67 lines)
modules/
  mod-dungeon-scale/
    conf/DungeonScale.conf.dist                       # documented config, staged next to worldserver
    src/DungeonScale.h / .cpp                         # scaling rules, config, honor
    src/DungeonScaleScripts.cpp                       # hook wiring + GM commands
    src/mod_dungeon_scale_loader.cpp                  # module entry point
```

## Installation

SkyFire master now has a module system (`modules/`, compiled in automatically), so the module itself needs no core edits. It does need **four creature hooks** that SkyFire doesn't have yet. Those come in one small patch.

```bash
cd /path/to/SkyFire_548

# 1. Apply the core hooks (additive only; no existing behaviour changes)
git apply /path/to/548DS/core-patches/0001-Scripting-add-AllCreatureScript-hooks.patch

# 2. Drop the module in. The folder MUST be named mod-dungeon-scale:
#    the loader function name is derived from it.
cp -r /path/to/548DS/modules/mod-dungeon-scale modules/

# 3. Re-run CMake so the module is discovered, then build as usual
cd build && cmake .. && make -j$(nproc) && make install
```

CMake prints `+ module: mod-dungeon-scale` when it picks the module up. After install:

```bash
cd /path/to/server/etc                 # wherever worldserver.conf lives
cp DungeonScale.conf.dist DungeonScale.conf
```

If `DungeonScale.conf` is missing, the module falls back to `DungeonScale.conf.dist` and logs a note. If neither file exists, the module stays disabled and logs an error.

## Tuning workflow

1. Enter the dungeon and run `.dungeonscale info` to see the map id, player count and the multipliers for each rank.
2. Target a mob and run `.dungeonscale creature` to see its rank, its health and damage multipliers, and its unscaled health.
3. If a dungeon is still too hard solo, add overrides for that map only:
   ```ini
   DungeonScale.Map.961.Health = 0.25          # all ranks
   DungeonScale.Map.961.Damage.EndBoss = 0.20  # just the last boss
   ```
4. Restart the server (or run `.reload config`). Creatures are rescaled the next time a player in the instance updates.

A GM with GM mode **on** isn't counted as a player, so the instance scales as if one player were inside.

## How it works

**Multiplier:**
`solo + (1 - solo) × (players - 1) / (maxPlayers - 1)`
Here `maxPlayers` is the instance size from the DBC (5, 10, 25 or 40), and `players` is the number of non-GM players, clamped to `[MinPlayers, maxPlayers]`.

**Rank:**
1. `DungeonScale.RankOverride`, if the entry is listed there.
2. A boss is any creature with the dungeon-boss flag (from `instance_encounters`), rank 3, or the boss type flag. If its encounter is marked `lastEncounterDungeon`, it's an **EndBoss**; otherwise it's a **MiniBoss**.
3. Elite and rare-elite creatures are **Elite**.
4. Everything else is **Normal**.
5. Creatures summoned by a boss are scaled like the boss (but give no honor by default).

**Health:** The module scales the creature's `UNIT_MOD_HEALTH` base value rather than setting max health directly. Health auras still stack correctly, and the unscaled value (`GetCreateHealth()`) is kept, so rescaling never drifts.

**Damage:** Scaled in the existing `UnitScript` hooks (`ModifyMeleeDamage`, `ModifySpellDamageTaken`, `ModifyPeriodicDamageAurasTick`) whenever the attacker is a scaled creature. NPC-on-NPC healing is scaled by the target's health multiplier, so mob healers don't undo the scaling.

**Honor:** Paid out through `ModifyCurrency(Honor Points)`, correcting for the currency's ×100 precision, so players see exactly the configured number. The honor weekly and total caps still apply.

### Core hooks added by the patch

| Hook | Called from | Used for |
|---|---|---|
| `AllCreatureScript::OnCreatureAddWorld` | `Creature::AddToWorld()` | Scale newly spawned or summoned creatures |
| `AllCreatureScript::OnCreatureRemoveWorld` | `Creature::RemoveFromWorld()` | Stop tracking despawned creatures |
| `AllCreatureScript::OnCreatureSelectLevel` | end of `Creature::SelectLevel()` | Re-apply scaling after a respawn or entry change |
| `AllCreatureScript::OnCreatureKilled` | `Unit::Kill()`, after kill rewards | Honor (also fires for pet or totem kills) |

The hook names follow AzerothCore's `AllCreatureScript`, which makes it easier to port other AC modules later.

## Known limitations

- **Flat-value mechanics aren't scaled.** Examples: boss absorb shields ("absorbs 2,000,000 damage") and "deal X damage to break free" checks. Percent-based mechanics are fine.
- **Scripts that call `SetMaxHealth()` directly** on a creature are overridden when the instance rescales. Add those entries to `DungeonScale.IgnoreEntries`.
- **End-boss detection depends on the world DB.** It needs `instance_encounters.lastEncounterDungeon`. If a dungeon's end boss shows up as `MiniBoss`, fix it with `DungeonScale.RankOverride`.
- **Enabling the module with `.reload config`** only affects creatures that spawn afterwards. A restart applies it everywhere. Changing values with a reload works immediately.
- **Level scaling isn't done.** Creature levels are unchanged, as are XP, gold and loot.
