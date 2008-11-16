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
 */

#include "account-server/accounthandler.hpp"

#include "defines.h"
#include "point.h"
#include "account-server/account.hpp"
#include "account-server/accountclient.hpp"
#include "account-server/character.hpp"
#include "account-server/dalstorage.hpp"
#include "account-server/serverhandler.hpp"
#include "chat-server/chathandler.hpp"
#include "common/configuration.hpp"
#include "net/connectionhandler.hpp"
#include "net/messagein.hpp"
#include "net/messageout.hpp"
#include "net/netcomputer.hpp"
#include "utils/logger.h"
#include "utils/stringfilter.h"
#include "utils/tokencollector.hpp"
#include "utils/tokendispenser.hpp"
#include "utils/sha256.h"

class AccountHandler : public ConnectionHandler
{
    public:
        /**
         * Constructor.
         */
        AccountHandler();

        /**
         * Called by the token collector in order to associate a client to its
         * account ID.
         */
        void tokenMatched(AccountClient *computer, int accountID);

        /**
         * Called by the token collector when a client was not acknowledged for
         * some time and should be disconnected.
         */
        void deletePendingClient(AccountClient *computer);

        /**
         * Called by the token collector.
         */
        void deletePendingConnect(int) {}

        /**
         * Token collector for connecting a client coming from a game server
         * without having to provide username and password a second time.
         */
        TokenCollector< AccountHandler, AccountClient *, int > mTokenCollector;

    protected:
        /**
         * Processes account related messages.
         */
        void processMessage(NetComputer *computer, MessageIn &message);

        NetComputer *computerConnected(ENetPeer *peer);

        void computerDisconnected(NetComputer *comp);
};

static AccountHandler *accountHandler;

AccountHandler::AccountHandler():
    mTokenCollector(this)
{
}

bool AccountClientHandler::initialize(int port)
{
    accountHandler = new AccountHandler;
    LOG_INFO("Account handler started:");
    return accountHandler->startListen(port);
}

void AccountClientHandler::deinitialize()
{
    accountHandler->stopListen();
    delete accountHandler;
}

void AccountClientHandler::process()
{
    accountHandler->process(50);
}

void AccountClientHandler::prepareReconnect(std::string const &token, int id)
{
    accountHandler->mTokenCollector.addPendingConnect(token, id);
}

NetComputer* AccountHandler::computerConnected(ENetPeer *peer)
{
    return new AccountClient(peer);
}

void AccountHandler::computerDisconnected(NetComputer *comp)
{
    AccountClient* computer = static_cast< AccountClient * >(comp);

    if (computer->status == CLIENT_QUEUED)
        // Delete it from the pendingClient list
        mTokenCollector.deletePendingClient(computer);

    delete computer; // ~AccountClient unsets the account
}

static void sendCharacterData(AccountClient &computer, int slot, Character const &ch)
{
    MessageOut charInfo(APMSG_CHAR_INFO);
    charInfo.writeByte(slot);
    charInfo.writeString(ch.getName());
    charInfo.writeByte(ch.getGender());
    charInfo.writeByte(ch.getHairStyle());
    charInfo.writeByte(ch.getHairColor());
    charInfo.writeShort(ch.getLevel());
    charInfo.writeShort(ch.getCharacterPoints());
    charInfo.writeShort(ch.getCorrectionPoints());
    charInfo.writeLong(ch.getPossessions().money);

    for (int j = CHAR_ATTR_BEGIN; j < CHAR_ATTR_END; ++j)
    {
        charInfo.writeShort(ch.getAttribute(j));
    }

    computer.send(charInfo);
}

