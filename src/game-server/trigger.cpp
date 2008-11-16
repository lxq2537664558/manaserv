/*
 *  The Mana World Server
 *  Copyright 2006 The Mana World Development Team
 *
 *  This file is part of The Mana World.
 *
 *  The Mana World is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  The Mana World is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Mana World; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "game-server/trigger.hpp"

#include "game-server/character.hpp"
#include "game-server/mapcomposite.hpp"
#include "game-server/movingobject.hpp"
#include "game-server/state.hpp"

#include "utils/logger.h"

void WarpAction::process(Object *obj)
{
    if (obj->getType() == OBJECT_CHARACTER)
    {
        GameState::enqueueWarp(static_cast< Character * >(obj), mMap, mX, mY);
    }
}

void ScriptAction::process(Object *obj)
{
    LOG_DEBUG("Script trigger area activated: "<<mFunction<<"("<<obj<<", "<<mArg<<")");
    if (!mScript) return;
    if (mFunction == "") return;
    mScript->prepare(mFunction);
    mScript->push(obj);
    mScript->push(mArg);
    mScript->execute();
}

void TriggerArea::update()
{
    std::set<Object*> insideNow;
    for (MovingObjectIterator i(getMap()->getInsideRectangleIterator(mZone)); i; ++i)
    {
        if (mZone.contains((*i)->getPosition())) //<-- Why is this additional condition necessary? Shouldn't getInsideRectangleIterator already exclude those outside of the zone? --Crush
        {
            insideNow.insert(*i);

            if (!mOnce || mInside.find(*i) == mInside.end())
            {
                mAction->process(*i);
            }
        }
    }
    mInside.swap(insideNow); //swapping is faster than assigning
}
