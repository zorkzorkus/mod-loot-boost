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
        static char const* const multiplierNames[BUCKET_COUNT] =
        {
            "BossLoot.Multiplier.Dungeon",
            "BossLoot.Multiplier.Raid10",
            "BossLoot.Multiplier.Raid15",
            "BossLoot.Multiplier.Raid20",
            "BossLoot.Multiplier.Raid25",
            "BossLoot.Multiplier.Raid40",
            "BossLoot.Multiplier.Other"
        };

        _bossEnabled = sConfigMgr->GetOption<bool>("BossLoot.Enable", true);
        _logKills = sConfigMgr->GetOption<bool>("BossLoot.Log", true);
        _maxItems = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("BossLoot.MaxItems", 15), 1, MAX_NR_LOOT_ITEMS);

        for (uint8 i = 0; i < BUCKET_COUNT; ++i)
            _multipliers[i] = std::max(0.0f, sConfigMgr->GetOption<float>(multiplierNames[i], 2.0f));

        _emblemMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("BossLoot.Emblems.Multiplier", 5.0f));

        // The emblem list is used while the loot tables are scanned at startup,
        // so changes to it only take effect after a restart.
        if (!_emblemListLoaded)
        {
            std::string const list = sConfigMgr->GetOption<std::string>("BossLoot.Emblems.Items", "29434,40752,40753,45624,47241,49426");
            for (uint32 id : ParseIdList(list))
                _emblemItems.insert(id);
            _emblemListLoaded = true;
        }

        _boeEnabled = sConfigMgr->GetOption<bool>("BoeLoot.Enable", true);
        _boeRareMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("BoeLoot.Rare.Multiplier", 5.0f));
        _boeEpicMultiplier = std::max(0.0f, sConfigMgr->GetOption<float>("BoeLoot.Epic.Multiplier", 5.0f));
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

    void Manager::BuildData()
    {
        uint32 const startTime = getMSTime();

        _creatureBonusRefs.clear();
        _chestBonusRefs.clear();
        _creatureGearGroups.clear();
        _boePools.clear();
        _boeExcludedItems.clear();
        _boeExcludedPools.clear();

        // --- Reference pools ---------------------------------------------
        std::vector<LootRow> const referenceRows = LoadRows("reference_loot_template");

        std::unordered_map<uint32, PoolInfo> pools;
        for (LootRow const& row : referenceRows)
        {
            PoolInfo& pool = pools[row.entry];

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

        auto isBossPool = [&pools](uint32 referenceId) -> bool
        {
            auto itr = pools.find(referenceId);
            if (itr == pools.end())
                return false;

            PoolInfo const& pool = itr->second;
            return !pool.nested && pool.items > 0 && pool.anyRarePlus && !pool.allCurrency;
        };

        uint32 boeRarePools = 0;
        uint32 boeEpicPools = 0;
        for (auto const& [referenceId, pool] : pools)
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

        // --- Conditions (items with conditions are never added by the module) ---
        std::unordered_set<uint64> creatureConditioned;
        std::unordered_set<uint64> chestConditioned;
        if (QueryResult result = WorldDatabase.Query("SELECT SourceTypeOrReferenceId, SourceGroup, SourceEntry FROM conditions WHERE SourceTypeOrReferenceId IN (1, 4)"))
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
            } while (result->NextRow());
        }

        // --- Boss gear references (creatures and chests) --------------------
        auto collectBonusReferences = [&isBossPool](std::vector<LootRow> const& rows,
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
                if (!isBossPool(referenceId))
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

        // --- Plain gear groups (used for bosses only, decided at kill time) ---
        std::unordered_map<uint64, GearGroup> gearGroups;
        for (LootRow const& row : creatureRows)
        {
            if (!row.groupId || row.reference || row.quest)
                continue;

            auto statsItr = creatureStats.find(MakeKey(row.entry, row.groupId));
            if (statsItr == creatureStats.end())
                continue;

            GroupStats const& stats = statsItr->second;
            if (stats.referenceRows || stats.questRows)
                continue; // mixed groups are left to the core

            if (creatureConditioned.count(MakeKey(row.entry, row.item)))
                continue;

            // Skip rows the core itself rejects as invalid
            if (!row.minCount || row.maxCount < row.minCount)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.item);
            if (!proto)
                continue;

            // Items without a chance share whatever the chanced items leave over
            float weight = row.chance;
            if (weight <= 0.0f)
                weight = stats.zeroChanceRows ? std::max(0.0f, 100.0f - stats.explicitChance) / float(stats.zeroChanceRows) : 0.0f;

            GearGroup& group = gearGroups[MakeKey(row.entry, row.groupId)];
            group.groupId = row.groupId;
            group.entries.push_back({ row.item, weight, row.lootMode, row.minCount, row.maxCount, proto->InventoryType != INVTYPE_NON_EQUIP });
        }

        uint32 gearGroupCount = 0;
        for (auto& [key, group] : gearGroups)
        {
            bool const hasGear = std::any_of(group.entries.begin(), group.entries.end(),
                [](GroupEntry const& entry) { return entry.equippable; });
            if (!hasGear)
                continue;

            GroupStats const& stats = creatureStats.at(key);
            float total = stats.explicitChance;
            if (stats.zeroChanceRows)
                total = std::max(total, 100.0f);
            group.totalChance = std::min(total, 100.0f);

            uint32 const lootId = uint32(key >> 32);
            _creatureGearGroups[lootId].push_back(std::move(group));
            ++gearGroupCount;
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

        markCrowded(referenceRows, BuildGroupStats(referenceRows));
        markCrowded(creatureRows, creatureStats);
        markCrowded(chestRows, chestStats);
        for (char const* table : { "item_loot_template", "fishing_loot_template", "pickpocketing_loot_template" })
        {
            std::vector<LootRow> const rows = LoadRows(table);
            markCrowded(rows, BuildGroupStats(rows));
        }

        _dataBuilt = true;

        LOG_INFO("module", "LootBoost: {} boss gear references in {} creature loot tables, {} in {} chest loot tables, {} plain gear groups (used when a boss dies), {} BoE blue pools, {} BoE epic pools, {} BoE entries skipped (crowded groups). Built in {} ms.",
            creatureRefCount, _creatureBonusRefs.size(), chestRefCount, _chestBonusRefs.size(), gearGroupCount,
            boeRarePools, boeEpicPools, _boeExcludedItems.size() + _boeExcludedPools.size(), GetMSTimeDiffToNow(startTime));
    }

    // ------------------------------------------------------------------
    // Boss loot
    // ------------------------------------------------------------------

    void Manager::OnLootGenerated(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner, uint16 lootMode)
    {
        if (!_dataBuilt || !loot || !tab || !lootOwner)
            return;

        if (!_bossEnabled.load(std::memory_order_relaxed))
            return;

        bool const isCreature = &store == &LootTemplates_Creature;
        bool const isChest = &store == &LootTemplates_Gameobject;
        if (!isCreature && !isChest)
            return;

        std::vector<BonusReference> const* refs = nullptr;
        std::vector<GearGroup> const* groups = nullptr;
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
                    auto refItr = _creatureBonusRefs.find(lootId);
                    if (refItr != _creatureBonusRefs.end())
                        refs = &refItr->second;

                    if (creature->IsDungeonBoss() || creature->isWorldBoss())
                    {
                        auto groupItr = _creatureGearGroups.find(lootId);
                        if (groupItr != _creatureGearGroups.end())
                            groups = &groupItr->second;
                    }

                    sourceMap = creature->FindMap();
                    sourceName = creature->GetName();
                    sourceEntry = creature->GetEntry();
                }
            }
        }
        else
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

        if ((refs || groups) && sourceMap)
        {
            SizeBucket const bucket = GetBucket(sourceMap);
            float const multiplier = _multipliers[bucket].load(std::memory_order_relaxed);
            uint32 const maxItems = _maxItems.load(std::memory_order_relaxed);
            std::size_t const before = loot->items.size();

            if (multiplier > 1.0f)
            {
                if (refs)
                    AddBonusPasses(loot, store, lootOwner, lootMode, *refs, multiplier, maxItems);
                if (groups)
                    AddBonusPicks(loot, lootMode, *groups, multiplier, maxItems);
            }

            if (_logKills.load(std::memory_order_relaxed))
                LOG_INFO("module", "LootBoost: {} (entry {}) uses {} x{:.2f}: {} -> {} items",
                    sourceName, sourceEntry, GetBucketName(bucket), multiplier, before, loot->items.size());
        }

        MultiplyEmblems(loot);
    }

    uint32 Manager::AddBonusPasses(Loot* loot, LootStore const& store, Player* lootOwner, uint16 lootMode,
        std::vector<BonusReference> const& refs, float multiplier, uint32 maxItems) const
    {
        uint32 added = 0;

        for (BonusReference const& ref : refs)
        {
            if (!(ref.lootMode & lootMode))
                continue;

            LootTemplate const* pool = LootTemplates_Reference.GetLootFor(ref.referenceId);
            if (!pool)
                continue;

            uint32 const passes = RandomRound(float(ref.maxCount) * (multiplier - 1.0f));
            for (uint32 i = 0; i < passes; ++i)
            {
                if (loot->items.size() >= maxItems)
                    return added;

                std::size_t const itemsBefore = loot->items.size();
                std::size_t const questBefore = loot->quest_items.size();
                uint8 const unlootedBefore = loot->unlootedCount;

                // Same call the core uses for a reference row
                pool->Process(*loot, store, lootMode, lootOwner, 0, false);

                if (loot->items.size() > maxItems)
                {
                    // This pass would overfill the window: undo it, try the next pool
                    RollbackTo(loot, itemsBefore, questBefore, unlootedBefore);
                    break;
                }

                added += uint32(loot->items.size() - itemsBefore);
            }
        }

        return added;
    }

    uint32 Manager::AddBonusPicks(Loot* loot, uint16 lootMode, std::vector<GearGroup> const& groups,
        float multiplier, uint32 maxItems) const
    {
        uint32 added = 0;
        std::vector<GroupEntry const*> candidates;
        std::vector<double> weights;

        for (GearGroup const& group : groups)
        {
            uint32 const picks = RandomRound(multiplier - 1.0f);
            for (uint32 i = 0; i < picks; ++i)
            {
                if (loot->items.size() >= maxItems)
                    return added;

                // A bonus pick keeps the group's normal chance to drop anything
                if (group.totalChance < 100.0f && !roll_chance_f(group.totalChance))
                    continue;

                // Pick among the items that haven't dropped yet, with their
                // odds scaled up, so a pick never comes up empty
                candidates.clear();
                weights.clear();
                for (GroupEntry const& entry : group.entries)
                {
                    if (!(entry.lootMode & lootMode) || entry.weight <= 0.0f)
                        continue;

                    std::size_t const alreadyDropped = std::size_t(std::count_if(loot->items.begin(), loot->items.end(),
                        [&entry](LootItem const& item) { return item.itemid == entry.itemId; }));
                    if (alreadyDropped >= (entry.equippable ? 1u : 3u))
                        continue;

                    candidates.push_back(&entry);
                    weights.push_back(double(entry.weight));
                }

                if (candidates.empty())
                    break; // everything in this group has dropped already

                GroupEntry const* pick = candidates[urandweighted(weights.size(), weights.data())];

                std::size_t const itemsBefore = loot->items.size();
                std::size_t const questBefore = loot->quest_items.size();
                uint8 const unlootedBefore = loot->unlootedCount;

                LootStoreItem storeItem(pick->itemId, 0, 100.0f, false, pick->lootMode, group.groupId, pick->minCount, pick->maxCount);
                loot->AddItem(storeItem);

                if (loot->items.size() > maxItems)
                {
                    RollbackTo(loot, itemsBefore, questBefore, unlootedBefore);
                    return added;
                }

                added += uint32(loot->items.size() - itemsBefore);
            }
        }

        return added;
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
