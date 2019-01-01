/*
* Copyright (C) 2008-2010 Trinity <http://www.trinitycore.org/>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "Common.h"
#include "DatabaseEnv.h"
#include "CreatureTextMgr.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Object.h"
#include "WorldPacket.h"
#include "Chat.h"
#include "Containers.h"

#include "Object.h"
#include "GridNotifiers.h"
#include "CellImpl.h"

class CreatureTextBuilder
{
    public:
        CreatureTextBuilder(WorldObject const* obj, uint8 gender, ChatMsg msgtype, uint8 textGroup, uint32 id, uint32 language, WorldObject const* target)
            : _source(obj), _gender(gender), _msgType(msgtype), _textGroup(textGroup), _textId(id), _language(language), _target(target) { }

        size_t operator()(WorldPacket* data, LocaleConstant locale) const
        {
            std::string const& text = sCreatureTextMgr->GetLocalizedChatString(_source->GetEntry(), _gender, _textGroup, _textId, locale);

            return ChatHandler::BuildChatPacket(*data, _msgType, Language(_language), _source, _target, text, 0, "", locale);
        }

    private:
        WorldObject const* _source;
        uint8 _gender;
        ChatMsg _msgType;
        uint8 _textGroup;
        uint32 _textId;
        uint32 _language;
        WorldObject const* _target;
};

class PlayerTextBuilder
{
    public:
        PlayerTextBuilder(WorldObject const* obj, WorldObject const* speaker, uint8 gender, ChatMsg msgtype, uint8 textGroup, uint32 id, uint32 language, WorldObject const* target)
            : _source(obj), _talker(speaker), _gender(gender), _msgType(msgtype), _textGroup(textGroup), _textId(id), _language(language), _target(target) { }

        size_t operator()(WorldPacket* data, LocaleConstant locale) const
        {
            std::string const& text = sCreatureTextMgr->GetLocalizedChatString(_source->GetEntry(), _gender, _textGroup, _textId, locale);

            return ChatHandler::BuildChatPacket(*data, _msgType, Language(_language), _talker, _target, text, 0, "", locale);
        }

    private:
        WorldObject const* _source;
        WorldObject const* _talker;
        uint8 _gender;
        ChatMsg _msgType;
        uint8 _textGroup;
        uint32 _textId;
        uint32 _language;
        WorldObject const* _target;
};

void CreatureTextMgr::LoadCreatureTexts()
{
    mTextMap.clear(); // for reload case
    //all currently used temp texts are NOT reset

    PreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_SEL_CREATURE_TEXT);
    PreparedQueryResult result = WorldDatabase.Query(stmt);
    
    if (!result)
    {
        TC_LOG_INFO("server.loading",">> Loaded 0 Creature Texts. DB table `creature_texts` is empty.");
        return;
    }

    uint32 TextCount = 0;
    uint32 CreatureCount = 0;

    do
    {
        Field* fields = result->Fetch();
        CreatureTextEntry temp;

        temp.entry           = fields[0].GetUInt32();
        temp.group           = fields[1].GetUInt8();
        temp.id              = fields[2].GetUInt8();
        temp.text            = fields[3].GetString();
        temp.type            = ChatMsg(fields[4].GetUInt8());
        temp.lang            = Language(fields[5].GetUInt8());
        temp.probability     = fields[6].GetFloat();
        temp.emote           = Emote(fields[7].GetUInt32());
        temp.duration        = fields[8].GetUInt32();
        temp.sound           = fields[9].GetUInt32();
        temp.BroadcastTextId = fields[10].GetUInt32();
        temp.TextRange       = CreatureTextRange(fields[11].GetUInt8());

        if (temp.sound)
        {
            if (!sSoundEntriesStore.LookupEntry(temp.sound)){
                TC_LOG_ERROR("sql.sql","CreatureTextMgr: Entry %u, Group %u in table `creature_texts` has Sound %u but sound does not exist.", temp.entry, temp.group, temp.sound);
                temp.sound = 0;
            }
        }
        if (!GetLanguageDescByID(temp.lang))
        {
            TC_LOG_ERROR("sql.sql","CreatureTextMgr: Entry %u, Group %u in table `creature_texts` using Language %u but Language does not exist.", temp.entry, temp.group, uint32(temp.lang));
            temp.lang = LANG_UNIVERSAL;
        }
        switch (temp.type)
        {
        case CHAT_MSG_MONSTER_SAY:
        case CHAT_MSG_MONSTER_PARTY:
        case CHAT_MSG_MONSTER_YELL:
        case CHAT_MSG_MONSTER_WHISPER:
        case CHAT_MSG_MONSTER_EMOTE:
        case CHAT_MSG_RAID_BOSS_WHISPER:
        case CHAT_MSG_RAID_BOSS_EMOTE:
            break;
        default:
            TC_LOG_ERROR("sql.sql", "CreatureTextMgr: Entry %u, Group %u in table `creature_texts` has Type %u but this chat type is not a valid chat type for creatures. Defaulting to 12 (CHAT_MSG_MONSTER_SAY)", temp.entry, temp.group, uint32(temp.type));
            temp.type = CHAT_MSG_MONSTER_SAY;
        }
        if (temp.emote)
        {
            if (!sEmotesStore.LookupEntry(temp.emote))
            {
                TC_LOG_ERROR("FIXME","CreatureTextMgr: Entry %u, Group %u in table `creature_texts` has Emote %u but emote does not exist.", temp.entry, temp.group, uint32(temp.emote));
                temp.emote = EMOTE_ONESHOT_NONE;
            }
        }

        if (temp.BroadcastTextId)
        {
            if (!sObjectMgr->GetBroadcastText(temp.BroadcastTextId))
            {
                TC_LOG_ERROR("sql.sql", "CreatureTextMgr: Entry %u, Group %u, Id %u in table `creature_text` has non-existing or incompatible BroadcastTextId %u.", temp.entry, temp.group, temp.id, temp.BroadcastTextId);
                temp.BroadcastTextId = 0;
            }
        }

        if (temp.TextRange > TEXT_RANGE_WORLD)
        {
            TC_LOG_ERROR("sql.sql", "CreatureTextMgr: Entry %u, Group %u, Id %u in table `creature_text` has incorrect TextRange %u.", temp.entry, temp.group, temp.id, temp.TextRange);
            temp.TextRange = TEXT_RANGE_NORMAL;
        }

        //entry not yet added, add empty TextHolder (list of groups)
        if (mTextMap.find(temp.entry) == mTextMap.end())
            ++CreatureCount;

        //add the text into our entry's group
        mTextMap[temp.entry][temp.group].push_back(temp);
        
        ++TextCount;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading",">> Loaded %u Creature Texts for %u Creatures.", TextCount, CreatureCount);
}


void CreatureTextMgr::LoadCreatureTextLocales()
{
    uint32 oldMSTime = GetMSTime();

    mLocaleTextMap.clear(); // for reload case

    QueryResult result = WorldDatabase.Query("SELECT entry, groupid, id, text_loc1, text_loc2, text_loc3, text_loc4, text_loc5, text_loc6, text_loc7, text_loc8 FROM locales_creature_text");

    if (!result)
        return;

    uint32 textCount = 0;

    do
    {
        Field* fields = result->Fetch();
        CreatureTextLocale& loc = mLocaleTextMap[CreatureTextId(fields[0].GetUInt32(), uint32(fields[1].GetUInt8()), uint32(fields[2].GetUInt8()))];
        for (uint8 i = TOTAL_LOCALES - 1; i > 0; --i)
        {
            LocaleConstant locale = LocaleConstant(i);
            ObjectMgr::AddLocaleString(fields[3 + i - 1].GetString(), locale, loc.Text);
        }

        ++textCount;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded %u creature localized texts in %u ms", textCount, GetMSTimeDiffToNow(oldMSTime));

}

uint32 CreatureTextMgr::SendChat(Creature* source, uint8 textGroup, WorldObject const* whisperTarget /*= nullptr*/, ChatMsg msgType /*= CHAT_MSG_ADDON*/, Language language /*= LANG_ADDON*/, CreatureTextRange range /*= TEXT_RANGE_NORMAL*/, uint32 sound /*= 0*/, Team team /*= TEAM_OTHER*/, bool gmOnly /*= false*/, Player* srcPlr /*= nullptr*/)
{
    if (!source)
        return 0;

    CreatureTextMap::const_iterator sList = mTextMap.find(source->GetEntry());
    if (sList == mTextMap.end())
    {
        TC_LOG_ERROR("sql.sql", "CreatureTextMgr: Could not find Text for Creature %s (Entry %u, spawnId %u) in 'creature_text' table. Ignoring.", source->GetName().c_str(), source->GetEntry(), source->GetSpawnId());
        return 0;
    }

    CreatureTextHolder const& textHolder = sList->second;
    auto itr = textHolder.find(textGroup);
    if (itr == textHolder.end())
    {
        TC_LOG_ERROR("sql.sql", "CreatureTextMgr: Could not find TextGroup %u for Creature %s (Entry %u, spawnId %u) in 'creature_text' table. Ignoring.", uint32(textGroup), source->GetName().c_str(), source->GetEntry(), source->GetSpawnId());
        return 0;
    }

    CreatureTextGroup const& textGroupContainer = itr->second;  //has all texts in the group
    CreatureTextRepeatIds repeatGroup = GetRepeatGroup(source, textGroup);//has all textIDs from the group that were already said
    CreatureTextGroup tempGroup;//will use this to talk after sorting repeatGroup

    for (const auto & giter : textGroupContainer)
        if (std::find(repeatGroup.begin(), repeatGroup.end(), giter.id) == repeatGroup.end())
            tempGroup.push_back(giter);

    if (tempGroup.empty())
    {
        source->ClearTextRepeatGroup(textGroup);
        tempGroup = textGroupContainer;
    }

    auto iter = Trinity::Containers::SelectRandomWeightedContainerElement(tempGroup, [](CreatureTextEntry const& t) -> double
    {
        return t.probability;
    });

    ChatMsg finalType = (msgType == CHAT_MSG_ADDON) ? iter->type : msgType;
    Language finalLang = (language == LANG_ADDON) ? iter->lang : language;
    uint32 finalSound = sound ? sound : iter->sound;

    if (range == TEXT_RANGE_NORMAL)
        range = iter->TextRange;

    if (finalSound)
        SendSound(source, finalSound, finalType, whisperTarget, range, team, gmOnly);

    Unit* finalSource = source;
    if (srcPlr)
        finalSource = srcPlr;

    if (iter->emote)
        SendEmote(finalSource, iter->emote);

    if (srcPlr)
    {
        PlayerTextBuilder builder(source, finalSource, finalSource->GetGender(), finalType, iter->group, iter->id, finalLang, whisperTarget);
        SendChatPacket(finalSource, builder, finalType, whisperTarget, range, team, gmOnly);
    }
    else
    {
        CreatureTextBuilder builder(finalSource, finalSource->GetGender(), finalType, iter->group, iter->id, finalLang, whisperTarget);
        SendChatPacket(finalSource, builder, finalType, whisperTarget, range, team, gmOnly);
    }

    SetRepeatId(source, textGroup, iter->id);

    return iter->duration;
}

