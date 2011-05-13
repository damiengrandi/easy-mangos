/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "DBCStores.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Log.h"
#include "World.h"
#include "Opcodes.h"
#include "Spell.h"
#include "ScriptMgr.h"
#include "Totem.h"
#include "SpellAuras.h"

#include <G3D/Vector3.h>

using G3D::Vector3;

void WorldSession::HandleUseItemOpcode(WorldPacket& recvPacket)
{
    uint8 bagIndex, slot;
    uint8 unk_flags;                                        // flags (if 0x02 - some additional data are received)
    uint8 cast_count;                                       // next cast if exists (single or not)
    ObjectGuid itemGuid;
    uint32 glyphIndex;                                      // something to do with glyphs?
    uint32 spellid;                                         // casted spell id

    recvPacket >> bagIndex >> slot >> cast_count >> spellid >> itemGuid >> glyphIndex >> unk_flags;

    // TODO: add targets.read() check
    Player* pUser = _player;

    // ignore for remote control state
    if (!pUser->IsSelfMover())
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        return;
    }

    // reject fake data
    if (glyphIndex >= MAX_GLYPH_SLOT_INDEX)
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, NULL, NULL );
        return;
    }

    Item *pItem = pUser->GetItemByPos(bagIndex, slot);
    if (!pItem)
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, NULL, NULL );
        return;
    }

    if (pItem->GetObjectGuid() != itemGuid)
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, NULL, NULL );
        return;
    }

    DETAIL_LOG("WORLD: CMSG_USE_ITEM packet, bagIndex: %u, slot: %u, cast_count: %u, spellid: %u, Item: %u, glyphIndex: %u, unk_flags: %u, data length = %i", bagIndex, slot, cast_count, spellid, pItem->GetEntry(), glyphIndex, unk_flags, (uint32)recvPacket.size());

    ItemPrototype const *proto = pItem->GetProto();
    if (!proto)
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, NULL );
        return;
    }

    // some item classes can be used only in equipped state
    if (proto->InventoryType != INVTYPE_NON_EQUIP && !pItem->IsEquipped())
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, NULL );
        return;
    }

    InventoryResult msg = pUser->CanUseItem(pItem);
    if (msg != EQUIP_ERR_OK)
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError( msg, pItem, NULL );
        return;
    }

    // not allow use item from trade (cheat way only)
    if (pItem->IsInTrade())
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, NULL );
        return;
    }

    // only allow conjured consumable, bandage, poisons (all should have the 2^21 item flag set in DB)
    if (proto->Class == ITEM_CLASS_CONSUMABLE &&
        !(proto->Flags & ITEM_FLAG_USEABLE_IN_ARENA) &&
        pUser->InArena())
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at not read packet tail
        pUser->SendEquipError(EQUIP_ERR_NOT_DURING_ARENA_MATCH,pItem,NULL);
        return;
    }

    if (pUser->isInCombat())
    {
        for(int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            if (SpellEntry const *spellInfo = sSpellStore.LookupEntry(proto->Spells[i].SpellId))
            {
                if (IsNonCombatSpell(spellInfo))
                {
                    recvPacket.rpos(recvPacket.wpos());     // prevent spam at not read packet tail
                    pUser->SendEquipError(EQUIP_ERR_NOT_IN_COMBAT,pItem,NULL);
                    return;
                }
            }
        }

        // Prevent potion drink if another potion in processing (client have potions disabled in like case)
        if (pItem->IsPotion() && pUser->GetLastPotionId())
        {
            recvPacket.rpos(recvPacket.wpos());             // prevent spam at not read packet tail
            pUser->SendEquipError(EQUIP_ERR_OBJECT_IS_BUSY,pItem,NULL);
            return;
        }
    }

    // check also  BIND_WHEN_PICKED_UP and BIND_QUEST_ITEM for .additem or .additemset case by GM (not binded at adding to inventory)
    if( pItem->GetProto()->Bonding == BIND_WHEN_USE || pItem->GetProto()->Bonding == BIND_WHEN_PICKED_UP || pItem->GetProto()->Bonding == BIND_QUEST_ITEM )
    {
        if (!pItem->IsSoulBound())
        {
            pItem->SetState(ITEM_CHANGED, pUser);
            pItem->SetBinding( true );
        }
    }

    SpellCastTargets targets;

    recvPacket >> targets.ReadForCaster(pUser);

    targets.Update(pUser);

    if (!pItem->IsTargetValidForItemUse(targets.getUnitTarget()))
    {
        // free gray item after use fail
        pUser->SendEquipError(EQUIP_ERR_NONE, pItem, NULL);

        // send spell error
        if (SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellid))
        {
            // for implicit area/coord target spells
            if (IsPointEffectTarget(Targets(spellInfo->EffectImplicitTargetA[EFFECT_INDEX_0])) ||
                IsAreaEffectTarget(Targets(spellInfo->EffectImplicitTargetA[EFFECT_INDEX_0])))
                Spell::SendCastResult(_player,spellInfo,cast_count,SPELL_FAILED_NO_VALID_TARGETS);
            // for explicit target spells
            else
                Spell::SendCastResult(_player,spellInfo,cast_count,SPELL_FAILED_BAD_TARGETS);
        }
        return;
    }

    //Note: If script stop casting it must send appropriate data to client to prevent stuck item in gray state.
    if (!sScriptMgr.OnItemUse(pUser, pItem, targets))
    {
        // no script or script not process request by self
        pUser->CastItemUseSpell(pItem,targets,cast_count,glyphIndex);
    }
}

