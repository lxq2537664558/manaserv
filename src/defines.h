/*
 *  The Mana World Server
 *  Copyright 2004 The Mana World Development Team
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
 *
 */
 
 // this file holds the global definitions and constants to be included
 // in multiple places throughout the server
 
 // debug and error definitions go in debug.h, not here
 
// I. ACCOUNT DEFINITIONS
   // A. Account Types
      #define STATUS_NORMAL     0 // denotes normal account status
      #define STATUS_ADMIN      1 // denotes admin acount status
      #define STATUS_GM         2 // denotes GM account status
      #define STATUS_BANNED     3 // denotes a temporarily banned account
      #define STATUS_RESTRICTED 4 // denotes a restricted access account
      
      
      
      
      
// DATA STRUCTURES DEFINITIONS

// persistent character data
struct charData
{
    string charName;         // character's name
    //equipData charEquip;   // structure of equipped items
    //estateData charEstate; // character's estate data
    //petData charPet[3];    // character's pets
    //itemData charItem;     // character's inventory
    //graphicData charGraphic; // character's appearance
}

// Account Data Structure
struct accountData
{
    string accountID;        // the account's ID
    string accountEMail;     // the email of the account's owner
    string accountPass;      // the account's password
    int accountStatus;       // the account's status: normal, gm, banned, etc.
    charData accountChar[5]; // the characters stored in the account.
}
 