void CreatureTextMgr::SendSound(Creature* source, uint32 sound, ChatMsg msgType, WorldObject const* whisperTarget, CreatureTextRange range, Team team, bool gmOnly)
{
    if (!sound || !source)
        return;

    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << uint32(sound);
    SendNonChatPacket(source, &data, msgType, whisperTarget, range, team, gmOnly);
}

void CreatureTextMgr::SendNonChatPacket(WorldObject* source, WorldPacket* data, ChatMsg msgType, WorldObject const* whisperTarget, CreatureTextRange range, Team team, bool gmOnly) const
{
    switch (msgType)
    {
        case CHAT_MSG_MONSTER_PARTY:
        {
            if (!whisperTarget)
                return;

            if (Player const* whisperPlayer = whisperTarget->ToPlayer())
            {
                if (Group const* group = whisperPlayer->GetGroup())
                    group->BroadcastWorker([data](Player* player) { player->SendDirectMessage(data); });
            }
            return;
        }
        case CHAT_MSG_MONSTER_WHISPER:
        case CHAT_MSG_RAID_BOSS_WHISPER:
        {
            if (range == TEXT_RANGE_NORMAL) // ignores team and gmOnly
            {
                if (!whisperTarget || whisperTarget->GetTypeId() != TYPEID_PLAYER)
                    return;

                whisperTarget->ToPlayer()->GetSession()->SendPacket(data);
                return;
            }
            break;
        }
        default:
            break;
    }

    switch (range)
    {
        case TEXT_RANGE_AREA:
        {
            uint32 areaId = source->GetAreaId();
            Map::PlayerList const& players = source->GetMap()->GetPlayers();
            for (const auto & player : players)
                if (player.GetSource()->GetAreaId() == areaId && (team == TEAM_OTHER || Team(player.GetSource()->GetTeam()) == team) && (!gmOnly || player.GetSource()->IsGameMaster()))
                    player.GetSource()->SendDirectMessage(data);
            return;
        }
        case TEXT_RANGE_ZONE:
        {
            uint32 zoneId = source->GetZoneId();
            Map::PlayerList const& players = source->GetMap()->GetPlayers();
            for (const auto & player : players)
                if (player.GetSource()->GetZoneId() == zoneId && (team == TEAM_OTHER || Team(player.GetSource()->GetTeam()) == team) && (!gmOnly || player.GetSource()->IsGameMaster()))
                    player.GetSource()->SendDirectMessage(data);
            return;
        }
        case TEXT_RANGE_MAP:
        {
            Map::PlayerList const& players = source->GetMap()->GetPlayers();
            for (const auto & player : players)
                if ((team == TEAM_OTHER || Team(player.GetSource()->GetTeam()) == team) && (!gmOnly || player.GetSource()->IsGameMaster()))
                    player.GetSource()->SendDirectMessage(data);
            return;
        }
        case TEXT_RANGE_WORLD:
        {
            SessionMap const& smap = sWorld->GetAllSessions();
            for (const auto & iter : smap)
                if (Player* player = iter.second->GetPlayer())
                    if (player->GetSession()  && (team == TEAM_OTHER || Team(player->GetTeam()) == team) && (!gmOnly || player->IsGameMaster()))
                        player->SendDirectMessage(data);
            return;
        }
        case TEXT_RANGE_NORMAL:
        default:
            break;
    }

    float dist = GetRangeForChatType(msgType);
    source->SendMessageToSetInRange(data, dist, true);
}

