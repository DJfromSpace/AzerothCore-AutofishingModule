#include "ScriptMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "GameObject.h"
#include "MotionMaster.h"
#include "Spell.h"
#include "LootMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <string>

namespace
{
constexpr uint32 FISHING_SPELL_ID = 7620;
constexpr uint32 FISHING_BOBBER_ENTRY = 35591;
constexpr uint32 FISHING_POLE_ITEM = 6256;

struct AutoFishState
{
    bool enabled = false;
    bool armed = false;
    bool lootOpened = false;
    uint32 nextActionMs = 0;
    uint32 lastSeenBobberMs = 0;
    uint32 lootRetryMs = 0;
    uint32 stuckLogMs = 0;
};

std::unordered_map<ObjectGuid, AutoFishState> s_AutoFishStates;

bool g_AutoFishEnabled = false;
bool g_AutoFishDebug = false;
uint32 g_AutoFishDelayMin = 700;
uint32 g_AutoFishDelayMax = 1600;
std::vector<uint32> g_AutoFishAllowedGuids;

uint32 RollDelay()
{
    if (g_AutoFishDelayMax <= g_AutoFishDelayMin)
        return g_AutoFishDelayMin;
    return urand(g_AutoFishDelayMin, g_AutoFishDelayMax);
}

bool IsAllowed(Player* player)
{
    if (!g_AutoFishEnabled || !player)
        return false;

    if (player->GetSession() == nullptr || player->GetSession()->IsBot())
        return false;

    if (g_AutoFishAllowedGuids.empty())
        return true;

    uint32 guid = player->GetGUID().GetCounter();
    return std::find(g_AutoFishAllowedGuids.begin(), g_AutoFishAllowedGuids.end(), guid) != g_AutoFishAllowedGuids.end();
}

bool HasFishingPole(Player* player)
{
    if (!player)
        return false;

    if (Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
    {
        if (mainHand->GetTemplate() && mainHand->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
            mainHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            return true;
    }

    if (Item* offHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
    {
        if (offHand->GetTemplate() && offHand->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
            offHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            return true;
    }

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            if (item->GetTemplate() && item->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                return true;
        }
    }

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = player->GetBagByPos(bag))
        {
            for (uint32 i = 0; i < pBag->GetBagSize(); ++i)
            {
                if (Item* item = pBag->GetItemByPos(i))
                {
                    if (item->GetTemplate() && item->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                        item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                        return true;
                }
            }
        }
    }

    return false;
}

GameObject* FindChannelFishingBobber(Player* player)
{
    if (!player || !player->GetMap())
        return nullptr;

    ObjectGuid channelGuid = player->GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT);
    if (!channelGuid.IsGameObject())
        return nullptr;

    GameObject* bobber = player->GetMap()->GetGameObject(channelGuid);
    if (!bobber || bobber->GetEntry() != FISHING_BOBBER_ENTRY || bobber->GetOwnerGUID() != player->GetGUID())
        return nullptr;

    return bobber;
}

bool IsFishing(Player* player)
{
    if (!player)
        return false;

    Spell* spell = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (spell && spell->GetSpellInfo() && spell->GetSpellInfo()->Id == FISHING_SPELL_ID)
        return true;

    return player->GetUInt32Value(UNIT_CHANNEL_SPELL) != 0 && FindChannelFishingBobber(player) != nullptr;
}

void LogState(Player* player, char const* msg);

bool AutoLootFishingBobber(Player* player, GameObject* bobber)
{
    if (!player || !bobber)
        return false;

    bobber->Use(player);
    LogState(player, "activated fishing bobber for skill update");
    return true;
}

bool AutoStoreFishingLoot(Player* player)
{
    if (!player || !player->GetSession())
        return false;

    ObjectGuid lootGuid = player->GetLootGUID();
    if (!lootGuid.IsGameObject())
        return false;

    Loot* loot = nullptr;
    if (GameObject* lootGo = player->GetMap()->GetGameObject(lootGuid))
        loot = &lootGo->loot;

    if (!loot)
        return false;

    uint32 maxLootSlot = loot->GetMaxSlotInLootFor(player);
    if (maxLootSlot == 0)
        maxLootSlot = static_cast<uint32>(loot->items.size() + loot->quest_items.size());

    LogState(player, "attempting auto-loot from fishing window");
    LogState(player, (std::string("loot slot count = ") + std::to_string(maxLootSlot)).c_str());

    std::vector<uint8> lootSlots;
    for (uint32 slot = 0; slot < maxLootSlot; ++slot)
    {
        LootItem* lootItem = loot->LootItemInSlot(slot, player);
        if (!lootItem || lootItem->is_looted)
            continue;

        ItemPosCountVec destination;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK)
        {
            LogState(player, (std::string("cannot auto-loot slot; inventory result = ") +
                std::to_string(static_cast<uint32>(msg))).c_str());
            return false;
        }

        lootSlots.push_back(static_cast<uint8>(slot));
    }

    if (lootSlots.empty())
    {
        LogState(player, "loot window has no eligible item slots yet");
        return false;
    }

    for (uint8 lootSlot : lootSlots)
    {
        WorldPacket* lootPacket = new WorldPacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        *lootPacket << lootSlot;
        player->GetSession()->QueuePacket(lootPacket);
        LogState(player, (std::string("queued normal autoloot for slot ") + std::to_string(lootSlot)).c_str());
    }

    WorldPacket* releasePacket = new WorldPacket(CMSG_LOOT_RELEASE, 8);
    *releasePacket << lootGuid;
    player->GetSession()->QueuePacket(releasePacket);
    LogState(player, "queued fishing loot-window release");
    return true;
}

void StopAutoFishing(Player* player, char const* reason)
{
    if (!player)
        return;

    AutoFishState& state = s_AutoFishStates[player->GetGUID()];
    state.armed = false;
    state.lootOpened = false;
    state.nextActionMs = 0;
    state.lastSeenBobberMs = 0;
    state.lootRetryMs = 0;
    state.stuckLogMs = 0;
    LogState(player, reason);
}

GameObject* FindOwnFishingBobber(Player* player)
{
    if (GameObject* bobber = FindChannelFishingBobber(player))
        return bobber;

    GameObject* go = player ? player->GetGameObject(FISHING_SPELL_ID) : nullptr;
    if (go && go->GetEntry() == FISHING_BOBBER_ENTRY && go->GetOwnerGUID() == player->GetGUID())
        return go;
    return nullptr;
}

void LogState(Player* player, char const* msg)
{
    if (!g_AutoFishDebug || !player)
        return;

    LOG_INFO("module", "AutoFishing [{}:{}] {}", player->GetName(), player->GetGUID().GetCounter(), msg);
}
}

