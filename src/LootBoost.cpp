#include "LootBoost.h"

#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace LootBoost
{
    namespace
    {
        // Groups whose chances add up to at least this much are "real" item
        // groups (for example boss gear). BoE items in such groups are not
        // boosted, because raising one item's chance would push the others out.
        constexpr float CROWDED_GROUP_THRESHOLD = 50.0f;

        // Condition source types (see ConditionMgr.h)
        constexpr int32 CONDITION_SOURCE_CREATURE_LOOT = 1;
        constexpr int32 CONDITION_SOURCE_GAMEOBJECT_LOOT = 4;
        constexpr int32 CONDITION_SOURCE_REFERENCE_LOOT = 10;

        struct LootRow
        {
            uint32 entry = 0;
            uint32 item = 0;
            int32 reference = 0;
            float chance = 0.0f;
            bool quest = false;
            uint16 lootMode = 0;
            uint8 groupId = 0;
            uint8 minCount = 0;
            uint8 maxCount = 0;
        };

        struct GroupStats
        {
            uint32 rows = 0;
            uint32 referenceRows = 0;
            uint32 questRows = 0;
            uint32 zeroChanceRows = 0;
            float explicitChance = 0.0f;
        };

        struct PoolInfo
        {
            uint32 items = 0;
            bool nested = false;
            bool anyRarePlus = false;
            bool allCurrency = true;
            bool allBoeRare = true;
            bool allBoeEpic = true;
        };

        uint64 MakeKey(uint32 entry, uint32 second)
        {
            return (uint64(entry) << 32) | second;
        }

        std::vector<LootRow> LoadRows(std::string const& table)
        {
            std::vector<LootRow> rows;

            QueryResult result = WorldDatabase.Query("SELECT Entry, Item, Reference, Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount FROM " + table);
            if (!result)
                return rows;

            rows.reserve(result->GetRowCount());
            do
            {
                Field* fields = result->Fetch();

                LootRow row;
                row.entry = fields[0].Get<uint32>();
                row.item = fields[1].Get<uint32>();
                row.reference = fields[2].Get<int32>();
                row.chance = fields[3].Get<float>();
                row.quest = fields[4].Get<int8>() != 0;
                row.lootMode = fields[5].Get<uint16>();
                row.groupId = fields[6].Get<uint8>();
                row.minCount = fields[7].Get<uint8>();
                row.maxCount = fields[8].Get<uint8>();
                rows.push_back(row);
            } while (result->NextRow());

            return rows;
        }

        std::unordered_map<uint64, GroupStats> BuildGroupStats(std::vector<LootRow> const& rows)
        {
            std::unordered_map<uint64, GroupStats> stats;
            for (LootRow const& row : rows)
            {
                if (!row.groupId)
                    continue;

                GroupStats& group = stats[MakeKey(row.entry, row.groupId)];
                ++group.rows;
                if (row.reference)
                    ++group.referenceRows;
                if (row.quest)
                    ++group.questRows;
                if (row.chance > 0.0f)
                    group.explicitChance += row.chance;
                else
                    ++group.zeroChanceRows;
            }
            return stats;
        }

        // Effective drop chance of a grouped row. Rows without a chance share
        // whatever the chanced rows of the group leave over, like in the core.
        float GroupedWeight(LootRow const& row, GroupStats const& stats)
        {
            if (row.chance > 0.0f)
                return row.chance;

            if (!stats.zeroChanceRows)
                return 0.0f;

            return std::max(0.0f, 100.0f - stats.explicitChance) / float(stats.zeroChanceRows);
        }

        // Chance in percent that a group drops anything at all.
        float GroupTotalChance(GroupStats const& stats)
        {
            float total = stats.explicitChance;
            if (stats.zeroChanceRows)
                total = std::max(total, 100.0f);
            return std::min(total, 100.0f);
        }

        // Rows the core itself rejects as invalid
        bool HasValidCounts(LootRow const& row)
        {
            return row.minCount && row.maxCount >= row.minCount;
        }

        PickEntry MakeEntry(LootRow const& row, float weight)
        {
            return { row.item, weight, row.lootMode, row.minCount, row.maxCount };
        }

        std::vector<uint32> ParseIdList(std::string const& text)
        {
            std::vector<uint32> ids;
            uint64 current = 0;
            bool inNumber = false;

            for (char c : text)
            {
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    current = current * 10 + uint64(c - '0');
                    inNumber = true;
                    if (current > 0xFFFFFFFFull)
                        current = 0xFFFFFFFFull;
                }
                else if (inNumber)
                {
                    ids.push_back(uint32(current));
                    current = 0;
                    inNumber = false;
                }
            }

            if (inNumber)
                ids.push_back(uint32(current));

            return ids;
        }

        void RollbackTo(Loot* loot, std::size_t itemCount, std::size_t questCount, uint8 unlootedCount)
        {
            while (loot->items.size() > itemCount)
                loot->items.pop_back();
            while (loot->quest_items.size() > questCount)
                loot->quest_items.pop_back();
            loot->unlootedCount = unlootedCount;
        }
    }

    Manager* Manager::instance()
    {
        static Manager instance;
        return &instance;
    }

    // ------------------------------------------------------------------
    // Config
    // ------------------------------------------------------------------

    void Manager::LoadConfig(bool reload)
    {
        static char const* const bucketNames[BUCKET_COUNT] =
        {
            "Dungeon", "Raid10", "Raid15", "Raid20", "Raid25", "Raid40", "Other"
        };

        // Every default here must match the "Default:" line of the option in
        // conf/mod_loot_boost.conf.dist. All defaults leave loot unchanged,
        // so a missing option never changes anything.

        _bossEnabled = sConfigMgr->GetOption<bool>("BossLoot.Enable", false);
        _logKills = sConfigMgr->GetOption<bool>("BossLoot.Log", false);
        _maxItems = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("BossLoot.MaxItems", 15), 1, MAX_NR_LOOT_ITEMS);

        for (uint8 i = 0; i < BUCKET_COUNT; ++i)
        {
            std::string const name = bucketNames[i];
            _multipliers[i] = std::max(0.0f, sConfigMgr->GetOption<float>("BossLoot.Multiplier." + name, 1.0f));
            _duplicateWeights[i] = std::clamp(sConfigMgr->GetOption<float>("BossLoot.Duplicates.Weight." + name, 0.0f), 0.0f, 1.0f);
        }

        _emblemMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("BossLoot.Emblems.Multiplier", 1.0f));

        // The emblem list is used while the loot tables are scanned at startup,
        // so changes to it only take effect after a restart.
        if (!_emblemListLoaded)
        {
            std::string const list = sConfigMgr->GetOption<std::string>("BossLoot.Emblems.Items", "29434,40752,40753,45624,47241,49426");
            for (uint32 id : ParseIdList(list))
                _emblemItems.insert(id);
            _emblemListLoaded = true;
        }

        _rareEnabled = sConfigMgr->GetOption<bool>("RareLoot.Enable", false);
        _rareDungeonMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("RareLoot.Multiplier.Dungeon", 1.0f));
        _rareWorldMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("RareLoot.Multiplier.World", 1.0f));

        _boeEnabled = sConfigMgr->GetOption<bool>("BoeLoot.Enable", false);
        _boeRareMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("BoeLoot.Rare.Multiplier", 1.0f));
        _boeEpicMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("BoeLoot.Epic.Multiplier", 1.0f));
        _boeMaxChance = std::clamp(sConfigMgr->GetOption<float>("BoeLoot.MaxChance", 5.0f), 0.0f, 100.0f);

        if (reload)
            LOG_INFO("module", "LootBoost: config reloaded (BossLoot.Emblems.Items changes need a restart).");
    }

    // ------------------------------------------------------------------
    // Startup scan
    // ------------------------------------------------------------------

    bool Manager::IsEmblem(uint32 itemId) const
    {
        return _emblemItems.find(itemId) != _emblemItems.end();
    }

    // Items worth multiplying: anything equippable, plus epic items that
    // can't be equipped, like tier tokens. Money, emblems and quest items
    // never count.
    bool Manager::IsBossLoot(ItemTemplate const* proto) const
    {
        if (!proto)
            return false;

        if (proto->Class == ITEM_CLASS_MONEY || proto->Class == ITEM_CLASS_QUEST || IsEmblem(proto->ItemId))
            return false;

        return proto->InventoryType != INVTYPE_NON_EQUIP || proto->Quality >= ITEM_QUALITY_EPIC;
    }

    void Manager::BuildData()
    {
        uint32 const startTime = getMSTime();

        _pools.clear();
        _creatureBonusRefs.clear();
        _chestBonusRefs.clear();
        _creatureGroups.clear();
        _creatureSingles.clear();
        _boePools.clear();
        _boeExcludedItems.clear();
        _boeExcludedPools.clear();

        // --- Conditions (items with conditions are never added by the module) ---
        std::unordered_set<uint64> creatureConditioned;
        std::unordered_set<uint64> chestConditioned;
        std::unordered_set<uint64> referenceConditioned;
        if (QueryResult result = WorldDatabase.Query("SELECT SourceTypeOrReferenceId, SourceGroup, SourceEntry FROM conditions WHERE SourceTypeOrReferenceId IN (1, 4, 10)"))
        {
            do
            {
                Field* fields = result->Fetch();
                int32 const sourceType = fields[0].Get<int32>();
                uint32 const lootId = fields[1].Get<uint32>();
                uint32 const item = uint32(fields[2].Get<int32>());

                if (sourceType == CONDITION_SOURCE_CREATURE_LOOT)
                    creatureConditioned.insert(MakeKey(lootId, item));
                else if (sourceType == CONDITION_SOURCE_GAMEOBJECT_LOOT)
                    chestConditioned.insert(MakeKey(lootId, item));
                else if (sourceType == CONDITION_SOURCE_REFERENCE_LOOT)
                    referenceConditioned.insert(MakeKey(lootId, item));
            } while (result->NextRow());
        }

        // --- Reference pools ---------------------------------------------
        std::vector<LootRow> const referenceRows = LoadRows("reference_loot_template");
        std::unordered_map<uint64, GroupStats> const referenceStats = BuildGroupStats(referenceRows);

        std::unordered_map<uint32, PoolInfo> poolInfos;
        for (LootRow const& row : referenceRows)
        {
            PoolInfo& pool = poolInfos[row.entry];

            if (row.reference)
            {
                pool.nested = true;
                continue;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.item);
            if (!proto)
                continue;

            ++pool.items;

            if (proto->Quality >= ITEM_QUALITY_RARE)
                pool.anyRarePlus = true;

            if (proto->Class != ITEM_CLASS_MONEY && !IsEmblem(row.item))
                pool.allCurrency = false;

            bool const boe = proto->Bonding == BIND_WHEN_EQUIPPED;
            if (!boe || proto->Quality != ITEM_QUALITY_RARE)
                pool.allBoeRare = false;
            if (!boe || proto->Quality != ITEM_QUALITY_EPIC)
                pool.allBoeEpic = false;
        }

        auto isBossPool = [&poolInfos](uint32 referenceId) -> bool
        {
            auto itr = poolInfos.find(referenceId);
            if (itr == poolInfos.end())
                return false;

            PoolInfo const& pool = itr->second;
            return !pool.nested && pool.items > 0 && pool.anyRarePlus && !pool.allCurrency;
        };

        uint32 boeRarePools = 0;
        uint32 boeEpicPools = 0;
        for (auto const& [referenceId, pool] : poolInfos)
        {
            if (pool.nested || !pool.items)
                continue;

            if (pool.allBoeRare)
            {
                _boePools[referenceId] = ITEM_QUALITY_RARE;
                ++boeRarePools;
            }
            else if (pool.allBoeEpic)
            {
                _boePools[referenceId] = ITEM_QUALITY_EPIC;
                ++boeEpicPools;
            }
        }

        // --- Boss gear pools, rolled by the module itself ------------------
        // The whole pool is copied, like the core rolls it for a reference row.
        // Quest rows, rows with conditions and groups mixed with quest rows
        // are left out.
        std::unordered_map<uint64, PickGroup> poolGroups;
        for (LootRow const& row : referenceRows)
        {
            if (row.reference || row.quest || !isBossPool(row.entry))
                continue;

            if (referenceConditioned.count(MakeKey(row.entry, row.item)) || !HasValidCounts(row))
                continue;

            if (!sObjectMgr->GetItemTemplate(row.item))
                continue;

            if (!row.groupId)
            {
                if (row.chance > 0.0f)
                    _pools[row.entry].singles.push_back(MakeEntry(row, row.chance));
                continue;
            }

            GroupStats const& stats = referenceStats.at(MakeKey(row.entry, row.groupId));
            if (stats.questRows)
                continue;

            PickGroup& group = poolGroups[MakeKey(row.entry, row.groupId)];
            group.groupId = row.groupId;
            group.totalChance = GroupTotalChance(stats);
            group.entries.push_back(MakeEntry(row, GroupedWeight(row, stats)));
        }

        for (auto& [key, group] : poolGroups)
            _pools[uint32(key >> 32)].groups.push_back(std::move(group));

        // --- Boss gear references (creatures and chests) --------------------
        auto collectBonusReferences = [this](std::vector<LootRow> const& rows,
            std::unordered_map<uint64, GroupStats> const& stats,
            std::unordered_set<uint64> const& conditioned,
            std::unordered_map<uint32, std::vector<BonusReference>>& out) -> uint32
        {
            uint32 count = 0;
            for (LootRow const& row : rows)
            {
                // Guaranteed reference rows only
                if (!row.reference || row.quest || row.chance < 100.0f || !row.maxCount)
                    continue;

                // A grouped reference only counts if it is alone in its group
                if (row.groupId)
                {
                    auto itr = stats.find(MakeKey(row.entry, row.groupId));
                    if (itr == stats.end() || itr->second.rows != 1)
                        continue;
                }

                if (conditioned.count(MakeKey(row.entry, row.item)))
                    continue;

                uint32 const referenceId = uint32(std::abs(row.reference));
                if (!_pools.count(referenceId))
                    continue;

                out[row.entry].push_back({ referenceId, row.maxCount, row.lootMode });
                ++count;
            }
            return count;
        };

        std::vector<LootRow> const creatureRows = LoadRows("creature_loot_template");
        std::unordered_map<uint64, GroupStats> const creatureStats = BuildGroupStats(creatureRows);
        uint32 const creatureRefCount = collectBonusReferences(creatureRows, creatureStats, creatureConditioned, _creatureBonusRefs);

        std::vector<LootRow> const chestRows = LoadRows("gameobject_loot_template");
        std::unordered_map<uint64, GroupStats> const chestStats = BuildGroupStats(chestRows);
        uint32 const chestRefCount = collectBonusReferences(chestRows, chestStats, chestConditioned, _chestBonusRefs);

        // --- Plain loot groups and single items (decided at kill time) -------
        // Groups: used for bosses and rares. Single items: used for rares only.
        std::unordered_map<uint64, PickGroup> creatureGroups;
        std::unordered_set<uint64> creatureGroupsWithLoot;
        uint32 singleCount = 0;
        for (LootRow const& row : creatureRows)
        {
            if (row.reference || row.quest)
                continue;

            if (creatureConditioned.count(MakeKey(row.entry, row.item)) || !HasValidCounts(row))
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.item);
            if (!proto)
                continue;

            if (!row.groupId)
            {
                // Ungrouped: green or better gear, or epic tokens
                if (row.chance > 0.0f && IsBossLoot(proto) && proto->Quality >= ITEM_QUALITY_UNCOMMON)
                {
                    _creatureSingles[row.entry].push_back(MakeEntry(row, row.chance));
                    ++singleCount;
                }
                continue;
            }

            uint64 const key = MakeKey(row.entry, row.groupId);
            GroupStats const& stats = creatureStats.at(key);
            if (stats.referenceRows || stats.questRows)
                continue; // mixed groups are left to the core

            PickGroup& group = creatureGroups[key];
            group.groupId = row.groupId;
            group.totalChance = GroupTotalChance(stats);
            group.entries.push_back(MakeEntry(row, GroupedWeight(row, stats)));

            if (IsBossLoot(proto))
                creatureGroupsWithLoot.insert(key);
        }

        uint32 groupCount = 0;
        for (auto& [key, group] : creatureGroups)
        {
            // Only groups with gear or tokens in them
            if (!creatureGroupsWithLoot.count(key))
                continue;

            _creatureGroups[uint32(key >> 32)].push_back(std::move(group));
            ++groupCount;
        }

        // --- BoE guard: skip BoEs that sit in crowded groups ----------------
        auto markCrowded = [this](std::vector<LootRow> const& rows, std::unordered_map<uint64, GroupStats> const& stats)
        {
            for (LootRow const& row : rows)
            {
                if (!row.groupId || row.chance <= 0.0f)
                    continue;

                auto itr = stats.find(MakeKey(row.entry, row.groupId));
                if (itr == stats.end() || itr->second.explicitChance < CROWDED_GROUP_THRESHOLD)
                    continue;

                if (row.reference)
                {
                    uint32 const referenceId = uint32(std::abs(row.reference));
                    if (_boePools.count(referenceId))
                        _boeExcludedPools.insert(referenceId);
                }
                else if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.item))
                {
                    if (proto->Bonding == BIND_WHEN_EQUIPPED && (proto->Quality == ITEM_QUALITY_RARE || proto->Quality == ITEM_QUALITY_EPIC))
                        _boeExcludedItems.insert(row.item);
                }
            }
        };

        markCrowded(referenceRows, referenceStats);
        markCrowded(creatureRows, creatureStats);
        markCrowded(chestRows, chestStats);
        for (char const* table : { "item_loot_template", "fishing_loot_template", "pickpocketing_loot_template" })
        {
            std::vector<LootRow> const rows = LoadRows(table);
            markCrowded(rows, BuildGroupStats(rows));
        }

        _dataBuilt = true;

        LOG_INFO("module", "LootBoost: {} boss gear pools, {} references to them in {} creature loot tables, {} in {} chest loot tables, {} loot groups and {} single items (used for bosses and rares), {} BoE blue pools, {} BoE epic pools, {} BoE entries skipped (crowded groups). Built in {} ms.",
            _pools.size(), creatureRefCount, _creatureBonusRefs.size(), chestRefCount, _chestBonusRefs.size(), groupCount, singleCount,
            boeRarePools, boeEpicPools, _boeExcludedItems.size() + _boeExcludedPools.size(), GetMSTimeDiffToNow(startTime));
    }

    // ------------------------------------------------------------------
    // Boss and rare loot
    // ------------------------------------------------------------------

    bool Manager::IsRare(Creature const* creature)
    {
        uint32 const rank = creature->GetCreatureTemplate()->rank;
        return rank == CREATURE_ELITE_RARE || rank == CREATURE_ELITE_RAREELITE;
    }

    void Manager::OnLootGenerated(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner, uint16 lootMode)
    {
        if (!_dataBuilt || !loot || !tab || !lootOwner)
            return;

        bool const bossEnabled = _bossEnabled.load(std::memory_order_relaxed);
        bool const rareEnabled = _rareEnabled.load(std::memory_order_relaxed);
        if (!bossEnabled && !rareEnabled)
            return;

        bool const isCreature = &store == &LootTemplates_Creature;
        bool const isChest = &store == &LootTemplates_Gameobject;
        if (!isCreature && !isChest)
            return;

        SourceKind kind = SOURCE_BOSS;
        std::vector<BonusReference> const* refs = nullptr;
        std::vector<PickGroup> const* groups = nullptr;
        std::vector<PickEntry> const* singles = nullptr;
        Map const* sourceMap = nullptr;
        std::string sourceName;
        uint32 sourceEntry = 0;

        if (isCreature)
        {
            // The looter is always on the same map as the corpse
            Map* map = lootOwner->FindMap();
            Creature* creature = map ? map->GetCreature(loot->sourceWorldObjectGUID) : nullptr;
            if (creature)
            {
                uint32 const lootId = creature->GetCreatureTemplate()->lootid;
                if (lootId && store.GetLootFor(lootId) == tab)
                {
                    bool const boss = creature->IsDungeonBoss() || creature->isWorldBoss();
                    bool const rare = !boss && IsRare(creature);
                    if (rare)
                        kind = SOURCE_RARE;

                    // Bosses and rares get their groups boosted, rares also their
                    // single items. Boss gear pools are boosted on any creature.
                    if (rare ? rareEnabled : bossEnabled)
                    {
                        auto refItr = _creatureBonusRefs.find(lootId);
                        if (refItr != _creatureBonusRefs.end())
                            refs = &refItr->second;

                        if (boss || rare)
                        {
                            auto groupItr = _creatureGroups.find(lootId);
                            if (groupItr != _creatureGroups.end())
                                groups = &groupItr->second;
                        }

                        if (rare)
                        {
                            auto singleItr = _creatureSingles.find(lootId);
                            if (singleItr != _creatureSingles.end())
                                singles = &singleItr->second;
                        }
                    }

                    sourceMap = creature->FindMap();
                    sourceName = creature->GetName();
                    sourceEntry = creature->GetEntry();
                }
            }
        }
        else if (bossEnabled)
        {
            GameObject* chest = loot->sourceGameObject;
            if (!chest)
            {
                Map* map = lootOwner->FindMap();
                chest = map ? map->GetGameObject(loot->sourceWorldObjectGUID) : nullptr;
            }

            if (chest)
            {
                uint32 const lootId = chest->GetGOInfo()->GetLootId();
                if (lootId && store.GetLootFor(lootId) == tab)
                {
                    auto refItr = _chestBonusRefs.find(lootId);
                    if (refItr != _chestBonusRefs.end())
                        refs = &refItr->second;

                    sourceMap = chest->FindMap();
                    sourceName = chest->GetName();
                    sourceEntry = chest->GetEntry();
                }
            }
        }

        if ((refs || groups || singles) && sourceMap)
        {
            SizeBucket const bucket = GetBucket(sourceMap);
            float multiplier = _multipliers[bucket].load(std::memory_order_relaxed);
            if (kind == SOURCE_RARE)
                multiplier = bucket == BUCKET_DUNGEON
                    ? _rareDungeonMultiplier.load(std::memory_order_relaxed)
                    : _rareWorldMultiplier.load(std::memory_order_relaxed);

            float const duplicateWeight = _duplicateWeights[bucket].load(std::memory_order_relaxed);
            uint32 const maxItems = _maxItems.load(std::memory_order_relaxed);
            std::size_t const before = loot->items.size();

            if (multiplier > 1.0f)
            {
                bool room = true;
                if (refs)
                    room = AddBonusPasses(loot, lootMode, *refs, multiplier, duplicateWeight, maxItems);
                if (room && groups)
                    room = AddBonusPicks(loot, lootMode, *groups, multiplier, duplicateWeight, maxItems);
                if (room && singles)
                    AddBonusRolls(loot, lootMode, *singles, multiplier, duplicateWeight, maxItems);
            }

            if (_logKills.load(std::memory_order_relaxed))
                LOG_INFO("module", "LootBoost: {} (entry {}, {}) uses {} x{:.2f}, duplicates x{:.2f}: {} -> {} items",
                    sourceName, sourceEntry, kind == SOURCE_RARE ? "rare" : "boss", GetBucketName(bucket),
                    multiplier, duplicateWeight, before, loot->items.size());
        }

        if (bossEnabled)
            MultiplyEmblems(loot);
    }

    bool Manager::AddBonusPasses(Loot* loot, uint16 lootMode, std::vector<BonusReference> const& refs,
        float multiplier, float duplicateWeight, uint32 maxItems) const
    {
        for (BonusReference const& ref : refs)
        {
            if (!(ref.lootMode & lootMode))
                continue;

            auto itr = _pools.find(ref.referenceId);
            if (itr == _pools.end())
                continue;

            uint32 const passes = RandomRound(float(ref.maxCount) * (multiplier - 1.0f));
            for (uint32 i = 0; i < passes; ++i)
                if (!RollTable(loot, lootMode, itr->second, duplicateWeight, maxItems))
                    return false;
        }

        return true;
    }

    bool Manager::AddBonusPicks(Loot* loot, uint16 lootMode, std::vector<PickGroup> const& groups,
        float multiplier, float duplicateWeight, uint32 maxItems) const
    {
        for (PickGroup const& group : groups)
        {
            uint32 const picks = RandomRound(multiplier - 1.0f);
            for (uint32 i = 0; i < picks; ++i)
                if (!PickFromGroup(loot, lootMode, group, duplicateWeight, maxItems))
                    return false;
        }

        return true;
    }

    bool Manager::AddBonusRolls(Loot* loot, uint16 lootMode, std::vector<PickEntry> const& singles,
        float multiplier, float duplicateWeight, uint32 maxItems) const
    {
        for (PickEntry const& entry : singles)
        {
            uint32 const rolls = RandomRound(multiplier - 1.0f);
            for (uint32 i = 0; i < rolls; ++i)
                if (!RollSingle(loot, lootMode, entry, duplicateWeight, maxItems))
                    return false;
        }

        return true;
    }

    // One pass over a pool: every single item rolls, every group picks once.
    bool Manager::RollTable(Loot* loot, uint16 lootMode, PickTable const& table, float duplicateWeight, uint32 maxItems) const
    {
        for (PickEntry const& entry : table.singles)
            if (!RollSingle(loot, lootMode, entry, duplicateWeight, maxItems))
                return false;

        for (PickGroup const& group : table.groups)
            if (!PickFromGroup(loot, lootMode, group, duplicateWeight, maxItems))
                return false;

        return true;
    }

    bool Manager::RollSingle(Loot* loot, uint16 lootMode, PickEntry const& entry, float duplicateWeight, uint32 maxItems) const
    {
        if (loot->items.size() >= maxItems)
            return false;

        if (!(entry.lootMode & lootMode))
            return true;

        float const chance = entry.weight * DuplicateFactor(loot, entry.itemId, duplicateWeight);
        if (chance <= 0.0f || !roll_chance_f(chance))
            return true;

        return AddPicked(loot, entry, 0, maxItems);
    }

    bool Manager::PickFromGroup(Loot* loot, uint16 lootMode, PickGroup const& group, float duplicateWeight, uint32 maxItems) const
    {
        if (loot->items.size() >= maxItems)
            return false;

        // A bonus pick keeps the group's normal chance to drop anything
        if (group.totalChance < 100.0f && !roll_chance_f(group.totalChance))
            return true;

        // Items that already dropped stay in the pick, at a reduced weight
        std::vector<PickEntry const*> candidates;
        std::vector<double> weights;
        candidates.reserve(group.entries.size());
        weights.reserve(group.entries.size());

        for (PickEntry const& entry : group.entries)
        {
            if (!(entry.lootMode & lootMode))
                continue;

            double const weight = double(entry.weight) * DuplicateFactor(loot, entry.itemId, duplicateWeight);
            if (weight <= 0.0)
                continue;

            candidates.push_back(&entry);
            weights.push_back(weight);
        }

        if (candidates.empty())
            return true; // with a duplicate weight of 0: everything has dropped already

        PickEntry const* pick = candidates[urandweighted(weights.size(), weights.data())];
        return AddPicked(loot, *pick, group.groupId, maxItems);
    }

    bool Manager::AddPicked(Loot* loot, PickEntry const& entry, uint8 groupId, uint32 maxItems) const
    {
        std::size_t const itemsBefore = loot->items.size();
        std::size_t const questBefore = loot->quest_items.size();
        uint8 const unlootedBefore = loot->unlootedCount;

        // Each added item rolls its own random stats, like any other drop
        LootStoreItem storeItem(entry.itemId, 0, 100.0f, false, entry.lootMode, groupId, entry.minCount, entry.maxCount);
        loot->AddItem(storeItem);

        if (loot->items.size() > maxItems)
        {
            // This item would overfill the window: undo it and stop
            RollbackTo(loot, itemsBefore, questBefore, unlootedBefore);
            return false;
        }

        return true;
    }

    // Each copy of an item already in the window multiplies its chance
    // by the duplicate weight once more.
    float Manager::DuplicateFactor(Loot const* loot, uint32 itemId, float duplicateWeight)
    {
        uint32 const copies = uint32(std::count_if(loot->items.begin(), loot->items.end(),
            [itemId](LootItem const& item) { return item.itemid == itemId; }));

        if (!copies)
            return 1.0f;

        return std::pow(duplicateWeight, float(copies));
    }

    void Manager::MultiplyEmblems(Loot* loot) const
    {
        float const multiplier = _emblemMultiplier.load(std::memory_order_relaxed);
        if (multiplier == 1.0f || _emblemItems.empty())
            return;

        for (LootItem& item : loot->items)
        {
            if (!IsEmblem(item.itemid))
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
            if (!proto)
                continue;

            uint32 const maxCount = std::min<uint32>(255, std::max<uint32>(1, proto->GetMaxStackSize()));
            uint32 const count = std::clamp<uint32>(RandomRound(float(item.count) * multiplier), 1, maxCount);
            item.count = uint8(count);
        }
    }

    SizeBucket Manager::GetBucket(Map const* map)
    {
        if (!map || map->IsBattlegroundOrArena())
            return BUCKET_OTHER;

        InstanceMap const* instance = map->ToInstanceMap();
        if (!instance)
            return BUCKET_OTHER; // open world

        if (!map->IsRaid())
            return BUCKET_DUNGEON;

        switch (instance->GetMaxPlayers())
        {
            case 10: return BUCKET_RAID10;
            case 15: return BUCKET_RAID15;
            case 20: return BUCKET_RAID20;
            case 25: return BUCKET_RAID25;
            case 40: return BUCKET_RAID40;
            default: return BUCKET_OTHER;
        }
    }

    char const* Manager::GetBucketName(SizeBucket bucket)
    {
        switch (bucket)
        {
            case BUCKET_DUNGEON: return "Dungeon";
            case BUCKET_RAID10:  return "Raid10";
            case BUCKET_RAID15:  return "Raid15";
            case BUCKET_RAID20:  return "Raid20";
            case BUCKET_RAID25:  return "Raid25";
            case BUCKET_RAID40:  return "Raid40";
            default:             return "Other";
        }
    }

    uint32 Manager::RandomRound(float value)
    {
        if (value <= 0.0f)
            return 0;

        float const whole = std::floor(value);
        uint32 result = uint32(whole);
        if (rand_norm() < double(value - whole))
            ++result;
        return result;
    }

    // ------------------------------------------------------------------
    // BoE odds
    // ------------------------------------------------------------------

    bool Manager::IsBoostedStore(LootStore const& store) const
    {
        return &store == &LootTemplates_Creature
            || &store == &LootTemplates_Gameobject
            || &store == &LootTemplates_Item
            || &store == &LootTemplates_Fishing
            || &store == &LootTemplates_Pickpocketing;
    }

    void Manager::AdjustRollChance(LootStoreItem const* item, float& chance, LootStore const& store) const
    {
        if (!_dataBuilt || !item || item->needs_quest)
            return;

        if (!_boeEnabled.load(std::memory_order_relaxed))
            return;

        // Only rare rolls: anything at or above the cap is left alone
        float const cap = _boeMaxChance.load(std::memory_order_relaxed);
        if (chance <= 0.0f || chance >= cap)
            return;

        if (!IsBoostedStore(store))
            return;

        uint32 quality = 0;
        if (item->reference)
        {
            uint32 const referenceId = uint32(std::abs(item->reference));
            auto itr = _boePools.find(referenceId);
            if (itr == _boePools.end() || _boeExcludedPools.count(referenceId))
                return;
            quality = itr->second;
        }
        else
        {
            if (_boeExcludedItems.count(item->itemid))
                return;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
            if (!proto || proto->Bonding != BIND_WHEN_EQUIPPED)
                return;
            if (proto->Quality != ITEM_QUALITY_RARE && proto->Quality != ITEM_QUALITY_EPIC)
                return;
            quality = proto->Quality;
        }

        float const multiplier = quality == ITEM_QUALITY_EPIC
            ? _boeEpicMultiplier.load(std::memory_order_relaxed)
            : _boeRareMultiplier.load(std::memory_order_relaxed);

        if (multiplier == 1.0f)
            return;

        float boosted = chance * multiplier;
        if (multiplier > 1.0f)
            boosted = std::min(boosted, cap); // boosted items stay below the cap

        chance = boosted;
    }
}