static void handleLoginMessage(AccountClient &computer, MessageIn &msg)
{
    MessageOut reply(APMSG_LOGIN_RESPONSE);

    if (computer.status != CLIENT_LOGIN)
    {
        reply.writeByte(ERRMSG_FAILURE);
        computer.send(reply);
        return;
    }

    int clientVersion = msg.readLong();

    if (clientVersion < Configuration::getValue("clientVersion", 0))
    {
        reply.writeByte(LOGIN_INVALID_VERSION);
        computer.send(reply);
        return;
    }

    std::string username = msg.readString();
    std::string password = msg.readString();

    if (stringFilter->findDoubleQuotes(username))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
        computer.send(reply);
        return;
    }

    if (accountHandler->getClientNumber() >= MAX_CLIENTS )
    {
        reply.writeByte(ERRMSG_SERVER_FULL);
        computer.send(reply);
        return;
    }

    // Check if the account exists
    Account *acc = storage->getAccount(username);

    if (!acc || acc->getPassword() != password)
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
        computer.send(reply);
        delete acc;
        return;
    }

    if (acc->getLevel() == AL_BANNED)
    {
        reply.writeByte(LOGIN_BANNED);
        computer.send(reply);
        delete acc;
        return;
    }

    // set lastLogin date of the account
    time_t login;
    time(&login);
    acc->setLastLogin(login);
    storage->updateLastLogin(acc);


    // Associate account with connection
    computer.setAccount(acc);
    computer.status = CLIENT_CONNECTED;

    reply.writeByte(ERRMSG_OK);
    computer.send(reply); // Acknowledge login

    // Return information about available characters
    Characters &chars = acc->getCharacters();

    // Send characters list
    for (unsigned int i = 0; i < chars.size(); i++)
    {
        sendCharacterData(computer, i, *chars[i]);
    }
}

static void handleLogoutMessage(AccountClient &computer)
{
    MessageOut reply(APMSG_LOGOUT_RESPONSE);

    if (computer.status == CLIENT_LOGIN)
    {
        reply.writeByte(ERRMSG_NO_LOGIN);
    }
    else if (computer.status == CLIENT_CONNECTED)
    {
        computer.unsetAccount();
        computer.status = CLIENT_LOGIN;
        reply.writeByte(ERRMSG_OK);
    }
    else if (computer.status == CLIENT_QUEUED)
    {
        // Delete it from the pendingClient list
        accountHandler->mTokenCollector.deletePendingClient(&computer);
        computer.status = CLIENT_LOGIN;
        reply.writeByte(ERRMSG_OK);
    }
    computer.send(reply);
}

static void handleReconnectMessage(AccountClient &computer, MessageIn &msg)
{
    if (computer.status != CLIENT_LOGIN)
    {
        LOG_DEBUG("Account tried to reconnect, but was already logged in "
                  "or queued.");
        return;
    }

    std::string magic_token = msg.readString(MAGIC_TOKEN_LENGTH);
    computer.status = CLIENT_QUEUED; // Before the addPendingClient
    accountHandler->mTokenCollector.addPendingClient(magic_token, &computer);
}

static void handleRegisterMessage(AccountClient &computer, MessageIn &msg)
{
    int clientVersion = msg.readLong();
    std::string username = msg.readString();
    std::string password = msg.readString();
    std::string email = msg.readString();

    MessageOut reply(APMSG_REGISTER_RESPONSE);

    if (computer.status != CLIENT_LOGIN)
    {
        reply.writeByte(ERRMSG_FAILURE);
    }
    else if (clientVersion < Configuration::getValue("clientVersion", 0))
    {
        reply.writeByte(REGISTER_INVALID_VERSION);
    }
    else if (stringFilter->findDoubleQuotes(username))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (stringFilter->findDoubleQuotes(email))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if ((username.length() < MIN_LOGIN_LENGTH) ||
            (username.length() > MAX_LOGIN_LENGTH))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (password.length() < MIN_PASSWORD_LENGTH ||
             password.length() > MAX_PASSWORD_LENGTH)
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (stringFilter->findDoubleQuotes(password))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (!stringFilter->isEmailValid(email))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    // Checking if the Name is slang's free.
    else if (!stringFilter->filterContent(username))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    // Check whether the account already exists.
    else if (storage->doesUserNameExist(username))
    {
        reply.writeByte(REGISTER_EXISTS_USERNAME);
    }
    // Find out whether the email is already in use.
    else if (storage->doesEmailAddressExist(sha256(email)))
    {
        reply.writeByte(REGISTER_EXISTS_EMAIL);
    }
    else
    {
        Account *acc = new Account;
        acc->setName(username);
        // We hash the password using the username as salt.
        acc->setPassword(sha256(username + password));
        // We hash email server-side without using a salt.
        acc->setEmail(sha256(email));
        acc->setLevel(AL_NORMAL);

        // set the date and time of the account registration, and the last login
        time_t regdate;
        time(&regdate);
        acc->setRegistrationDate(regdate);
        acc->setLastLogin(regdate);

        storage->addAccount(acc);
        reply.writeByte(ERRMSG_OK);

        // Associate account with connection
        computer.setAccount(acc);
        computer.status = CLIENT_CONNECTED;
    }

    computer.send(reply);
}