class AutoFishingPlayerScript : public PlayerScript
{
public:
    AutoFishingPlayerScript() : PlayerScript("AutoFishingPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_AFTER_UPDATE }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        if (player->GetSession() == nullptr || player->GetSession()->IsBot())
            return;

        AutoFishState& state = s_AutoFishStates[player->GetGUID()];
        state = AutoFishState{};
        state.enabled = g_AutoFishEnabled && IsAllowed(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        if (player->GetSession() == nullptr || player->GetSession()->IsBot())
            return;

        s_AutoFishStates.erase(player->GetGUID());
    }

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        if (!player || !player->IsInWorld() || !IsAllowed(player))
            return;

        AutoFishState& state = s_AutoFishStates[player->GetGUID()];
        if (!state.enabled)
            return;

        if (player->GetSession() == nullptr || !player->GetSession()->PlayerLoading())
        {
            if (player->isDead() || player->IsInCombat() || player->isMoving() || player->IsBeingTeleported() || !player->IsAlive())
            {
                if (state.armed || state.lootOpened)
                    StopAutoFishing(player, "cancelling current fishing cycle because movement/combat/death/teleport");
                return;
            }

            GameObject* bobber = FindOwnFishingBobber(player);
            bool fishing = IsFishing(player);

            if (!state.armed)
            {
                if (!fishing && !bobber)
                    return;

                state.armed = true;
                state.lastSeenBobberMs = 0;
                LogState(player, bobber ? "armed by manual fishing cast and acquired bobber" : "armed by manual fishing cast");
            }

            if (!HasFishingPole(player))
            {
                StopAutoFishing(player, "stopping because no fishing pole is equipped or carried");
                return;
            }

            state.stuckLogMs = 0;

            if (state.nextActionMs > diff)
            {
                state.nextActionMs -= diff;
                return;
            }

            // GameObject::Use opens fishing loot synchronously, but the bobber remains
            // present until the normal loot-release opcode is processed. Handle the
            // pending loot before looking at the bobber again, or this state gets
            // mistaken for an ended cast and auto-loot never runs.
            if (state.lootOpened)
            {
                if (player->GetLootGUID().IsGameObject() && AutoStoreFishingLoot(player))
                {
                    state.lootOpened = false;
                    state.lootRetryMs = 0;
                    state.nextActionMs = RollDelay();
                    state.lastSeenBobberMs = 0;
                    LogState(player, "queued fishing catch for automatic looting");
                    return;
                }

                state.lootRetryMs += 250;
                if (state.lootRetryMs < 3000)
                {
                    state.nextActionMs = 250;
                    LogState(player, "loot window is not ready; retrying auto-loot");
                    return;
                }

                StopAutoFishing(player, "stopping because fishing loot could not be auto-stored");
                return;
            }

            bobber = FindOwnFishingBobber(player);
            fishing = IsFishing(player);

            if (bobber)
            {
                if (state.lastSeenBobberMs == 0)
                {
                    LogState(player, (std::string("tracking fishing bobber; loot state = ") +
                        std::to_string(static_cast<uint32>(bobber->getLootState()))).c_str());
                }

                if (bobber->getLootState() == GO_READY)
                {
                    if (AutoLootFishingBobber(player, bobber))
                        LogState(player, "looted fishing bobber");
                    else
                        LogState(player, "fishing bobber ready but nothing was looted");
                    state.armed = true;
                    state.lootOpened = true;
                    state.nextActionMs = 250;
                    state.lastSeenBobberMs = 0;
                    state.lootRetryMs = 0;
                    return;
                }

                if (!fishing)
                {
                    LogState(player, "bobber exists after fishing ended; waiting for it to despawn");
                    state.lastSeenBobberMs = 0;
                    state.lootOpened = false;
                    state.nextActionMs = 300;
                    return;
                }

                if (state.lastSeenBobberMs == 0)
                    state.lastSeenBobberMs = diff;
                else
                    state.lastSeenBobberMs += diff;

                if (state.lastSeenBobberMs > 15000)
                {
                    LogState(player, "bobber existed too long without becoming ready; resetting cast state");
                    state.nextActionMs = 0;
                    state.lastSeenBobberMs = 0;
                }

                return;
            }

            if (FindOwnFishingBobber(player))
            {
                state.nextActionMs = 300;
                return;
            }

            if (!fishing)
            {
                player->CastSpell(player, FISHING_SPELL_ID, false);
                state.armed = true;
                state.lootOpened = false;
                state.nextActionMs = 1200;
                LogState(player, "cast fishing");
                return;
            }
        }
    }
};

