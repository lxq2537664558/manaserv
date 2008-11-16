/*
 *  The Mana World Server
 *  Copyright 2004 The Mana World Development Team
 *
 *  This file is part of The Mana World.
 *
 *  The Mana World  is free software; you can redistribute  it and/or modify it
 *  under the terms of the GNU General  Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or any later version.
 *
 *  The Mana  World is  distributed in  the hope  that it  will be  useful, but
 *  WITHOUT ANY WARRANTY; without even  the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 *  more details.
 *
 *  You should  have received a  copy of the  GNU General Public  License along
 *  with The Mana  World; if not, write to the  Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <cassert>

#include "game-server/being.hpp"

#include "defines.h"
#include "game-server/attackzone.hpp"
#include "game-server/collisiondetection.hpp"
#include "game-server/eventlistener.hpp"
#include "game-server/mapcomposite.hpp"
#include "game-server/effect.hpp"
#include "utils/logger.h"

Being::Being(int type, int id):
    MovingObject(type, id),
    mAction(STAND),
    mHpRegenTimer(0)
{
    Attribute attr = { 0, 0 };
    mAttributes.resize(NB_BEING_ATTRIBUTES, attr);
    // Initialize element resistance to 100 (normal damage).
    for (int i = BASE_ELEM_BEGIN; i < BASE_ELEM_END; ++i)
    {
        mAttributes[i].base = 100;
    }
}

int Being::damage(Object *, Damage const &damage)
{
    if (mAction == DEAD) return 0;

    int HPloss = damage.base;
    if (damage.delta)
    {
        HPloss += rand() / (RAND_MAX / (damage.delta + 1));
    }

    int hitThrow = rand()%(damage.cth + 1);
    int evadeThrow = rand()%(getModifiedAttribute(BASE_ATTR_EVADE) + 1);
    if (evadeThrow > hitThrow)
    {
        HPloss = 0;
    }

    /* Elemental modifier at 100 means normal damage. At 0, it means immune.
       And at 200, it means vulnerable (double damage). */
    int mod1 = getModifiedAttribute(BASE_ELEM_BEGIN + damage.element);
    HPloss = HPloss * (mod1 / 100);
    /* Defence is an absolute value which is subtracted from the damage total. */
    int mod2 = 0;
    switch (damage.type)
    {
        case DAMAGE_PHYSICAL:
            mod2 = getModifiedAttribute(BASE_ATTR_PHY_RES);
            HPloss = HPloss - mod2;
            break;
        case DAMAGE_MAGICAL:
            mod2 = getModifiedAttribute(BASE_ATTR_MAG_RES);
            HPloss = HPloss / (mod2 + 1);
            break;
        default:
            break;
    }

    if (HPloss < 0) HPloss = 0;

    mHitsTaken.push_back(HPloss);
    Attribute &HP = mAttributes[BASE_ATTR_HP];
    LOG_DEBUG("Being " << getPublicID() << " suffered "<<HPloss<<" damage. HP: "<<HP.base + HP.mod<<"/"<<HP.base);
    HP.mod -= HPloss;
    if (HPloss != 0) modifiedAttribute(BASE_ATTR_HP);

    return HPloss;
}

void Being::died()
{
    if (mAction == DEAD) return;

    LOG_DEBUG("Being " << getPublicID() << " died.");
    setAction(DEAD);
    // dead beings stay where they are
    clearDestination();

    for (Listeners::iterator i = mListeners.begin(),
         i_end = mListeners.end(); i != i_end;)
    {
        EventListener const &l = **i;
        ++i; // In case the listener removes itself from the list on the fly.
        if (l.dispatch->died) l.dispatch->died(&l, this);
    }
}

void Being::move()
{
    MovingObject::move();
    if (mAction == WALK || mAction == STAND)
    {
        mAction = (mActionTime) ? WALK : STAND;
    }
}

int Being::directionToAngle(int direction)
{
    switch (direction)
    {
        case DIRECTION_UP:    return  90;
        case DIRECTION_DOWN:  return 270;
        case DIRECTION_RIGHT: return 180;
        case DIRECTION_LEFT:
        default:              return   0;
    }
}