static void handleUnregisterMessage(AccountClient &computer, MessageIn &msg)
{
    LOG_DEBUG("AccountHandler::handleUnregisterMessage");
    std::string username = msg.readString();
    std::string password = msg.readString();

    MessageOut reply(APMSG_UNREGISTER_RESPONSE);

    if (computer.status != CLIENT_CONNECTED)
    {
        reply.writeByte(ERRMSG_FAILURE);
        computer.send(reply);
        return;
    }

    if (stringFilter->findDoubleQuotes(username))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
        computer.send(reply);
        return;
    }

    // See if the account exists
    Account *acc = storage->getAccount(username);

    if (!acc || acc->getPassword() != password)
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
        computer.send(reply);
        delete acc;
        return;
    }

    // Delete account and associated characters
    LOG_INFO("Unregistered \"" << username
             << "\", AccountID: " << acc->getID());
    storage->delAccount(acc);
    reply.writeByte(ERRMSG_OK);

    computer.send(reply);
}

static void handleEmailChangeMessage(AccountClient &computer, MessageIn &msg)
{
    MessageOut reply(APMSG_EMAIL_CHANGE_RESPONSE);

    Account *acc = computer.getAccount();
    if (!acc)
    {
        reply.writeByte(ERRMSG_NO_LOGIN);
        computer.send(reply);
        return;
    }

    const std::string email = msg.readString();
    const std::string emailHash = sha256(email);

    if (!stringFilter->isEmailValid(email))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (stringFilter->findDoubleQuotes(email))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (storage->doesEmailAddressExist(emailHash))
    {
        reply.writeByte(ERRMSG_EMAIL_ALREADY_EXISTS);
    }
    else
    {
        acc->setEmail(emailHash);
        // Keep the database up to date otherwise we will go out of sync
        storage->flush(acc);
        reply.writeByte(ERRMSG_OK);
    }
    computer.send(reply);
}

static void handlePasswordChangeMessage(AccountClient &computer, MessageIn &msg)
{
    std::string oldPassword = msg.readString();
    std::string newPassword = msg.readString();

    MessageOut reply(APMSG_PASSWORD_CHANGE_RESPONSE);

    Account *acc = computer.getAccount();
    if (!acc)
    {
        reply.writeByte(ERRMSG_NO_LOGIN);
    }
    else if (newPassword.length() != SHA256_HASH_LENGTH)
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (stringFilter->findDoubleQuotes(newPassword))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (oldPassword != acc->getPassword())
    {
        reply.writeByte(ERRMSG_FAILURE);
    }
    else
    {
        acc->setPassword(newPassword);
        // Keep the database up to date otherwise we will go out of sync
        storage->flush(acc);
        reply.writeByte(ERRMSG_OK);
    }

    computer.send(reply);
}