class AutoFishingCommandScript : public CommandScript
{
public:
    AutoFishingCommandScript() : CommandScript("AutoFishingCommandScript") {}

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        static Acore::ChatCommands::ChatCommandTable autofishTable = {
            {"on", HandleAutoFishOn, SEC_PLAYER, Acore::ChatCommands::Console::No},
            {"off", HandleAutoFishOff, SEC_PLAYER, Acore::ChatCommands::Console::No},
            {"status", HandleAutoFishStatus, SEC_PLAYER, Acore::ChatCommands::Console::No},
        };

        static Acore::ChatCommands::ChatCommandTable commandTable = {
            {"autofish", autofishTable},
        };

        return commandTable;
    }

    static bool HandleAutoFishOn(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        s_AutoFishStates[player->GetGUID()].enabled = true;
        s_AutoFishStates[player->GetGUID()].armed = false;
        handler->PSendSysMessage("AutoFishing enabled.");
        return true;
    }

    static bool HandleAutoFishOff(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        s_AutoFishStates[player->GetGUID()].enabled = false;
        s_AutoFishStates[player->GetGUID()].armed = false;
        handler->PSendSysMessage("AutoFishing disabled.");
        return true;
    }

    static bool HandleAutoFishStatus(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        bool enabled = s_AutoFishStates[player->GetGUID()].enabled;
        handler->PSendSysMessage("AutoFishing is {}.", enabled ? "enabled" : "disabled");
        return true;
    }
};

class AutoFishingWorldScript : public WorldScript
{
public:
    AutoFishingWorldScript() : WorldScript("AutoFishingWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_AutoFishEnabled = sConfigMgr->GetOption<bool>("AutoFishing.Enabled", false);
        g_AutoFishDebug = sConfigMgr->GetOption<bool>("AutoFishing.Debug", true);
        g_AutoFishDelayMin = sConfigMgr->GetOption<uint32>("AutoFishing.DelayMsMin", 700);
        g_AutoFishDelayMax = sConfigMgr->GetOption<uint32>("AutoFishing.DelayMsMax", 1600);

        g_AutoFishAllowedGuids.clear();
        std::string allowed = sConfigMgr->GetOption<std::string>("AutoFishing.AllowedGUIDs", "");
        std::stringstream ss(allowed);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (token.empty())
                continue;
            g_AutoFishAllowedGuids.push_back(static_cast<uint32>(std::stoul(token)));
        }

        LOG_INFO("module", "AutoFishing loaded: enabled={}, debug={}, allowed_guids={}", g_AutoFishEnabled, g_AutoFishDebug, g_AutoFishAllowedGuids.size());
    }
};

void Addmod_autofishingScripts()
{
    new AutoFishingWorldScript();
    new AutoFishingPlayerScript();
    new AutoFishingCommandScript();
}