#define OPEN_CHEST 11437
#define OPEN_SAFE 11535
#define OPEN_CAGE 11792
#define OPEN_BOOTY_CHEST 5107
#define OPEN_STRONGBOX 8517

void WorldSession::HandleOpenItemOpcode(WorldPacket& recvPacket)
{
    DETAIL_LOG("WORLD: CMSG_OPEN_ITEM packet, data length = %i",(uint32)recvPacket.size());

    uint8 bagIndex, slot;

    recvPacket >> bagIndex >> slot;

    DETAIL_LOG("bagIndex: %u, slot: %u",bagIndex,slot);

    Player* pUser = _player;

    // ignore for remote control state
    if (!pUser->IsSelfMover())
        return;

    Item *pItem = pUser->GetItemByPos(bagIndex, slot);
    if(!pItem)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, NULL, NULL );
        return;
    }

    ItemPrototype const *proto = pItem->GetProto();
    if(!proto)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, NULL );
        return;
    }

    // locked item
    uint32 lockId = proto->LockID;
    if(lockId && !pItem->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_UNLOCKED))
    {
        LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);

        if (!lockInfo)
        {
            pUser->SendEquipError(EQUIP_ERR_ITEM_LOCKED, pItem, NULL );
            sLog.outError( "WORLD::OpenItem: item [guid = %u] has an unknown lockId: %u!", pItem->GetGUIDLow() , lockId);
            return;
        }

        // required picklocking
        if(lockInfo->Skill[1] || lockInfo->Skill[0])
        {
            pUser->SendEquipError(EQUIP_ERR_ITEM_LOCKED, pItem, NULL );
            return;
        }
    }

    if (pItem->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_WRAPPED))// wrapped?
    {
        QueryResult *result = CharacterDatabase.PQuery("SELECT entry, flags FROM character_gifts WHERE item_guid = '%u'", pItem->GetGUIDLow());
        if (result)
        {
            Field *fields = result->Fetch();
            uint32 entry = fields[0].GetUInt32();
            uint32 flags = fields[1].GetUInt32();

            pItem->SetGuidValue(ITEM_FIELD_GIFTCREATOR, ObjectGuid());
            pItem->SetEntry(entry);
            pItem->SetUInt32Value(ITEM_FIELD_FLAGS, flags);
            pItem->SetState(ITEM_CHANGED, pUser);
            delete result;
        }
        else
        {
            sLog.outError("Wrapped item %u don't have record in character_gifts table and will deleted", pItem->GetGUIDLow());
            pUser->DestroyItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
            return;
        }

        static SqlStatementID delGifts ;

        SqlStatement stmt = CharacterDatabase.CreateStatement(delGifts, "DELETE FROM character_gifts WHERE item_guid = ?");
        stmt.PExecute(pItem->GetGUIDLow());
    }
    else
        pUser->SendLoot(pItem->GetObjectGuid(),LOOT_CORPSE);
}