static void handleCharacterCreateMessage(AccountClient &computer, MessageIn &msg)
{
    std::string name = msg.readString();
    int hairStyle = msg.readByte();
    int hairColor = msg.readByte();
    int gender = msg.readByte();

    MessageOut reply(APMSG_CHAR_CREATE_RESPONSE);

    Account *acc = computer.getAccount();
    if (!acc)
    {
        reply.writeByte(ERRMSG_NO_LOGIN);
    }
    else if (!stringFilter->filterContent(name))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (stringFilter->findDoubleQuotes(name))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else if (hairStyle > MAX_HAIRSTYLE_VALUE)
    {
        reply.writeByte(CREATE_INVALID_HAIRSTYLE);
    }
    else if (hairColor > MAX_HAIRCOLOR_VALUE)
    {
        reply.writeByte(CREATE_INVALID_HAIRCOLOR);
    }
    else if (gender > MAX_GENDER_VALUE)
    {
        reply.writeByte(CREATE_INVALID_GENDER);
    }
    else if ((name.length() < MIN_CHARACTER_LENGTH) ||
             (name.length() > MAX_CHARACTER_LENGTH))
    {
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
    }
    else
    {
        if (storage->doesCharacterNameExist(name))
        {
            reply.writeByte(CREATE_EXISTS_NAME);
            computer.send(reply);
            return;
        }

        // An account shouldn't have more than MAX_OF_CHARACTERS characters.
        Characters &chars = acc->getCharacters();
        if (chars.size() >= MAX_OF_CHARACTERS)
        {
            reply.writeByte(CREATE_TOO_MUCH_CHARACTERS);
            computer.send(reply);
            return;
        }

        // LATER_ON: Add race, face and maybe special attributes.

        // Customization of character's attributes...
        int attributes[CHAR_ATTR_NB];
        for (int i = 0; i < CHAR_ATTR_NB; ++i)
            attributes[i] = msg.readShort();

        unsigned int totalAttributes = 0;
        bool validNonZeroAttributes = true;
        for (int i = 0; i < CHAR_ATTR_NB; ++i)
        {
            // For good total attributes check.
            totalAttributes += attributes[i];

            // For checking if all stats are at least > 0
            if (attributes[i] <= 0) validNonZeroAttributes = false;
        }

        if (totalAttributes > POINTS_TO_DISTRIBUTES_AT_LVL1)
        {
            reply.writeByte(CREATE_ATTRIBUTES_TOO_HIGH);
        }
        else if (totalAttributes < POINTS_TO_DISTRIBUTES_AT_LVL1)
        {
            reply.writeByte(CREATE_ATTRIBUTES_TOO_LOW);
        }
        else if (!validNonZeroAttributes)
        {
            reply.writeByte(CREATE_ATTRIBUTES_EQUAL_TO_ZERO);
        }
        else
        {
            Character *newCharacter = new Character(name);
            for (int i = CHAR_ATTR_BEGIN; i < CHAR_ATTR_END; ++i)
                newCharacter->setAttribute(i, attributes[i - CHAR_ATTR_BEGIN]);
            newCharacter->setAccount(acc);
            newCharacter->setLevel(1);
            newCharacter->setCharacterPoints(0);
            newCharacter->setCorrectionPoints(0);
            newCharacter->setGender(gender);
            newCharacter->setHairStyle(hairStyle);
            newCharacter->setHairColor(hairColor);
            newCharacter->setMapId(Configuration::getValue("defaultMap", 1));
            Point startingPos(Configuration::getValue("startX", 512),
                              Configuration::getValue("startY", 512));
            newCharacter->setPosition(startingPos);
            acc->addCharacter(newCharacter);

            LOG_INFO("Character " << name << " was created for "
                     << acc->getName() << "'s account.");

            storage->flush(acc); // flush changes
            reply.writeByte(ERRMSG_OK);
            computer.send(reply);

            // Send new characters infos back to client
            int slot = chars.size() - 1;
            sendCharacterData(computer, slot, *chars[slot]);
            return;
        }
    }

    computer.send(reply);
}

static void handleCharacterSelectMessage(AccountClient &computer, MessageIn &msg)
{
    MessageOut reply(APMSG_CHAR_SELECT_RESPONSE);

    Account *acc = computer.getAccount();
    if (!acc)
    {
        reply.writeByte(ERRMSG_NO_LOGIN);
        computer.send(reply);
        return; // not logged in
    }

    unsigned charNum = msg.readByte();
    Characters &chars = acc->getCharacters();

    // Character ID = 0 to Number of Characters - 1.
    if (charNum >= chars.size())
    {
        // invalid char selection
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
        computer.send(reply);
        return;
    }

    Character *selectedChar = chars[charNum];

    std::string address;
    int port;
    if (!GameServerHandler::getGameServerFromMap
            (selectedChar->getMapId(), address, port))
    {
        LOG_ERROR("Character Selection: No game server for the map.");
        reply.writeByte(ERRMSG_FAILURE);
        computer.send(reply);
        return;
    }

    reply.writeByte(ERRMSG_OK);

    LOG_DEBUG(selectedChar->getName() << " is trying to enter the servers.");

    std::string magic_token(utils::getMagicToken());
    reply.writeString(magic_token, MAGIC_TOKEN_LENGTH);
    reply.writeString(address);
    reply.writeShort(port);

    // TODO: get correct address and port for the chat server
    reply.writeString(Configuration::getValue("accountServerAddress",
                                              "localhost"));
    reply.writeShort(Configuration::getValue("accountServerPort",
                                             DEFAULT_SERVER_PORT) + 2);

    GameServerHandler::registerClient(magic_token, selectedChar);
    registerChatClient(magic_token, selectedChar->getName(), acc->getLevel());

    computer.send(reply);
}