void Being::performAttack(Damage const &damage, AttackZone const *attackZone)
{
    Point ppos = getPosition();
    const int attackAngle = directionToAngle(getDirection());

    std::list<Being *> victims;

    for (MovingObjectIterator
         i(getMap()->getAroundObjectIterator(this, attackZone->range)); i; ++i)
    {
        MovingObject *o = *i;
        Point opos = o->getPosition();

        if (o == this) continue;

        int type = o->getType();

        if (type != OBJECT_CHARACTER && type != OBJECT_MONSTER) continue;


        switch (attackZone->shape)
        {
            case ATTZONESHAPE_CONE:
                if  (Collision::diskWithCircleSector(
                        opos, o->getSize(),
                        ppos, attackZone->range,
                        attackZone->angle / 2, attackAngle)
                    )
                {
                    victims.push_back(static_cast< Being * >(o));
                }
                break;
            default:
                break;
        }
    }

    if (attackZone->multiTarget)
    {
        // damage everyone
        for (std::list<Being *>::iterator i = victims.begin();
             i != victims.end();
             i++)
        {
            (*i)->damage(this, damage);
        }
    }
    else
    {
        // find the closest and damage this one
        Being* closestVictim = NULL;
        int closestDistance = INT_MAX;
        for (std::list<Being *>::iterator i = victims.begin();
             i != victims.end();
             i++)
        {
            Point opos = (*i)->getPosition();
            int distance = abs(opos.x - ppos.x) + abs(opos.y - ppos.y);
            /* not using pythagoras here is a) faster and b) results in more natural
               target selection because targets closer to the center line of the
               attack angle are prioritized
            */
            if (distance < closestDistance)
            {
                closestVictim = (*i);
                closestDistance = distance;
            }
        }
        if (closestVictim) closestVictim->damage(this, damage);
    }
}

void Being::setAction(Action action)
{
    mAction = action;
    if (action != Being::ATTACK && // The players are informed about these actions
        action != Being::WALK)     // by other messages
    {
        raiseUpdateFlags(UPDATEFLAG_ACTIONCHANGE);
    }
}

void Being::applyModifier(int attr, int amount, int duration, int lvl)
{
    if (duration)
    {
        AttributeModifier mod;
        mod.attr = attr;
        mod.value = amount;
        mod.duration = duration;
        mod.level = lvl;
        mModifiers.push_back(mod);
    }
    mAttributes[attr].mod += amount;
    modifiedAttribute(attr);
}

void Being::dispellModifiers(int level)
{
    AttributeModifiers::iterator i = mModifiers.begin();
    while (i != mModifiers.end())
    {
        if (i->level && i->level <= level)
        {
            mAttributes[i->attr].mod -= i->value;
            modifiedAttribute(i->attr);
            i = mModifiers.erase(i);
            continue;
        }
        ++i;
    }
}

int Being::getModifiedAttribute(int attr) const
{
    int res = mAttributes[attr].base + mAttributes[attr].mod;
    return res <= 0 ? 0 : res;
}

void Being::update()
{

    int oldHP = getModifiedAttribute(BASE_ATTR_HP);
    int newHP = oldHP;
    int maxHP = getAttribute(BASE_ATTR_HP);

    // regenerate HP
    if (mAction != DEAD && ++mHpRegenTimer >= TICKS_PER_HP_REGENERATION)
    {
        mHpRegenTimer = 0;
        newHP += getModifiedAttribute(BASE_ATTR_HP_REGEN);
    }
    //cap HP at maximum
    if (newHP > maxHP)
    {
        newHP = maxHP;
    }
    //only update HP when it actually changed to avoid network noise
    if (newHP != oldHP)
    {
        applyModifier(BASE_ATTR_HP, newHP - oldHP);
        raiseUpdateFlags(UPDATEFLAG_HEALTHCHANGE);
    }

    // Update lifetime of effects.
    AttributeModifiers::iterator i = mModifiers.begin();
    while (i != mModifiers.end())
    {
        --i->duration;
        if (!i->duration)
        {
            mAttributes[i->attr].mod -= i->value;
            modifiedAttribute(i->attr);
            i = mModifiers.erase(i);
            continue;
        }
        ++i;
    }

    //check if being died
    if (getModifiedAttribute(BASE_ATTR_HP) <= 0 && mAction != DEAD)
    {
        died();
    }
}