void WorldSession::HandleGameObjectUseOpcode( WorldPacket & recv_data )
{
    ObjectGuid guid;

    recv_data >> guid;

    DEBUG_LOG("WORLD: Recvd CMSG_GAMEOBJ_USE Message guid: %s", guid.GetString().c_str());

    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    GameObject *obj = GetPlayer()->GetMap()->GetGameObject(guid);

    if(!obj)
        return;

    // Never expect this opcode for some type GO's
    if (obj->GetGoType() == GAMEOBJECT_TYPE_GENERIC)
    {
        sLog.outError("HandleGameObjectUseOpcode: CMSG_GAMEOBJ_USE for not allowed GameObject type %u (Entry %u), didn't expect this to happen.", obj->GetGoType(), obj->GetEntry());
        return;
    }

    obj->Use(_player);
}

void WorldSession::HandleGameobjectReportUse(WorldPacket& recvPacket)
{
    ObjectGuid guid;
    recvPacket >> guid;

    DEBUG_LOG("WORLD: Recvd CMSG_GAMEOBJ_REPORT_USE Message guid: %s", guid.GetString().c_str());

    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    GameObject* go = GetPlayer()->GetMap()->GetGameObject(guid);
    if (!go)
        return;

    if(!go->IsWithinDistInMap(_player,INTERACTION_DISTANCE))
        return;

    _player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_USE_GAMEOBJECT, go->GetEntry());
}

void WorldSession::HandleCastSpellOpcode(WorldPacket& recvPacket)
{
    uint32 spellId;
    uint8  cast_count, unk_flags;
    recvPacket >> cast_count;
    recvPacket >> spellId;
    recvPacket >> unk_flags;                                // flags (if 0x02 - some additional data are received)

    // ignore for remote control state (for player case)
    Unit* _mover = GetPlayer()->GetMover();
    if (_mover != GetPlayer() && _mover->GetTypeId()==TYPEID_PLAYER)
    {
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at ignore packet
        return;
    }

    DEBUG_LOG("WORLD: got cast spell packet, spellId - %u, cast_count: %u, unk_flags %u, data length = %i",
        spellId, cast_count, unk_flags, (uint32)recvPacket.size());

    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId );

    if(!spellInfo)
    {
        sLog.outError("WORLD: unknown spell id %u", spellId);
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at ignore packet
        return;
    }

    if (sWorld.getConfig(CONFIG_BOOL_ALLOW_FLYING_MOUNTS_EVERYWHERE))
    {
        if (_player->isRunningSpell(spellInfo))
            _player->Unmount();
        else if (_player->isRunningFormSpell(spellInfo))
            _player->RemoveFlyingSpells();
    }

    //  Players on vehicles may cast many simple spells (like knock) from self

    Unit* mover = NULL;

    if (spellInfo->AttributesEx6 & SPELL_ATTR_EX6_UNK12 && _mover->IsCharmerOrOwnerPlayerOrPlayerItself())
        mover = _mover->GetCharmerOrOwnerPlayerOrPlayerItself();
    else
        mover = _mover;

    if (mover->GetTypeId()==TYPEID_PLAYER)
    {
        // not have spell in spellbook or spell passive and not casted by client
        if ( ((Player*)mover)->GetUInt16Value(PLAYER_FIELD_BYTES2, 0) == 0 && !((Player*)mover)->HasActiveSpell (spellId) || IsPassiveSpell(spellInfo))
        {
            sLog.outError("World: Player %u casts spell %u which he shouldn't have", mover->GetGUIDLow(), spellId);
            //cheater? kick? ban?
            recvPacket.rpos(recvPacket.wpos());                 // prevent spam at ignore packet
            return;
        }
    }
    else
    {
        // not have spell in spellbook or spell passive and not casted by client
        if (!((Creature*)mover)->HasSpell(spellId) || IsPassiveSpell(spellInfo))
        {
            //cheater? kick? ban?
            recvPacket.rpos(recvPacket.wpos());                 // prevent spam at ignore packet
            return;
        }
    }

    // client provided targets
    SpellCastTargets targets;

    recvPacket >> targets.ReadForCaster(mover);

    // some spell cast packet including more data (for projectiles?)
    if (unk_flags & 0x02)
    {
        uint8 unk1;

        recvPacket >> Unused<float>();                      // unk1, coords?
        recvPacket >> Unused<float>();                      // unk1, coords?
        recvPacket >> unk1;                                 // >> 1 or 0
        if(unk1)
        {
            ObjectGuid guid;                                // guid - unused
            MovementInfo movementInfo;

            recvPacket >> Unused<uint32>();                 // >> MSG_MOVE_STOP
            recvPacket >> guid.ReadAsPacked();
            recvPacket >> movementInfo;
        }
    }

    // auto-selection buff level base at target level (in spellInfo)
    if (Unit* target = targets.getUnitTarget())
    {
        // if rank not found then function return NULL but in explicit cast case original spell can be casted and later failed with appropriate error message
        if (SpellEntry const *actualSpellInfo = sSpellMgr.SelectAuraRankForLevel(spellInfo, target->getLevel()))
            spellInfo = actualSpellInfo;
    }

    Spell *spell = new Spell(mover, spellInfo, false);
    spell->m_cast_count = cast_count;                       // set count of casts
    spell->prepare(&targets);
}

