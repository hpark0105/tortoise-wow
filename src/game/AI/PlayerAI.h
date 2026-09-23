/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2010 MaNGOSZero <http://github.com/mangoszero/mangoszero/>
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

#ifndef MANGOS_PLAYERAI_H
#define MANGOS_PLAYERAI_H

#include "Common.h"
#include "Platform/Define.h"
#include "Policies/Singleton.h"
#include "Dynamic/ObjectRegistry.h"
#include "Dynamic/FactoryHolder.h"
#include "CreatureAI.h" // Pour 'enum CanCastResult'

class WorldObject;
class Unit;
class Creature;
class Player;
class SpellEntry;

class PlayerAI
{
    public:
        explicit PlayerAI(Player* pPlayer) : me(pPlayer), enablePositiveSpells(false) {}
        virtual ~PlayerAI();
        void SetPlayer(Player* player) { me = player; }
        virtual void Remove();

        // Called at World update tick
        virtual void UpdateAI(const uint32 /*diff*/);
        virtual void MovementInform(uint32 MovementType, uint32 Data = 0) {}

        // BL-002: observe-only damage observation callbacks (no-op in the
        // base class; existing AIs remain behaviorally unchanged). The
        // authoritative damage path fires them for a direct player attacker
        // or a direct player victim only, with the effective damage (the
        // victim's actual health loss, overkill excluded), the spell id
        // (0 for white damage), the periodic (DoT) flag and whether the
        // victim died from this operation.
        virtual void OnDamageDealt(Unit* /*target*/, uint32 /*effectiveDamage*/, uint32 /*spellId*/, bool /*periodic*/, bool /*targetDied*/) {}
        virtual void OnDamageTaken(Unit* /*attacker*/, uint32 /*effectiveDamage*/, uint32 /*spellId*/, bool /*periodic*/, bool /*victimDied*/) {}

        ///== Helpeurs =====================================
        CanCastResult CanCastSpell(Unit* pTarget, const SpellEntry *pSpell, bool isTriggered, bool checkControlled = true);

        ///== Fields =======================================

        // Pointer to controlled by AI player
        Player* me;
        bool enablePositiveSpells;
};

class PlayerControlledAI: public PlayerAI
{
    public:
        explicit PlayerControlledAI(Player* pPlayer, Unit* caster = nullptr);

        virtual ~PlayerControlledAI();

        // Called at World update tick
        void UpdateAI(const uint32 /*diff*/) override;
        Unit* FindController();
        void UpdateTarget(Unit* victim);

        ///== Fields =======================================
        ObjectGuid controllerGuid;
        uint32 uiGlobalCD;
        turtle_vector<uint32, Category_AI> usableSpells;
        bool bIsMelee;
        bool isHealer;
        
};

#endif