float CreatureTextMgr::GetRangeForChatType(ChatMsg msgType) const
{
    float dist = sWorld->getConfig(CONFIG_LISTEN_RANGE_SAY);
    switch (msgType)
    {
        case CHAT_MSG_MONSTER_YELL:
            dist = sWorld->getConfig(CONFIG_LISTEN_RANGE_YELL);
            break;
        case CHAT_MSG_MONSTER_EMOTE:
        case CHAT_MSG_RAID_BOSS_EMOTE:
            dist = sWorld->getConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
            break;
        default:
            break;
    }

    return dist;
}

void CreatureTextMgr::SendEmote(Unit* source, uint32 emote)
{
    if (!source)
        return;

    source->HandleEmoteCommand(emote);
}

void CreatureTextMgr::SetRepeatId(Creature* source, uint8 textGroup, uint8 id)
{
    if (!source)
        return;

    source->SetTextRepeatId(textGroup, id);
}

CreatureTextRepeatIds CreatureTextMgr::GetRepeatGroup(Creature* source, uint8 textGroup)
{
    ASSERT(source);//should never happen

    return source->GetTextRepeatGroup(textGroup);
}

bool CreatureTextMgr::TextExist(uint32 sourceEntry, uint8 textGroup)
{
    if (!sourceEntry)
        return false;
    CreatureTextMap::const_iterator sList = mTextMap.find(sourceEntry);
    if (sList == mTextMap.end())
    {
        TC_LOG_ERROR("sql.sql","CreatureTextMgr::TextExist: Could not find Text for Creature (entry %u) in 'creature_text' table.", sourceEntry);
        return false;
    }
    CreatureTextHolder TextHolder = (*sList).second;
    CreatureTextHolder::const_iterator itr = TextHolder.find(textGroup);
    if (itr == TextHolder.end())
    {
        TC_LOG_ERROR("sql.sql","CreatureTextMgr::TextExist: Could not find TextGroup %u for Creature (entry %u).",uint32(textGroup), sourceEntry);
        return false;
    }
    return true;
}

std::string CreatureTextMgr::GetLocalizedChatString(uint32 entry, uint8 gender, uint8 textGroup, uint32 id, LocaleConstant locale) const
{
    auto mapitr = mTextMap.find(entry);
    if (mapitr == mTextMap.end())
        return "";

    auto holderItr = mapitr->second.find(textGroup);
    if (holderItr == mapitr->second.end())
        return "";

    auto groupItr = holderItr->second.begin();
    for (; groupItr != holderItr->second.end(); ++groupItr)
        if (groupItr->id == id)
            break;

    if (groupItr == holderItr->second.end())
        return "";

    if (locale > MAX_LOCALE)
        locale = DEFAULT_LOCALE;

    std::string baseText = "";
    BroadcastText const* bct = sObjectMgr->GetBroadcastText(groupItr->BroadcastTextId);

    if (bct)
        baseText = bct->GetText(locale, gender);
    else
        baseText = groupItr->text;

    if (locale != DEFAULT_LOCALE && !bct)
    {
        auto locItr = mLocaleTextMap.find(CreatureTextId(entry, uint32(textGroup), id));
        if (locItr != mLocaleTextMap.end())
            ObjectMgr::GetLocaleString(locItr->second.Text, locale, baseText);
    }

    return baseText;
}