static void handleCharacterDeleteMessage(AccountClient &computer, MessageIn &msg)
{
    MessageOut reply(APMSG_CHAR_DELETE_RESPONSE);

    Account *acc = computer.getAccount();
    if (!acc)
    {
        reply.writeByte(ERRMSG_NO_LOGIN);
        computer.send(reply);
        return; // not logged in
    }

    unsigned charNum = msg.readByte();
    Characters &chars = acc->getCharacters();

    // Character ID = 0 to Number of Characters - 1.
    if (charNum >= chars.size())
    {
        // invalid char selection
        reply.writeByte(ERRMSG_INVALID_ARGUMENT);
        computer.send(reply);
        return; // not logged in
    }

    LOG_INFO("Character deleted:" << chars[charNum]->getName());

    acc->delCharacter(charNum);
    storage->flush(acc);

    reply.writeByte(ERRMSG_OK);
    computer.send(reply);
}

void
AccountHandler::tokenMatched(AccountClient *computer, int accountID)
{
    MessageOut reply(APMSG_RECONNECT_RESPONSE);

    // Associate account with connection.
    Account *acc = storage->getAccount(accountID);
    computer->setAccount(acc);
    computer->status = CLIENT_CONNECTED;

    reply.writeByte(ERRMSG_OK);
    computer->send(reply);

    // Return information about available characters
    Characters &chars = acc->getCharacters();

    // Send characters list
    for (unsigned int i = 0; i < chars.size(); i++)
    {
        sendCharacterData(*computer, i, *chars[i]);
    }
}

void
AccountHandler::deletePendingClient(AccountClient* computer)
{
    MessageOut msg(APMSG_RECONNECT_RESPONSE);
    msg.writeByte(ERRMSG_TIME_OUT);
    computer->disconnect(msg);
    // The computer will be deleted when the disconnect event is processed
}

void AccountHandler::processMessage(NetComputer *comp, MessageIn &message)
{
    AccountClient &computer = *static_cast< AccountClient * >(comp);

    switch (message.getId())
    {
        case PAMSG_LOGIN:
            LOG_DEBUG("Received msg ... PAMSG_LOGIN");
            handleLoginMessage(computer, message);
            break;

        case PAMSG_LOGOUT:
            LOG_DEBUG("Received msg ... PAMSG_LOGOUT");
            handleLogoutMessage(computer);
            break;

        case PAMSG_RECONNECT:
            LOG_DEBUG("Received msg ... PAMSG_RECONNECT");
            handleReconnectMessage(computer, message);
            break;

        case PAMSG_REGISTER:
            LOG_DEBUG("Received msg ... PAMSG_REGISTER");
            handleRegisterMessage(computer, message);
            break;

        case PAMSG_UNREGISTER:
            LOG_DEBUG("Received msg ... PAMSG_UNREGISTER");
            handleUnregisterMessage(computer, message);
            break;

        case PAMSG_EMAIL_CHANGE:
            LOG_DEBUG("Received msg ... PAMSG_EMAIL_CHANGE");
            handleEmailChangeMessage(computer, message);
            break;

        case PAMSG_PASSWORD_CHANGE:
            LOG_DEBUG("Received msg ... PAMSG_PASSWORD_CHANGE");
            handlePasswordChangeMessage(computer, message);
            break;

        case PAMSG_CHAR_CREATE:
            LOG_DEBUG("Received msg ... PAMSG_CHAR_CREATE");
            handleCharacterCreateMessage(computer, message);
            break;

        case PAMSG_CHAR_SELECT:
            LOG_DEBUG("Received msg ... PAMSG_CHAR_SELECT");
            handleCharacterSelectMessage(computer, message);
            break;

        case PAMSG_CHAR_DELETE:
            LOG_DEBUG("Received msg ... PAMSG_CHAR_DELETE");
            handleCharacterDeleteMessage(computer, message);
            break;

        default:
            LOG_WARN("AccountHandler::processMessage, Invalid message type "
                     << message.getId());
            MessageOut result(XXMSG_INVALID);
            computer.send(result);
            break;
    }
}