void WorldSession::HandleCancelCastOpcode(WorldPacket& recvPacket)
{
    uint32 spellId;

    recvPacket.read_skip<uint8>();                          // counter, increments with every CANCEL packet, don't use for now
    recvPacket >> spellId;

    // ignore for remote control state (for player case)
    Unit* mover = _player->GetMover();
    if (mover != _player && mover->GetTypeId()==TYPEID_PLAYER)
        return;

    //FIXME: hack, ignore unexpected client cancel Deadly Throw cast
    if(spellId==26679)
        return;

    if(mover->IsNonMeleeSpellCasted(false))
        mover->InterruptNonMeleeSpells(false,spellId);
}

void WorldSession::HandleCancelAuraOpcode( WorldPacket& recvPacket)
{
    uint32 spellId;
    recvPacket >> spellId;

    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);
    if (!spellInfo)
        return;

    if (spellInfo->Attributes & SPELL_ATTR_CANT_CANCEL)
        return;

    if (IsPassiveSpell(spellInfo))
        return;

    if (!IsPositiveSpell(spellId))
    {
        // ignore for remote control state
        if (!_player->IsSelfMover())
        {
            // except own aura spells
            bool allow = false;
            for(int k = 0; k < MAX_EFFECT_INDEX; ++k)
            {
                if (spellInfo->EffectApplyAuraName[k] == SPELL_AURA_MOD_POSSESS ||
                    spellInfo->EffectApplyAuraName[k] == SPELL_AURA_MOD_POSSESS_PET)
                {
                    allow = true;
                    break;
                }
            }

            // this also include case when aura not found
            if(!allow)
                return;
        }
        else
            return;
    }

    // channeled spell case (it currently casted then)
    if (IsChanneledSpell(spellInfo))
    {
        if (Spell* curSpell = _player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (curSpell->m_spellInfo->Id==spellId)
                _player->InterruptSpell(CURRENT_CHANNELED_SPELL);
        return;
    }

    SpellAuraHolder *holder = _player->GetSpellAuraHolder(spellId);

    // not own area auras can't be cancelled (note: maybe need to check for aura on holder and not general on spell)
    if (holder && holder->GetCasterGuid() != _player->GetObjectGuid() && HasAreaAuraEffect(holder->GetSpellProto()))
        return;

    // non channeled case
    _player->RemoveAurasDueToSpellByCancel(spellId);
    if(_player->isFlyingSpell(spellInfo) || _player->isFlyingFormSpell(spellInfo))
        _player->SetFlyingMountTimer();
}

