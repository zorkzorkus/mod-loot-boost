/*
 * mod-loot-boost
 *
 * More loot from bosses and rare creatures (scaled per instance size),
 * multiplied emblems, and better odds for BoE blues and epics everywhere.
 *
 * Released under the GNU AGPL v3 license, like AzerothCore itself.
 */

#ifndef MOD_LOOT_BOOST_H
#define MOD_LOOT_BOOST_H

#include "Define.h"

#include <array>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class LootStore;
class LootTemplate;
class Map;
class Player;
struct ItemTemplate;
struct Loot;
struct LootStoreItem;

namespace LootBoost
{
    // Which multiplier and duplicate weight apply, based on where the loot drops.
    enum SizeBucket : uint8
    {
        BUCKET_DUNGEON = 0,
        BUCKET_RAID10,
        BUCKET_RAID15,
        BUCKET_RAID20,
        BUCKET_RAID25,
        BUCKET_RAID40,
        BUCKET_OTHER,
        BUCKET_COUNT
    };

    // What kind of loot source gets boosted.
    enum SourceKind : uint8
    {
        SOURCE_BOSS = 0,    // dungeon boss, world boss, boss chest, or any creature with a boss gear pool
        SOURCE_RARE         // rare or rare elite creature
    };

    // A guaranteed reference row that points to a boss gear pool.
    // Every bonus pass rolls the pool once more.
    struct BonusReference
    {
        uint32 referenceId;
        uint8 maxCount;
        uint16 lootMode;
    };

    // One item the module can add on its own.
    struct PickEntry
    {
        uint32 itemId;
        float weight;   // grouped: effective drop chance in percent; ungrouped: roll chance in percent
        uint16 lootMode;
        uint8 minCount;
        uint8 maxCount;
    };

    // A loot group: drops at most one of its items per pick.
    struct PickGroup
    {
        uint8 groupId;
        float totalChance; // chance in percent that the group drops anything at all
        std::vector<PickEntry> entries;
    };

    // A loot table the module rolls itself, the same way the core rolls one:
    // every ungrouped item rolls its own chance, every group picks once.
    struct PickTable
    {
        std::vector<PickEntry> singles;
        std::vector<PickGroup> groups;
    };

    class Manager
    {
    public:
        static Manager* instance();

        // Called on startup and on ".reload config".
        void LoadConfig(bool reload);

        // Called once at startup, after the world database is loaded.
        void BuildData();

        // Called after the core generated a loot window.
        void OnLootGenerated(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner, uint16 lootMode);

        // Called for every chance roll. May change the chance.
        void AdjustRollChance(LootStoreItem const* item, float& chance, LootStore const& store) const;

    private:
        Manager() = default;

        // Bonus logic. All of these return false once the loot window is full.
        bool AddBonusPasses(Loot* loot, uint16 lootMode, std::vector<BonusReference> const& refs,
            float multiplier, float duplicateWeight, uint32 maxItems) const;
        bool AddBonusPicks(Loot* loot, uint16 lootMode, std::vector<PickGroup> const& groups,
            float multiplier, float duplicateWeight, uint32 maxItems) const;
        bool AddBonusRolls(Loot* loot, uint16 lootMode, std::vector<PickEntry> const& singles,
            float multiplier, float duplicateWeight, uint32 maxItems) const;
        bool RollTable(Loot* loot, uint16 lootMode, PickTable const& table, float duplicateWeight, uint32 maxItems) const;
        bool RollSingle(Loot* loot, uint16 lootMode, PickEntry const& entry, float duplicateWeight, uint32 maxItems) const;
        bool PickFromGroup(Loot* loot, uint16 lootMode, PickGroup const& group, float duplicateWeight, uint32 maxItems) const;
        bool AddPicked(Loot* loot, PickEntry const& entry, uint8 groupId, uint32 maxItems) const;
        void MultiplyEmblems(Loot* loot) const;

        static float DuplicateFactor(Loot const* loot, uint32 itemId, float duplicateWeight);
        static SizeBucket GetBucket(Map const* map);
        static char const* GetBucketName(SizeBucket bucket);
        static bool IsRare(Creature const* creature);
        static uint32 RandomRound(float value);

        // Startup data helpers
        bool IsEmblem(uint32 itemId) const;
        bool IsBossLoot(ItemTemplate const* proto) const;
        bool IsBoostedStore(LootStore const& store) const;

        // Config (atomics, because loot is generated in parallel map threads)
        // The real defaults are set in LoadConfig.
        std::atomic<bool> _bossEnabled{false};
        std::atomic<bool> _logKills{false};
        std::atomic<uint32> _maxItems{15};
        std::array<std::atomic<float>, BUCKET_COUNT> _multipliers{};
        std::array<std::atomic<float>, BUCKET_COUNT> _duplicateWeights{};
        std::atomic<float> _emblemMultiplier{1.0f};
        std::atomic<bool> _rareEnabled{false};
        std::atomic<float> _rareDungeonMultiplier{1.0f};
        std::atomic<float> _rareWorldMultiplier{1.0f};
        std::atomic<bool> _boeEnabled{false};
        std::atomic<float> _boeRareMultiplier{1.0f};
        std::atomic<float> _boeEpicMultiplier{1.0f};
        std::atomic<float> _boeMaxChance{5.0f};

        // Read only after startup
        std::unordered_set<uint32> _emblemItems;
        bool _emblemListLoaded = false;

        std::unordered_map<uint32, PickTable> _pools;                               // boss gear pools by reference id
        std::unordered_map<uint32, std::vector<BonusReference>> _creatureBonusRefs; // by creature loot id
        std::unordered_map<uint32, std::vector<BonusReference>> _chestBonusRefs;    // by chest loot id
        std::unordered_map<uint32, std::vector<PickGroup>> _creatureGroups;         // by creature loot id
        std::unordered_map<uint32, std::vector<PickEntry>> _creatureSingles;        // by creature loot id (rares only)

        std::unordered_map<uint32, uint32> _boePools;       // reference id -> item quality (3 or 4)
        std::unordered_set<uint32> _boeExcludedItems;       // BoE items that sit in crowded groups
        std::unordered_set<uint32> _boeExcludedPools;       // BoE pools that sit in crowded groups
        bool _dataBuilt = false;
    };
}

#define sLootBoost LootBoost::Manager::instance()

#endif