void WorldSession::HandlePetCancelAuraOpcode( WorldPacket& recvPacket)
{
    ObjectGuid guid;
    uint32 spellId;

    recvPacket >> guid;
    recvPacket >> spellId;

    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId );
    if(!spellInfo)
    {
        sLog.outError("WORLD: unknown PET spell id %u", spellId);
        return;
    }

    Creature* pet = GetPlayer()->GetMap()->GetAnyTypeCreature(guid);

    if (!pet)
    {
        sLog.outError("HandlePetCancelAuraOpcode - %s not exist.", guid.GetString().c_str());
        return;
    }

    if (guid != GetPlayer()->GetPetGuid() && guid != GetPlayer()->GetCharmGuid())
    {
        sLog.outError("HandlePetCancelAura. %s isn't pet of %s", guid.GetString().c_str(), GetPlayer()->GetGuidStr().c_str());
        return;
    }

    if (!pet->isAlive())
    {
        pet->SendPetActionFeedback(FEEDBACK_PET_DEAD);
        return;
    }

    pet->RemoveAurasDueToSpell(spellId);

    pet->AddCreatureSpellCooldown(spellId);
}

void WorldSession::HandleCancelGrowthAuraOpcode( WorldPacket& /*recvPacket*/)
{
    // nothing do
}

void WorldSession::HandleCancelAutoRepeatSpellOpcode( WorldPacket& /*recvPacket*/)
{
    // cancel and prepare for deleting
    // do not send SMSG_CANCEL_AUTO_REPEAT! client will send this Opcode again (loop)
    _player->GetMover()->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true, false);
}

void WorldSession::HandleCancelChanneling( WorldPacket & recv_data)
{
    recv_data.read_skip<uint32>();                          // spellid, not used

    // ignore for remote control state (for player case)
    Unit* mover = _player->GetMover();
    if (mover != _player && mover->GetTypeId()==TYPEID_PLAYER)
        return;

    mover->InterruptSpell(CURRENT_CHANNELED_SPELL);
}

void WorldSession::HandleTotemDestroyed( WorldPacket& recvPacket)
{
    uint8 slotId;

    recvPacket >> slotId;

    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    if (int(slotId) >= MAX_TOTEM_SLOT)
        return;

    if (Totem* totem = GetPlayer()->GetTotem(TotemSlot(slotId)))
        totem->UnSummon();
}

void WorldSession::HandleSelfResOpcode( WorldPacket & /*recv_data*/ )
{
    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "WORLD: CMSG_SELF_RES");                  // empty opcode

    if(_player->GetUInt32Value(PLAYER_SELF_RES_SPELL))
    {
        SpellEntry const *spellInfo = sSpellStore.LookupEntry(_player->GetUInt32Value(PLAYER_SELF_RES_SPELL));
        if(spellInfo)
            _player->CastSpell(_player, spellInfo, false);

        _player->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);
    }
}

void WorldSession::HandleSpellClick( WorldPacket & recv_data )
{
    ObjectGuid guid;
    recv_data >> guid;

    Creature *unit = _player->GetMap()->GetAnyTypeCreature(guid);
    if (!unit)
        return;

    if (_player->isInCombat() && !guid.IsVehicle())                              // client prevent click and set different icon at combat state
        return;

    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(unit->GetEntry());
    for(SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        if (itr->second.IsFitToRequirements(_player))
        {
            Unit *caster = (itr->second.castFlags & 0x1) ? (Unit*)_player : (Unit*)unit;
            Unit *target = (itr->second.castFlags & 0x2) ? (Unit*)_player : (Unit*)unit;

            caster->CastSpell(target, itr->second.spellId, true);
        }
    }
}

void WorldSession::HandleMirrorImageDataRequest( WorldPacket & recv_data )
{
    uint64 guid;
    recv_data >> guid;

    DEBUG_LOG("WORLD: CMSG_GET_MIRRORIMAGE_DATA");

    // Get unit for which data is needed by client
    Unit *unit = ObjectAccessor::GetUnit(*_player, guid);
    if (!unit)
        return;

    // Get creator of the unit
    Unit *creator = unit;

    // Get SPELL_AURA_247 caster
    if (!unit->GetAurasByType(SPELL_AURA_MIRROR_IMAGE).empty())
        creator = unit->GetAurasByType(SPELL_AURA_MIRROR_IMAGE).front()->GetCaster();

    WorldPacket data(SMSG_MIRRORIMAGE_DATA, 68);
    data << (uint64)guid;
    data << (uint32)creator->GetDisplayId();
    if (creator->GetTypeId()==TYPEID_PLAYER)
    {
        Player * pCreator = (Player *)creator;
        data << (uint8)pCreator->getRace();
        data << (uint8)pCreator->getGender();
        data << (uint8)pCreator->getClass();
        data << (uint8)pCreator->GetByteValue(PLAYER_BYTES, 0); // skin

        data << (uint8)pCreator->GetByteValue(PLAYER_BYTES, 1); // face
        data << (uint8)pCreator->GetByteValue(PLAYER_BYTES, 2); // hair
        data << (uint8)pCreator->GetByteValue(PLAYER_BYTES, 3); // haircolor
        data << (uint8)pCreator->GetByteValue(PLAYER_BYTES_2, 0); // facialhair

        data << (uint32)pCreator->GetGuildId();  // unk
        static const EquipmentSlots ItemSlots[] =
        {
            EQUIPMENT_SLOT_HEAD,
            EQUIPMENT_SLOT_SHOULDERS,
            EQUIPMENT_SLOT_BODY,
            EQUIPMENT_SLOT_CHEST,
            EQUIPMENT_SLOT_WAIST,
            EQUIPMENT_SLOT_LEGS,
            EQUIPMENT_SLOT_FEET,
            EQUIPMENT_SLOT_WRISTS,
            EQUIPMENT_SLOT_HANDS,
            EQUIPMENT_SLOT_BACK,
            EQUIPMENT_SLOT_TABARD,
            EQUIPMENT_SLOT_END
        };
        // Display items in visible slots
        for (EquipmentSlots const* itr = &ItemSlots[0];*itr!=EQUIPMENT_SLOT_END;++itr)
        {
            if (*itr == EQUIPMENT_SLOT_HEAD && pCreator->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM))
                data << (uint32)0;
            else if (*itr == EQUIPMENT_SLOT_BACK && pCreator->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK))
                data << (uint32)0;
            else if (Item const *item = pCreator->GetItemByPos(INVENTORY_SLOT_BAG_0, *itr))
                data << (uint32)item->GetProto()->DisplayInfoID;
            else
                data << (uint32)0;
        }
    }
    else
    {
        // Skip player data for creatures
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
        data << (uint32)0;
    }
    SendPacket( &data );
}

void WorldSession::HandleUpdateProjectilePosition(WorldPacket& recvPacket)
{
    ObjectGuid casterGuid;  // actually target ?
    uint32 spellId;         // Spell Id
    uint8  castCount;       //
    float m_targetX, m_targetY, m_targetZ; // Position of missile hit

    recvPacket >> casterGuid;
    recvPacket >> spellId;
    recvPacket >> castCount;

    recvPacket >> m_targetX >> m_targetY >> m_targetZ;

    // Do we need unit as we use 3d position anyway ?
    Unit* pCaster = GetPlayer()->GetMap()->GetUnit(casterGuid);
    if (!pCaster)
        return;

    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);

    if(!spellInfo)
    {
        sLog.outError("CMSG_UPDATE_PROJECTILE_POSITION: unknown spell id %u", spellId);
        recvPacket.rpos(recvPacket.wpos());                 // prevent spam at ignore packet
        return;
    }

    WorldPacket data(SMSG_NOTIFY_MISSILE_TRAJECTORY_COLLISION, 8+1+4+4+4);
    data << casterGuid;
    data << castCount;
    data << m_targetX;
    data << m_targetY;
    data << m_targetZ;
    SendPacket(&data);

    for(int i = 0; i < 3; ++i)
    {
        if(spellInfo->EffectTriggerSpell[i])
            if (SpellEntry const* spellInfoT = sSpellStore.LookupEntry(spellInfo->EffectTriggerSpell[i]))
                pCaster->CastSpell(m_targetX, m_targetY, m_targetZ, spellInfoT, true);
    }
}
