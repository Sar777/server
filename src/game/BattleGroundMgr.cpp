/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
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
#include "SharedDefines.h"
#include "Player.h"
#include "BattleGroundMgr.h"
#include "BattleGroundAV.h"
#include "BattleGroundAB.h"
#include "BattleGroundEY.h"
#include "BattleGroundWS.h"
#include "BattleGroundNA.h"
#include "BattleGroundBE.h"
#include "BattleGroundAA.h"
#include "BattleGroundRL.h"
#include "MapManager.h"
#include "Map.h"
#include "MapInstanced.h"
#include "ObjectMgr.h"
#include "ProgressBar.h"
#include "Chat.h"
#include "ArenaTeam.h"
#include "World.h"
#include "WorldPacket.h"
#include "GameEventMgr.h"

#include "Policies/SingletonImp.h"

INSTANTIATE_SINGLETON_1( BattleGroundMgr );

/*********************************************************/
/***            BATTLEGROUND QUEUE SYSTEM              ***/
/*********************************************************/

BattleGroundQueue::BattleGroundQueue()
{
}

BattleGroundQueue::~BattleGroundQueue()
{
    m_QueuedPlayers.clear();
    for (int i = 0; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for(uint32 j = 0; j < BG_QUEUE_GROUP_TYPES_COUNT; ++j)
        {
            for(GroupsQueueType::iterator itr = m_QueuedGroups[i][j].begin(); itr!= m_QueuedGroups[i][j].end(); ++itr)
                delete (*itr);
            m_QueuedGroups[i][j].clear();
        }
    }
}

/*********************************************************/
/***      BATTLEGROUND QUEUE SELECTION POOLS           ***/
/*********************************************************/

// selection pool initialization, used to clean up from prev selection
void BattleGroundQueue::SelectionPool::Init()
{
    SelectedGroups.clear();
    PlayerCount = 0;
}

// remove group info from selection pool
// returns true when we need to try to add new group to selection pool
// returns false when selection pool is ok or when we kicked smaller group than we need to kick
// sometimes it can be called on empty selection pool
bool BattleGroundQueue::SelectionPool::KickGroup(uint32 size)
{
    //find maxgroup or LAST group with size == size and kick it
    bool found = false;
    GroupsQueueType::iterator groupToKick = SelectedGroups.begin();
    for (GroupsQueueType::iterator itr = groupToKick; itr != SelectedGroups.end(); ++itr)
    {
        if (abs((int32)((*itr)->Players.size() - size)) <= 1)
        {
            groupToKick = itr;
            found = true;
        }
        else if (!found && (*itr)->Players.size() >= (*groupToKick)->Players.size())
            groupToKick = itr;
    }
    //if pool is empty, do nothing
    if (GetPlayerCount())
    {
        //update player count
        GroupQueueInfo* ginfo = (*groupToKick);
        SelectedGroups.erase(groupToKick);
        PlayerCount -= ginfo->Players.size();
        //return false if we kicked smaller group or there are enough players in selection pool
        if (ginfo->Players.size() <= size + 1)
            return false;
    }
    return true;
}

// add group to selection pool
// used when building selection pools
// returns true if we can invite more players, or when we added group to selection pool
// returns false when selection pool is full
bool BattleGroundQueue::SelectionPool::AddGroup(GroupQueueInfo *ginfo, uint32 desiredCount)
{
    //if group is larger than desired count - don't allow to add it to pool
    if (!ginfo->IsInvitedToBGInstanceGUID && desiredCount >= PlayerCount + ginfo->Players.size())
    {
        SelectedGroups.push_back(ginfo);
        // increase selected players count
        PlayerCount += ginfo->Players.size();
        return true;
    }
    if (PlayerCount < desiredCount)
        return true;
    return false;
}

/*********************************************************/
/***               BATTLEGROUND QUEUES                 ***/
/*********************************************************/

// add group to bg queue with the given leader and bg specifications
GroupQueueInfo * BattleGroundQueue::AddGroup(Player *leader, BattleGroundTypeId BgTypeId, BattleGroundBracketId bracketId, uint8 ArenaType, bool isRated, bool isPremade, uint32 arenaRating, uint32 arenateamid)
{
    // create new ginfo
    // cannot use the method like in addplayer, because that could modify an in-queue group's stats
    // (e.g. leader leaving queue then joining as individual again)
    GroupQueueInfo* ginfo = new GroupQueueInfo;
    ginfo->BgTypeId                  = BgTypeId;
    ginfo->ArenaType                 = ArenaType;
    ginfo->ArenaTeamId               = arenateamid;
    ginfo->IsRated                   = isRated;
    ginfo->IsInvitedToBGInstanceGUID = 0;
    ginfo->JoinTime                  = getMSTime();
    ginfo->Team                      = leader->GetTeam();
    ginfo->ArenaTeamRating           = arenaRating;
    ginfo->OpponentsTeamRating       = 0;

    ginfo->Players.clear();

    //compute index (if group is premade or joined a rated match) to queues
    uint32 index = 0;
    if (!isRated && !isPremade)
        index += BG_TEAMS_COUNT;
    if (ginfo->Team == HORDE)
        index++;
    DEBUG_LOG("Adding Group to BattleGroundQueue bgTypeId : %u, bracket_id : %u, index : %u", BgTypeId, bracketId, index);

    m_QueuedGroups[bracketId][index].push_back(ginfo);

    // return ginfo, because it is needed to add players to this group info
    return ginfo;
}

//add player to playermap
void BattleGroundQueue::AddPlayer(Player *plr, GroupQueueInfo *ginfo)
{
    //if player isn't in queue, he is added, if already is, then values are overwritten, no memory leak
    PlayerQueueInfo& info = m_QueuedPlayers[plr->GetGUID()];
    info.InviteTime                 = 0;
    info.LastInviteTime             = 0;
    info.LastOnlineTime             = getMSTime();
    info.GroupInfo                  = ginfo;

    // add the pinfo to ginfo's list
    ginfo->Players[plr->GetGUID()]  = &info;
}

//remove player from queue and from group info, if group info is empty then remove it too
void BattleGroundQueue::RemovePlayer(const uint64& guid, bool decreaseInvitedCount)
{
    //Player *plr = objmgr.GetPlayer(guid);

    int32 bracket_id = -1;                                     // signed for proper for-loop finish
    QueuedPlayersMap::iterator itr;

    //remove player from map, if he's there
    itr = m_QueuedPlayers.find(guid);
    if (itr == m_QueuedPlayers.end())
    {
        sLog.outError("BattleGroundQueue: couldn't find player to remove GUID: %u", GUID_LOPART(guid));
        return;
    }

    GroupQueueInfo* group = itr->second.GroupInfo;
    GroupsQueueType::iterator group_itr, group_itr_tmp;
    // mostly people with the highest levels are in battlegrounds, thats why
    // we count from MAX_BATTLEGROUND_QUEUES - 1 to 0
    // variable index removes useless searching in other team's queue
    uint32 index = (group->Team == HORDE) ? BG_TEAM_HORDE : BG_TEAM_ALLIANCE;

    for (int32 bracket_id_tmp = MAX_BATTLEGROUND_BRACKETS - 1; bracket_id_tmp >= 0 && bracket_id == -1; --bracket_id_tmp)
    {
        //we must check premade and normal team's queue - because when players from premade are joining bg,
        //they leave groupinfo so we can't use its players size to find out index
        for (uint32 j = index; j < BG_QUEUE_GROUP_TYPES_COUNT; j += BG_QUEUE_NORMAL_ALLIANCE)
        {
            for(group_itr_tmp = m_QueuedGroups[bracket_id_tmp][j].begin(); group_itr_tmp != m_QueuedGroups[bracket_id_tmp][j].end(); ++group_itr_tmp)
            {
                if ((*group_itr_tmp) == group)
                {
                    bracket_id = bracket_id_tmp;
                    group_itr = group_itr_tmp;
                    //we must store index to be able to erase iterator
                    index = j;
                    break;
                }
            }
        }
    }
    //player can't be in queue without group, but just in case
    if (bracket_id == -1)
    {
        sLog.outError("BattleGroundQueue: ERROR Cannot find groupinfo for player GUID: %u", GUID_LOPART(guid));
        return;
    }
    DEBUG_LOG("BattleGroundQueue: Removing player GUID %u, from bracket_id %u", GUID_LOPART(guid), (uint32)bracket_id);

    // ALL variables are correctly set
    // We can ignore leveling up in queue - it should not cause crash
    // remove player from group
    // if only one player there, remove group

    // remove player queue info from group queue info
    std::map<uint64, PlayerQueueInfo*>::iterator pitr = group->Players.find(guid);
    if (pitr != group->Players.end())
        group->Players.erase(pitr);

    // if invited to bg, and should decrease invited count, then do it
    if (decreaseInvitedCount && group->IsInvitedToBGInstanceGUID)
    {
        BattleGround* bg = sBattleGroundMgr.GetBattleGround(group->IsInvitedToBGInstanceGUID, group->BgTypeId);
        if (bg)
            bg->DecreaseInvitedCount(group->Team);
    }

    // remove player queue info
    m_QueuedPlayers.erase(itr);

    // announce to world if arena team left queue for rated match, show only once
    if (group->ArenaType && group->IsRated && group->Players.empty())
        AnnounceWorld(group, guid, false);

    // remove group queue info if needed
    if (group->Players.empty())
    {
        m_QueuedGroups[bracket_id][index].erase(group_itr);
        delete group;
    }
    // if group wasn't empty, so it wasn't deleted, and player have left a rated
    // queue -> everyone from the group should leave too
    // don't remove recursively if already invited to bg!
    else if (!group->IsInvitedToBGInstanceGUID && group->IsRated)
    {
        // remove next player, this is recursive
        // first send removal information
        if (Player *plr2 = sObjectMgr.GetPlayer(group->Players.begin()->first))
        {
            BattleGround * bg = sBattleGroundMgr.GetBattleGroundTemplate(group->BgTypeId);
            BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(group->BgTypeId, group->ArenaType);
            uint32 queueSlot = plr2->GetBattleGroundQueueIndex(bgQueueTypeId);
            plr2->RemoveBattleGroundQueueId(bgQueueTypeId); // must be called this way, because if you move this call to
                                                            // queue->removeplayer, it causes bugs
            WorldPacket data;
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, plr2->GetTeam(), queueSlot, STATUS_NONE, 0, 0);
            plr2->GetSession()->SendPacket(&data);
        }
        // then actually delete, this may delete the group as well!
        RemovePlayer(group->Players.begin()->first, decreaseInvitedCount);
    }
}

void BattleGroundQueue::AnnounceWorld(GroupQueueInfo *ginfo, const uint64& playerGUID, bool isAddedToQueue)
{

    if(ginfo->ArenaType) //if Arena
    {
        if (ginfo->IsRated)
        {
            BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(ginfo->BgTypeId);
            if(!bg)
                return;

            char const* bgName = bg->GetName();
            if(isAddedToQueue)
            {
                if (sWorld.getConfig(CONFIG_BOOL_ARENA_QUEUE_ANNOUNCER_JOIN))
                    sWorld.SendWorldText(LANG_ARENA_QUEUE_ANNOUNCE_WORLD_JOIN, bgName, ginfo->ArenaType, ginfo->ArenaType, ginfo->ArenaTeamRating);
            }
            else
            {
                if (sWorld.getConfig(CONFIG_BOOL_ARENA_QUEUE_ANNOUNCER_EXIT))
                    sWorld.SendWorldText(LANG_ARENA_QUEUE_ANNOUNCE_WORLD_EXIT, bgName, ginfo->ArenaType, ginfo->ArenaType, ginfo->ArenaTeamRating);
            }
        }
    }
    else //if BG
    {
        if( sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_QUEUE_ANNOUNCER_JOIN) )
        {
            Player *plr = sObjectMgr.GetPlayer(playerGUID);
            if(!plr)
                return;

            BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(ginfo->BgTypeId);
            if(!bg)
                return;

            BattleGroundBracketId bracket_id = plr->GetBattleGroundBracketIdFromLevel(bg->GetTypeID());
            char const* bgName = bg->GetName();

            uint32 q_min_level = Player::GetMinLevelForBattleGroundBracketId(bracket_id, ginfo->BgTypeId);
            uint32 q_max_level = Player::GetMaxLevelForBattleGroundBracketId(bracket_id, ginfo->BgTypeId);

            // replace hardcoded max level by player max level for nice output
            if(q_max_level > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
                q_max_level = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

            int8 MinPlayers = bg->GetMinPlayersPerTeam();

            uint8 qHorde = 0;
            uint8 qAlliance = 0;

            GroupsQueueType::const_iterator itr;
            for(itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].begin(); itr != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].end(); ++itr)
                if (!(*itr)->IsInvitedToBGInstanceGUID)
                    qAlliance += (*itr)->Players.size();
            for(itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].begin(); itr != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].end(); ++itr)
                if (!(*itr)->IsInvitedToBGInstanceGUID)
                    qHorde += (*itr)->Players.size();

            // Show queue status to player only (when joining queue)
            if(sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_QUEUE_ANNOUNCER_JOIN)==1)
            {
                ChatHandler(plr).PSendSysMessage(LANG_BG_QUEUE_ANNOUNCE_SELF,
                    bgName, q_min_level, q_max_level, qAlliance, MinPlayers, qHorde, MinPlayers);
            }
            // System message
            else
            {
                sWorld.SendWorldText(LANG_BG_QUEUE_ANNOUNCE_WORLD,
                    bgName, q_min_level, q_max_level, qAlliance, MinPlayers, qHorde, MinPlayers);
            }
        }
    }
}

bool BattleGroundQueue::InviteGroupToBG(GroupQueueInfo * ginfo, BattleGround * bg, uint32 side)
{
    // set side if needed
    if (side)
        ginfo->Team = side;

    if (!ginfo->IsInvitedToBGInstanceGUID)
    {
        // not yet invited
        // set invitation
        ginfo->IsInvitedToBGInstanceGUID = bg->GetInstanceID();
        BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
        // loop through the players
        for(std::map<uint64,PlayerQueueInfo*>::iterator itr = ginfo->Players.begin(); itr != ginfo->Players.end(); ++itr)
        {
            // set status
            itr->second->InviteTime = getMSTime();
            itr->second->LastInviteTime = getMSTime();

            // get the player
            Player* plr = sObjectMgr.GetPlayer(itr->first);
            // if offline, skip him
            if(!plr)
                continue;

            // invite the player
            sBattleGroundMgr.InvitePlayer(plr, bg->GetInstanceID(), bg->GetTypeID(), ginfo->Team);

            WorldPacket data;

            uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgQueueTypeId);

            DEBUG_LOG("Battleground: invited plr %s (%u) to BG instance %u queueindex %u bgtype %u, I can't help it if they don't press the enter battle button.",plr->GetName(),plr->GetGUIDLow(),bg->GetInstanceID(),queueSlot,bg->GetTypeID());

            // send status packet
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, side?side:plr->GetTeam(), queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, 0);
            plr->GetSession()->SendPacket(&data);
        }
        return true;
    }

    return false;
}

// used to remove the Enter Battle window if the battle has already ended, but someone still has it
// (this can happen in arenas mainly, since the preparation is shorter than the timer for the bgqueueremove event
void BattleGroundQueue::BGEndedRemoveInvites(BattleGround *bg)
{
    BattleGroundBracketId bracket_id = bg->GetBracketId();
    uint32 bgInstanceId = bg->GetInstanceID();
    BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    GroupsQueueType::iterator itr, next;
    for(uint32 i = 0; i < BG_QUEUE_GROUP_TYPES_COUNT; i++)
    {
        itr = m_QueuedGroups[bracket_id][i].begin();
        next = itr;
        while (next != m_QueuedGroups[bracket_id][i].end())
        {
            // must do this way, because the groupinfo will be deleted when all playerinfos are removed
            itr = next;
            ++next;
            GroupQueueInfo * ginfo = (*itr);
            // if group was invited to this bg instance, then remove all references
            if( ginfo->IsInvitedToBGInstanceGUID == bgInstanceId )
            {
                // after removing this much playerinfos, the ginfo will be deleted, so we'll use a for loop
                uint32 to_remove = ginfo->Players.size();
                uint32 team = ginfo->Team;
                for(uint32 j = 0; j < to_remove; j++)
                {
                    // always remove the first one in the group
                    std::map<uint64, PlayerQueueInfo * >::iterator itr2 = ginfo->Players.begin();
                    if( itr2 == ginfo->Players.end() )
                    {
                        sLog.outError("Empty Players in ginfo, this should never happen!");
                        return;
                    }
                    // get the player
                    Player * plr = sObjectMgr.GetPlayer(itr2->first);
                    if( !plr )
                    {
                        sLog.outError("Player offline when trying to remove from GroupQueueInfo, this should never happen.");
                        continue;
                    }

                    // get the queueslot
                    uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgQueueTypeId);
                    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
                    {
                        plr->RemoveBattleGroundQueueId(bgQueueTypeId);
                        // remove player from queue, this might delete the ginfo as well! don't use that pointer after this!
                        RemovePlayer(itr2->first, true);
                        WorldPacket data;
                        sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, team, queueSlot, STATUS_NONE, 0, 0);
                        plr->GetSession()->SendPacket(&data);
                    }
                }
            }
        }
    }
}

/*
This function is inviting players to already running battlegrounds
Invitation type is based on config file
large groups are disadvantageous, because they will be kicked first if invitation type = 1
*/
void BattleGroundQueue::FillPlayersToBG(BattleGround* bg, BattleGroundBracketId bracket_id)
{
    int32 hordeFree = bg->GetFreeSlotsForTeam(HORDE);
    int32 aliFree   = bg->GetFreeSlotsForTeam(ALLIANCE);

    //iterator for iterating through bg queue
    GroupsQueueType::const_iterator Ali_itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].begin();
    //count of groups in queue - used to stop cycles
    uint32 aliCount = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].size();
    //index to queue which group is current
    uint32 aliIndex = 0;
    for (; aliIndex < aliCount && m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*Ali_itr), aliFree); aliIndex++)
        ++Ali_itr;
    //the same thing for horde
    GroupsQueueType::const_iterator Horde_itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].begin();
    uint32 hordeCount = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].size();
    uint32 hordeIndex = 0;
    for (; hordeIndex < hordeCount && m_SelectionPools[BG_TEAM_HORDE].AddGroup((*Horde_itr), hordeFree); hordeIndex++)
        ++Horde_itr;

    //if ofc like BG queue invitation is set in config, then we are happy
    if (sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_INVITATION_TYPE) == 0)
        return;

    /*
    if we reached this code, then we have to solve NP - complete problem called Subset sum problem
    So one solution is to check all possible invitation subgroups, or we can use these conditions:
    1. Last time when BattleGroundQueue::Update was executed we invited all possible players - so there is only small possibility
        that we will invite now whole queue, because only 1 change has been made to queues from the last BattleGroundQueue::Update call
    2. Other thing we should consider is group order in queue
    */

    // At first we need to compare free space in bg and our selection pool
    int32 diffAli   = aliFree   - int32(m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount());
    int32 diffHorde = hordeFree - int32(m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount());
    while( abs(diffAli - diffHorde) > 1 && (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() > 0 || m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() > 0) )
    {
        //each cycle execution we need to kick at least 1 group
        if (diffAli < diffHorde)
        {
            //kick alliance group, add to pool new group if needed
            if (m_SelectionPools[BG_TEAM_ALLIANCE].KickGroup(diffHorde - diffAli))
            {
                for (; aliIndex < aliCount && m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*Ali_itr), (aliFree >= diffHorde) ? aliFree - diffHorde : 0); aliIndex++)
                    ++Ali_itr;
            }
            //if ali selection is already empty, then kick horde group, but if there are less horde than ali in bg - break;
            if (!m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())
            {
                if (aliFree <= diffHorde + 1)
                    break;
                m_SelectionPools[BG_TEAM_HORDE].KickGroup(diffHorde - diffAli);
            }
        }
        else
        {
            //kick horde group, add to pool new group if needed
            if (m_SelectionPools[BG_TEAM_HORDE].KickGroup(diffAli - diffHorde))
            {
                for (; hordeIndex < hordeCount && m_SelectionPools[BG_TEAM_HORDE].AddGroup((*Horde_itr), (hordeFree >= diffAli) ? hordeFree - diffAli : 0); hordeIndex++)
                    ++Horde_itr;
            }
            if (!m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount())
            {
                if (hordeFree <= diffAli + 1)
                    break;
                m_SelectionPools[BG_TEAM_ALLIANCE].KickGroup(diffAli - diffHorde);
            }
        }
        //count diffs after small update
        diffAli   = aliFree   - int32(m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount());
        diffHorde = hordeFree - int32(m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount());
    }
}

// this method checks if premade versus premade battleground is possible
// then after 30 mins (default) in queue it moves premade group to normal queue
// it tries to invite as much players as it can - to MaxPlayersPerTeam, because premade groups have more than MinPlayersPerTeam players
bool BattleGroundQueue::CheckPremadeMatch(BattleGroundBracketId bracket_id, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam)
{
    //check match
    if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() && !m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty())
    {
        //start premade match
        //if groups aren't invited
        GroupsQueueType::const_iterator ali_group, horde_group;
        for( ali_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].begin(); ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end(); ++ali_group)
            if (!(*ali_group)->IsInvitedToBGInstanceGUID)
                break;
        for( horde_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].begin(); horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end(); ++horde_group)
            if (!(*horde_group)->IsInvitedToBGInstanceGUID)
                break;

        if (ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end() && horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end())
        {
            m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*ali_group), MaxPlayersPerTeam);
            m_SelectionPools[BG_TEAM_HORDE].AddGroup((*horde_group), MaxPlayersPerTeam);
            //add groups/players from normal queue to size of bigger group
            uint32 maxPlayers = std::max(m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount(), m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount());
            GroupsQueueType::const_iterator itr;
            for(uint32 i = 0; i < BG_TEAMS_COUNT; i++)
            {
                for(itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin(); itr != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++itr)
                {
                    //if itr can join BG and player count is less that maxPlayers, then add group to selectionpool
                    if (!(*itr)->IsInvitedToBGInstanceGUID && !m_SelectionPools[i].AddGroup((*itr), maxPlayers))
                        break;
                }
            }
            //premade selection pools are set
            return true;
        }
    }
    // now check if we can move group from Premade queue to normal queue (timer has expired) or group size lowered!!
    // this could be 2 cycles but i'm checking only first team in queue - it can cause problem -
    // if first is invited to BG and seconds timer expired, but we can ignore it, because players have only 80 seconds to click to enter bg
    // and when they click or after 80 seconds the queue info is removed from queue
    uint32 time_before = getMSTime() - sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH);
    for(uint32 i = 0; i < BG_TEAMS_COUNT; i++)
    {
        if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].empty())
        {
            GroupsQueueType::iterator itr = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].begin();
            if (!(*itr)->IsInvitedToBGInstanceGUID && ((*itr)->JoinTime < time_before || (*itr)->Players.size() < MinPlayersPerTeam))
            {
                //we must insert group to normal queue and erase pointer from premade queue
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].push_front((*itr));
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].erase(itr);
            }
        }
    }
    //selection pools are not set
    return false;
}

// this method tries to create battleground or arena with MinPlayersPerTeam against MinPlayersPerTeam
bool BattleGroundQueue::CheckNormalMatch(BattleGround* bg_template, BattleGroundBracketId bracket_id, uint32 minPlayers, uint32 maxPlayers)
{
    GroupsQueueType::const_iterator itr_team[BG_TEAMS_COUNT];
    for(uint32 i = 0; i < BG_TEAMS_COUNT; i++)
    {
        itr_team[i] = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin();
        for(; itr_team[i] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++(itr_team[i]))
        {
            if (!(*(itr_team[i]))->IsInvitedToBGInstanceGUID)
            {
                m_SelectionPools[i].AddGroup(*(itr_team[i]), maxPlayers);
                if (m_SelectionPools[i].GetPlayerCount() >= minPlayers)
                    break;
            }
        }
    }
    //try to invite same number of players - this cycle may cause longer wait time even if there are enough players in queue, but we want ballanced bg
    uint32 j = BG_TEAM_ALLIANCE;
    if (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() < m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())
        j = BG_TEAM_HORDE;
    if( sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_INVITATION_TYPE) != 0
        && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() >= minPlayers && m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() >= minPlayers )
    {
        //we will try to invite more groups to team with less players indexed by j
        ++(itr_team[j]);                                         //this will not cause a crash, because for cycle above reached break;
        for(; itr_team[j] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + j].end(); ++(itr_team[j]))
        {
            if (!(*(itr_team[j]))->IsInvitedToBGInstanceGUID)
                if (!m_SelectionPools[j].AddGroup(*(itr_team[j]), m_SelectionPools[(j + 1) % BG_TEAMS_COUNT].GetPlayerCount()))
                    break;
        }
        // do not allow to start bg with more than 2 players more on 1 faction
        if (abs((int32)(m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() - m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())) > 2)
            return false;
    }
    //allow 1v0 if debug bg
    if (sBattleGroundMgr.isTesting() && bg_template->isBattleGround() && (m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() || m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount()))
        return true;
    //return true if there are enough players in selection pools - enable to work .debug bg command correctly
    return m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() >= minPlayers && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() >= minPlayers;
}

// this method will check if we can invite players to same faction skirmish match
bool BattleGroundQueue::CheckSkirmishForSameFaction(BattleGroundBracketId bracket_id, uint32 minPlayersPerTeam)
{
    if( m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() < minPlayersPerTeam && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() < minPlayersPerTeam )
        return false;
    uint32 teamIndex = BG_TEAM_ALLIANCE;
    uint32 otherTeam = BG_TEAM_HORDE;
    uint32 otherTeamId = HORDE;
    if (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() == minPlayersPerTeam )
    {
        teamIndex = BG_TEAM_HORDE;
        otherTeam = BG_TEAM_ALLIANCE;
        otherTeamId = ALLIANCE;
    }
    //clear other team's selection
    m_SelectionPools[otherTeam].Init();
    //store last ginfo pointer
    GroupQueueInfo* ginfo = m_SelectionPools[teamIndex].SelectedGroups.back();
    //set itr_team to group that was added to selection pool latest
    GroupsQueueType::iterator itr_team = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].begin();
    for(; itr_team != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr_team)
        if (ginfo == *itr_team)
            break;
    if (itr_team == m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end())
        return false;
    GroupsQueueType::iterator itr_team2 = itr_team;
    ++itr_team2;
    //invite players to other selection pool
    for(; itr_team2 != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr_team2)
    {
        //if selection pool is full then break;
        if (!(*itr_team2)->IsInvitedToBGInstanceGUID && !m_SelectionPools[otherTeam].AddGroup(*itr_team2, minPlayersPerTeam))
            break;
    }
    if (m_SelectionPools[otherTeam].GetPlayerCount() != minPlayersPerTeam)
        return false;

    //here we have correct 2 selections and we need to change one teams team and move selection pool teams to other team's queue
    for(GroupsQueueType::iterator itr = m_SelectionPools[otherTeam].SelectedGroups.begin(); itr != m_SelectionPools[otherTeam].SelectedGroups.end(); ++itr)
    {
        //set correct team
        (*itr)->Team = otherTeamId;
        //add team to other queue
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + otherTeam].push_front(*itr);
        //remove team from old queue
        GroupsQueueType::iterator itr2 = itr_team;
        ++itr2;
        for(; itr2 != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr2)
        {
            if (*itr2 == *itr)
            {
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].erase(itr2);
                break;
            }
        }
    }
    return true;
}

/*
this method is called when group is inserted, or player / group is removed from BG Queue - there is only one player's status changed, so we don't use while(true) cycles to invite whole queue
it must be called after fully adding the members of a group to ensure group joining
should be called from BattleGround::RemovePlayer function in some cases
*/
void BattleGroundQueue::Update(BattleGroundTypeId bgTypeId, BattleGroundBracketId bracket_id, uint8 arenaType, bool isRated, uint32 arenaRating)
{
    //if no players in queue - do nothing
    if( m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].empty() )
        return;

    //battleground with free slot for player should be always in the beggining of the queue
    // maybe it would be better to create bgfreeslotqueue for each bracket_id
    BGFreeSlotQueueType::iterator itr, next;
    for (itr = sBattleGroundMgr.BGFreeSlotQueue[bgTypeId].begin(); itr != sBattleGroundMgr.BGFreeSlotQueue[bgTypeId].end(); itr = next)
    {
        next = itr;
        ++next;
        // DO NOT allow queue manager to invite new player to arena
        if( (*itr)->isBattleGround() && (*itr)->GetTypeID() == bgTypeId && (*itr)->GetBracketId() == bracket_id &&
            (*itr)->GetStatus() > STATUS_WAIT_QUEUE && (*itr)->GetStatus() < STATUS_WAIT_LEAVE )
        {
            BattleGround* bg = *itr; //we have to store battleground pointer here, because when battleground is full, it is removed from free queue (not yet implemented!!)
            // and iterator is invalid

            // clear selection pools
            m_SelectionPools[BG_TEAM_ALLIANCE].Init();
            m_SelectionPools[BG_TEAM_HORDE].Init();

            // call a function that does the job for us
            FillPlayersToBG(bg, bracket_id);

            // now everything is set, invite players
            for(GroupsQueueType::const_iterator citr = m_SelectionPools[BG_TEAM_ALLIANCE].SelectedGroups.begin(); citr != m_SelectionPools[BG_TEAM_ALLIANCE].SelectedGroups.end(); ++citr)
                InviteGroupToBG((*citr), bg, (*citr)->Team);
            for(GroupsQueueType::const_iterator citr = m_SelectionPools[BG_TEAM_HORDE].SelectedGroups.begin(); citr != m_SelectionPools[BG_TEAM_HORDE].SelectedGroups.end(); ++citr)
                InviteGroupToBG((*citr), bg, (*citr)->Team);

            if (!bg->HasFreeSlots())
            {
                // remove BG from BGFreeSlotQueue
                bg->RemoveFromBGFreeSlotQueue();
            }
        }
    }

    // finished iterating through the bgs with free slots, maybe we need to create a new bg

    BattleGround * bg_template = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
    if (!bg_template)
    {
        sLog.outError("Battleground: Update: bg template not found for %u", bgTypeId);
        return;
    }
    // get the min. players per team, properly for larger arenas as well. (must have full teams for arena matches!)
    uint32 MinPlayersPerTeam = bg_template->GetMinPlayersPerTeam();
    uint32 MaxPlayersPerTeam = bg_template->GetMaxPlayersPerTeam();
    if (sBattleGroundMgr.isTesting())
        MinPlayersPerTeam = 1;
    if (bg_template->isArena())
    {
        if (sBattleGroundMgr.isArenaTesting())
        {
            MaxPlayersPerTeam = 1;
            MinPlayersPerTeam = 1;
        }
        else
        {
            //this switch can be much shorter
            MaxPlayersPerTeam = arenaType;
            MinPlayersPerTeam = arenaType;
            /*switch(arenaType)
            {
            case ARENA_TYPE_2v2:
                MaxPlayersPerTeam = 2;
                MinPlayersPerTeam = 2;
                break;
            case ARENA_TYPE_3v3:
                MaxPlayersPerTeam = 3;
                MinPlayersPerTeam = 3;
                break;
            case ARENA_TYPE_5v5:
                MaxPlayersPerTeam = 5;
                MinPlayersPerTeam = 5;
                break;
            }*/
        }
    }

    m_SelectionPools[BG_TEAM_ALLIANCE].Init();
    m_SelectionPools[BG_TEAM_HORDE].Init();

    if (bg_template->isBattleGround())
    {
        //check if there is premade against premade match
        if (CheckPremadeMatch(bracket_id, MinPlayersPerTeam, MaxPlayersPerTeam))
        {
            //create new battleground
            BattleGround * bg2 = sBattleGroundMgr.CreateNewBattleGround(bgTypeId, bracket_id, 0, false);
            if (!bg2)
            {
                sLog.outError("BattleGroundQueue::Update - Cannot create battleground: %u", bgTypeId);
                return;
            }
            //invite those selection pools
            for(uint32 i = 0; i < BG_TEAMS_COUNT; i++)
                for(GroupsQueueType::const_iterator citr = m_SelectionPools[BG_TEAM_ALLIANCE + i].SelectedGroups.begin(); citr != m_SelectionPools[BG_TEAM_ALLIANCE + i].SelectedGroups.end(); ++citr)
                    InviteGroupToBG((*citr), bg2, (*citr)->Team);
            //start bg
            bg2->StartBattleGround();
            //clear structures
            m_SelectionPools[BG_TEAM_ALLIANCE].Init();
            m_SelectionPools[BG_TEAM_HORDE].Init();
        }
    }

    // now check if there are in queues enough players to start new game of (normal battleground, or non-rated arena)
    if (!isRated)
    {
        // if there are enough players in pools, start new battleground or non rated arena
        if (CheckNormalMatch(bg_template, bracket_id, MinPlayersPerTeam, MaxPlayersPerTeam)
            || (bg_template->isArena() && CheckSkirmishForSameFaction(bracket_id, MinPlayersPerTeam)) )
        {
            // we successfully created a pool
            BattleGround * bg2 = sBattleGroundMgr.CreateNewBattleGround(bgTypeId, bracket_id, arenaType, false);
            if (!bg2)
            {
                sLog.outError("BattleGroundQueue::Update - Cannot create battleground: %u", bgTypeId);
                return;
            }

            // invite those selection pools
            for(uint32 i = 0; i < BG_TEAMS_COUNT; i++)
                for(GroupsQueueType::const_iterator citr = m_SelectionPools[BG_TEAM_ALLIANCE + i].SelectedGroups.begin(); citr != m_SelectionPools[BG_TEAM_ALLIANCE + i].SelectedGroups.end(); ++citr)
                    InviteGroupToBG((*citr), bg2, (*citr)->Team);
            // start bg
            bg2->StartBattleGround();
        }
    }
    else if (bg_template->isArena())
    {
        // found out the minimum and maximum ratings the newly added team should battle against
        // arenaRating is the rating of the latest joined team, or 0
        // 0 is on (automatic update call) and we must set it to team's with longest wait time
        if (!arenaRating )
        {
            GroupQueueInfo* front1 = NULL;
            GroupQueueInfo* front2 = NULL;
            if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty())
            {
                front1 = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].front();
                arenaRating = front1->ArenaTeamRating;
            }
            if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty())
            {
                front2 = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].front();
                arenaRating = front2->ArenaTeamRating;
            }
            if (front1 && front2)
            {
                if (front1->JoinTime < front2->JoinTime)
                    arenaRating = front1->ArenaTeamRating;
            }
            else if (!front1 && !front2)
                return; //queues are empty
        }

        //set rating range
        uint32 arenaMinRating = (arenaRating <= sBattleGroundMgr.GetMaxRatingDifference()) ? 0 : arenaRating - sBattleGroundMgr.GetMaxRatingDifference();
        uint32 arenaMaxRating = arenaRating + sBattleGroundMgr.GetMaxRatingDifference();
        // if max rating difference is set and the time past since server startup is greater than the rating discard time
        // (after what time the ratings aren't taken into account when making teams) then
        // the discard time is current_time - time_to_discard, teams that joined after that, will have their ratings taken into account
        // else leave the discard time on 0, this way all ratings will be discarded
        uint32 discardTime = getMSTime() - sBattleGroundMgr.GetRatingDiscardTimer();

        // we need to find 2 teams which will play next game

        GroupsQueueType::iterator itr_team[BG_TEAMS_COUNT];

        //optimalization : --- we dont need to use selection_pools - each update we select max 2 groups

        for(uint32 i = BG_QUEUE_PREMADE_ALLIANCE; i < BG_QUEUE_NORMAL_ALLIANCE; i++)
        {
            // take the group that joined first
            itr_team[i] = m_QueuedGroups[bracket_id][i].begin();
            for(; itr_team[i] != m_QueuedGroups[bracket_id][i].end(); ++(itr_team[i]))
            {
                // if group match conditions, then add it to pool
                if( !(*itr_team[i])->IsInvitedToBGInstanceGUID
                    && (((*itr_team[i])->ArenaTeamRating >= arenaMinRating && (*itr_team[i])->ArenaTeamRating <= arenaMaxRating)
                        || (*itr_team[i])->JoinTime < discardTime) )
                {
                    m_SelectionPools[i].AddGroup((*itr_team[i]), MaxPlayersPerTeam);
                    // break for cycle to be able to start selecting another group from same faction queue
                    break;
                }
            }
        }
        // now we are done if we have 2 groups - ali vs horde!
        // if we don't have, we must try to continue search in same queue
        // tmp variables are correctly set
        // this code isn't much userfriendly - but it is supposed to continue search for mathing group in HORDE queue
        if (m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() == 0 && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount())
        {
            itr_team[BG_TEAM_ALLIANCE] = itr_team[BG_TEAM_HORDE];
            ++itr_team[BG_TEAM_ALLIANCE];
            for(; itr_team[BG_TEAM_ALLIANCE] != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end(); ++(itr_team[BG_TEAM_ALLIANCE]))
            {
                if( !(*itr_team[BG_TEAM_ALLIANCE])->IsInvitedToBGInstanceGUID
                    && (((*itr_team[BG_TEAM_ALLIANCE])->ArenaTeamRating >= arenaMinRating && (*itr_team[BG_TEAM_ALLIANCE])->ArenaTeamRating <= arenaMaxRating)
                        || (*itr_team[BG_TEAM_ALLIANCE])->JoinTime < discardTime) )
                {
                    m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*itr_team[BG_TEAM_ALLIANCE]), MaxPlayersPerTeam);
                    break;
                }
            }
        }
        // this code isn't much userfriendly - but it is supposed to continue search for mathing group in ALLIANCE queue
        if (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() == 0 && m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())
        {
            itr_team[BG_TEAM_HORDE] = itr_team[BG_TEAM_ALLIANCE];
            ++itr_team[BG_TEAM_HORDE];
            for(; itr_team[BG_TEAM_HORDE] != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end(); ++(itr_team[BG_TEAM_HORDE]))
            {
                if( !(*itr_team[BG_TEAM_HORDE])->IsInvitedToBGInstanceGUID
                    && (((*itr_team[BG_TEAM_HORDE])->ArenaTeamRating >= arenaMinRating && (*itr_team[BG_TEAM_HORDE])->ArenaTeamRating <= arenaMaxRating)
                        || (*itr_team[BG_TEAM_HORDE])->JoinTime < discardTime) )
                {
                    m_SelectionPools[BG_TEAM_HORDE].AddGroup((*itr_team[BG_TEAM_HORDE]), MaxPlayersPerTeam);
                    break;
                }
            }
        }

        //if we have 2 teams, then start new arena and invite players!
        if (m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount())
        {
            BattleGround* arena = sBattleGroundMgr.CreateNewBattleGround(bgTypeId, bracket_id, arenaType, true);
            if (!arena)
            {
                sLog.outError("BattlegroundQueue::Update couldn't create arena instance for rated arena match!");
                return;
            }

            (*(itr_team[BG_TEAM_ALLIANCE]))->OpponentsTeamRating = (*(itr_team[BG_TEAM_HORDE]))->ArenaTeamRating;
            DEBUG_LOG("setting oposite teamrating for team %u to %u", (*(itr_team[BG_TEAM_ALLIANCE]))->ArenaTeamId, (*(itr_team[BG_TEAM_ALLIANCE]))->OpponentsTeamRating);
            (*(itr_team[BG_TEAM_HORDE]))->OpponentsTeamRating = (*(itr_team[BG_TEAM_ALLIANCE]))->ArenaTeamRating;
            DEBUG_LOG("setting oposite teamrating for team %u to %u", (*(itr_team[BG_TEAM_HORDE]))->ArenaTeamId, (*(itr_team[BG_TEAM_HORDE]))->OpponentsTeamRating);
            // now we must move team if we changed its faction to another faction queue, because then we will spam log by errors in Queue::RemovePlayer
            if ((*(itr_team[BG_TEAM_ALLIANCE]))->Team != ALLIANCE)
            {
                // add to alliance queue
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].push_front(*(itr_team[BG_TEAM_ALLIANCE]));
                // erase from horde queue
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].erase(itr_team[BG_TEAM_ALLIANCE]);
                itr_team[BG_TEAM_ALLIANCE] = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].begin();
            }
            if ((*(itr_team[BG_TEAM_HORDE]))->Team != HORDE)
            {
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].push_front(*(itr_team[BG_TEAM_HORDE]));
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].erase(itr_team[BG_TEAM_HORDE]);
                itr_team[BG_TEAM_HORDE] = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].begin();
            }

            InviteGroupToBG(*(itr_team[BG_TEAM_ALLIANCE]), arena, ALLIANCE);
            InviteGroupToBG(*(itr_team[BG_TEAM_HORDE]), arena, HORDE);

            DEBUG_LOG("Starting rated arena match!");

            arena->StartBattleGround();
        }
    }
}

/*********************************************************/
/***            BATTLEGROUND QUEUE EVENTS              ***/
/*********************************************************/

bool BGQueueInviteEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = sObjectMgr.GetPlayer( m_PlayerGuid );
    // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
    if (!plr)
        return true;

    BattleGround* bg = sBattleGroundMgr.GetBattleGround(m_BgInstanceGUID, m_BgTypeId);
    //if battleground ended and its instance deleted - do nothing
    if (!bg)
        return true;

    BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue or in battleground
    {
        // check if player is invited to this bg ... this check must be here, because when player leaves queue and joins another, it would cause a problems
        BattleGroundQueue::QueuedPlayersMap const& qpMap = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId].m_QueuedPlayers;
        BattleGroundQueue::QueuedPlayersMap::const_iterator qItr = qpMap.find(m_PlayerGuid);
        if (qItr != qpMap.end() && qItr->second.GroupInfo->IsInvitedToBGInstanceGUID == m_BgInstanceGUID)
        {
            WorldPacket data;
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, qItr->second.GroupInfo->Team, queueSlot, STATUS_WAIT_JOIN, INVITATION_REMIND_TIME, 0);
            plr->GetSession()->SendPacket(&data);
        }
    }
    return true;                                            //event will be deleted
}

void BGQueueInviteEvent::Abort(uint64 /*e_time*/)
{
    //do nothing
}

/*
    this event has many possibilities when it is executed:
    1. player is in battleground ( he clicked enter on invitation window )
    2. player left battleground queue and he isn't there any more
    3. player left battleground queue and he joined it again and IsInvitedToBGInstanceGUID = 0
    4. player left queue and he joined again and he has been invited to same battleground again -> we should not remove him from queue yet
    5. player is invited to bg and he didn't choose what to do and timer expired - only in this condition we should call queue::RemovePlayer
    we must remove player in the 5. case even if battleground object doesn't exist!
*/
bool BGQueueRemoveEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = sObjectMgr.GetPlayer( m_PlayerGuid );
    if (!plr)
        // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
        return true;

    BattleGround* bg = sBattleGroundMgr.GetBattleGround(m_BgInstanceGUID, m_BgTypeId);
    if (!bg)
        return true;

    DEBUG_LOG("Battleground: removing player %u from bg queue for instance %u because of not pressing enter battle in time.",plr->GetGUIDLow(),m_BgInstanceGUID);

    BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
    {
        // check if player is invited to this bg ... this check must be here, because when player leaves queue and joins another, it would cause a problems
        BattleGroundQueue::QueuedPlayersMap& qpMap = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId].m_QueuedPlayers;
        BattleGroundQueue::QueuedPlayersMap::iterator qMapItr = qpMap.find(m_PlayerGuid);
        if (qMapItr != qpMap.end() && qMapItr->second.GroupInfo && qMapItr->second.GroupInfo->IsInvitedToBGInstanceGUID == m_BgInstanceGUID)
        {
            if (qMapItr->second.GroupInfo->IsRated)
            {
                ArenaTeam * at = sObjectMgr.GetArenaTeamById(qMapItr->second.GroupInfo->ArenaTeamId);
                if (at)
                {
                    DEBUG_LOG("UPDATING memberLost's personal arena rating for %u by opponents rating: %u", GUID_LOPART(plr->GetGUID()), qMapItr->second.GroupInfo->OpponentsTeamRating);
                    at->MemberLost(plr, qMapItr->second.GroupInfo->OpponentsTeamRating);
                    at->SaveToDB();
                }
            }
            plr->RemoveBattleGroundQueueId(bgQueueTypeId);
            sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId].RemovePlayer(m_PlayerGuid, true);
            sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId].Update(bg->GetTypeID(),bg->GetBracketId());
            WorldPacket data;
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, m_PlayersTeam, queueSlot, STATUS_NONE, 0, 0);
            plr->GetSession()->SendPacket(&data);
        }
    }
    else
        DEBUG_LOG("Battleground: Player was already removed from queue");

    //event will be deleted
    return true;
}

void BGQueueRemoveEvent::Abort(uint64 /*e_time*/)
{
    //do nothing
}

/*********************************************************/
/***            BATTLEGROUND MANAGER                   ***/
/*********************************************************/

BattleGroundMgr::BattleGroundMgr() : m_AutoDistributionTimeChecker(0), m_ArenaTesting(false)
{
    for(uint32 i = BATTLEGROUND_TYPE_NONE; i < MAX_BATTLEGROUND_TYPE_ID; i++)
        m_BattleGrounds[i].clear();
    m_NextRatingDiscardUpdate = sWorld.getConfig(CONFIG_UINT32_ARENA_RATING_DISCARD_TIMER);
    m_Testing=false;
}

BattleGroundMgr::~BattleGroundMgr()
{
    DeleteAllBattleGrounds();
}

void BattleGroundMgr::DeleteAllBattleGrounds()
{
    for(uint32 i = BATTLEGROUND_TYPE_NONE; i < MAX_BATTLEGROUND_TYPE_ID; i++)
    {
        for(BattleGroundSet::iterator itr = m_BattleGrounds[i].begin(); itr != m_BattleGrounds[i].end();)
        {
            BattleGround * bg = itr->second;
            m_BattleGrounds[i].erase(itr++);
            delete bg;
        }
    }

    // destroy template battlegrounds that listed only in queues (other already terminated)
    for(uint32 bgTypeId = 0; bgTypeId < MAX_BATTLEGROUND_TYPE_ID; ++bgTypeId)
    {
        // ~BattleGround call unregistring BG from queue
        while(!BGFreeSlotQueue[bgTypeId].empty())
            delete BGFreeSlotQueue[bgTypeId].front();
    }
}

// used to update running battlegrounds, and delete finished ones
void BattleGroundMgr::Update(uint32 diff)
{
    BattleGroundSet::iterator itr, next;
    for(uint32 i = BATTLEGROUND_TYPE_NONE; i < MAX_BATTLEGROUND_TYPE_ID; i++)
    {
        itr = m_BattleGrounds[i].begin();
        // skip updating battleground template
        if( itr != m_BattleGrounds[i].end() )
            ++itr;
        for(itr = m_BattleGrounds[i].begin(); itr != m_BattleGrounds[i].end(); itr = next)
        {
            next = itr;
            ++next;
            itr->second->Update(diff);
            // use the SetDeleteThis variable
            // direct deletion caused crashes
            if(itr->second->m_SetDeleteThis)
            {
                BattleGround * bg = itr->second;
                m_BattleGrounds[i].erase(itr);
                delete bg;
            }
        }
    }
    // if rating difference counts, maybe force-update queues
    if(sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_RATING_DIFFERENCE) && sWorld.getConfig(CONFIG_UINT32_ARENA_RATING_DISCARD_TIMER))
    {
        // it's time to force update
        if(m_NextRatingDiscardUpdate < diff)
        {
            // forced update for level 70 rated arenas
            m_BattleGroundQueues[BATTLEGROUND_QUEUE_2v2].Update(BATTLEGROUND_AA,BG_BRACKET_ID_LAST,ARENA_TYPE_2v2,true,0);
            m_BattleGroundQueues[BATTLEGROUND_QUEUE_3v3].Update(BATTLEGROUND_AA,BG_BRACKET_ID_LAST,ARENA_TYPE_3v3,true,0);
            m_BattleGroundQueues[BATTLEGROUND_QUEUE_5v5].Update(BATTLEGROUND_AA,BG_BRACKET_ID_LAST,ARENA_TYPE_5v5,true,0);
            m_NextRatingDiscardUpdate = sWorld.getConfig(CONFIG_UINT32_ARENA_RATING_DISCARD_TIMER);
        }
        else
            m_NextRatingDiscardUpdate -= diff;
    }
    if (sWorld.getConfig(CONFIG_BOOL_ARENA_AUTO_DISTRIBUTE_POINTS))
    {
        if (m_AutoDistributionTimeChecker < diff)
        {
            if (sWorld.GetGameTime() > m_NextAutoDistributionTime)
            {
                DistributeArenaPoints();
                m_NextAutoDistributionTime = time_t(sWorld.GetGameTime() + BATTLEGROUND_ARENA_POINT_DISTRIBUTION_DAY * sWorld.getConfig(CONFIG_UINT32_ARENA_AUTO_DISTRIBUTE_INTERVAL_DAYS));
                CharacterDatabase.PExecute("UPDATE saved_variables SET NextArenaPointDistributionTime = '"UI64FMTD"'", uint64(m_NextAutoDistributionTime));
            }
            m_AutoDistributionTimeChecker = 600000; // check 10 minutes
        }
        else
            m_AutoDistributionTimeChecker -= diff;
    }
}

void BattleGroundMgr::BuildBattleGroundStatusPacket(WorldPacket *data, BattleGround *bg, uint32 team, uint8 QueueSlot, uint8 StatusID, uint32 Time1, uint32 Time2, uint32 arenatype, uint8 israted)
{
    // we can be in 3 queues in same time...
    if(StatusID == 0)
    {
        data->Initialize(SMSG_BATTLEFIELD_STATUS, 4*3);
        *data << uint32(QueueSlot);                         // queue id (0...2)
        *data << uint64(0);
        return;
    }

    data->Initialize(SMSG_BATTLEFIELD_STATUS, (4+1+1+4+2+4+1+4+4+4));
    *data << uint32(QueueSlot);                             // queue id (0...2) - player can be in 3 queues in time
    // uint64 in client
    *data << uint64( uint64(arenatype ? arenatype : bg->GetArenaType()) | (uint64(0x0D) << 8) | (uint64(bg->GetTypeID()) << 16) | (uint64(0x1F90) << 48) );
    *data << uint32(0);                                     // unknown
    // alliance/horde for BG and skirmish/rated for Arenas
    *data << uint8(bg->isArena() ? ( israted ? israted : bg->isRated() ) : bg->GetTeamIndexByTeamId(team));
/*    *data << uint8(arenatype ? arenatype : bg->GetArenaType());                     // team type (0=BG, 2=2x2, 3=3x3, 5=5x5), for arenas    // NOT PROPER VALUE IF ARENA ISN'T RUNNING YET!!!!
    switch(bg->GetTypeID())                                 // value depends on bg id
    {
        case BATTLEGROUND_AV:
            *data << uint8(1);
            break;
        case BATTLEGROUND_WS:
            *data << uint8(2);
            break;
        case BATTLEGROUND_AB:
            *data << uint8(3);
            break;
        case BATTLEGROUND_NA:
            *data << uint8(4);
            break;
        case BATTLEGROUND_BE:
            *data << uint8(5);
            break;
        case BATTLEGROUND_AA:
            *data << uint8(6);
            break;
        case BATTLEGROUND_EY:
            *data << uint8(7);
            break;
        case BATTLEGROUND_RL:
            *data << uint8(8);
            break;
        default:                                            // unknown
            *data << uint8(0);
            break;
    }

    if(bg->isArena() && (StatusID == STATUS_WAIT_QUEUE))
        *data << uint32(BATTLEGROUND_AA);                   // all arenas   I don't think so.
    else
    *data << uint32(bg->GetTypeID());                   // BG id from DBC

    *data << uint16(0x1F90);                                // unk value 8080
    *data << uint32(bg->GetInstanceID());                   // instance id

    if(bg->isBattleGround())
        *data << uint8(bg->GetTeamIndexByTeamId(team));     // team
    else
        *data << uint8(israted?israted:bg->isRated());                      // is rated battle
*/
    *data << uint32(StatusID);                              // status
    switch(StatusID)
    {
        case STATUS_WAIT_QUEUE:                             // status_in_queue
            *data << uint32(Time1);                         // average wait time, milliseconds
            *data << uint32(Time2);                         // time in queue, updated every minute!, milliseconds
            break;
        case STATUS_WAIT_JOIN:                              // status_invite
            *data << uint32(bg->GetMapId());                // map id
            *data << uint32(Time1);                         // time to remove from queue, milliseconds
            break;
        case STATUS_IN_PROGRESS:                            // status_in_progress
            *data << uint32(bg->GetMapId());                // map id
            *data << uint32(Time1);                         // time to bg auto leave, 0 at bg start, 120000 after bg end, milliseconds
            *data << uint32(Time2);                         // time from bg start, milliseconds
            *data << uint8(0x1);                            // Lua_GetBattlefieldArenaFaction (bool)
            break;
        default:
            sLog.outError("Unknown BG status!");
            break;
    }
}

void BattleGroundMgr::BuildPvpLogDataPacket(WorldPacket *data, BattleGround *bg)
{
    uint8 type = (bg->isArena() ? 1 : 0);
                                                            // last check on 2.4.1
    data->Initialize(MSG_PVP_LOG_DATA, (1+1+4+40*bg->GetPlayerScoresSize()));
    *data << uint8(type);                                   // type (battleground=0/arena=1)

    if(type)                                                // arena
    {
        // it seems this must be according to BG_WINNER_A/H and _NOT_ BG_TEAM_A/H
        for(int i = 1; i >= 0; --i)
        {
            *data << uint32(3000-bg->m_ArenaTeamRatingChanges[i]);                      // rating change: showed value - 3000
            *data << uint32(3999);                          // huge thanks for TOM_RUS for this!
            DEBUG_LOG("rating change: %d", bg->m_ArenaTeamRatingChanges[i]);
        }
        for(int i = 1; i >= 0; --i)
        {
            uint32 at_id = bg->m_ArenaTeamIds[i];
            ArenaTeam * at = sObjectMgr.GetArenaTeamById(at_id);
            if (at)
                *data << at->GetName();
            else
                *data << (uint8)0;
        }
    }

    if (bg->GetStatus() != STATUS_WAIT_LEAVE)
    {
        *data << uint8(0);                                  // bg not ended
    }
    else
    {
        *data << uint8(1);                                  // bg ended
        *data << uint8(bg->GetWinner());                    // who win
    }

    *data << (int32)(bg->GetPlayerScoresSize());

    for(BattleGround::BattleGroundScoreMap::const_iterator itr = bg->GetPlayerScoresBegin(); itr != bg->GetPlayerScoresEnd(); ++itr)
    {
        *data << (uint64)itr->first;
        *data << (int32)itr->second->KillingBlows;
        if (type == 0)
        {
            *data << (int32)itr->second->HonorableKills;
            *data << (int32)itr->second->Deaths;
            *data << (int32)(itr->second->BonusHonor);
        }
        else
        {
            Player *plr = sObjectMgr.GetPlayer(itr->first);
            uint32 team = bg->GetPlayerTeam(itr->first);
            if (!team && plr)
                team = plr->GetTeam();
            if (( bg->GetWinner()==0 && team == ALLIANCE ) || ( bg->GetWinner()==1 && team==HORDE ))
                *data << uint8(1);
            else
                *data << uint8(0);
        }
        *data << (int32)itr->second->DamageDone;             // damage done
        *data << (int32)itr->second->HealingDone;            // healing done
        switch(bg->GetTypeID())                              // battleground specific things
        {
            case BATTLEGROUND_AV:
                *data << (uint32)0x00000005;                // count of next fields
                *data << (uint32)((BattleGroundAVScore*)itr->second)->GraveyardsAssaulted;  // GraveyardsAssaulted
                *data << (uint32)((BattleGroundAVScore*)itr->second)->GraveyardsDefended;   // GraveyardsDefended
                *data << (uint32)((BattleGroundAVScore*)itr->second)->TowersAssaulted;      // TowersAssaulted
                *data << (uint32)((BattleGroundAVScore*)itr->second)->TowersDefended;       // TowersDefended
                *data << (uint32)((BattleGroundAVScore*)itr->second)->SecondaryObjectives;  // SecondaryObjectives - free some of the Lieutnants
                break;
            case BATTLEGROUND_WS:
                *data << (uint32)0x00000002;                // count of next fields
                *data << (uint32)((BattleGroundWGScore*)itr->second)->FlagCaptures;         // flag captures
                *data << (uint32)((BattleGroundWGScore*)itr->second)->FlagReturns;          // flag returns
                break;
            case BATTLEGROUND_AB:
                *data << (uint32)0x00000002;                // count of next fields
                *data << (uint32)((BattleGroundABScore*)itr->second)->BasesAssaulted;       // bases asssulted
                *data << (uint32)((BattleGroundABScore*)itr->second)->BasesDefended;        // bases defended
                break;
            case BATTLEGROUND_EY:
                *data << (uint32)0x00000001;                // count of next fields
                *data << (uint32)((BattleGroundEYScore*)itr->second)->FlagCaptures;         // flag captures
                break;
            case BATTLEGROUND_NA:
            case BATTLEGROUND_BE:
            case BATTLEGROUND_AA:
            case BATTLEGROUND_RL:
                *data << (int32)0;                          // 0
                break;
            default:
                DEBUG_LOG("Unhandled MSG_PVP_LOG_DATA for BG id %u", bg->GetTypeID());
                *data << (int32)0;
                break;
        }
    }
}

void BattleGroundMgr::BuildGroupJoinedBattlegroundPacket(WorldPacket *data, BattleGroundTypeId bgTypeId)
{
    /*bgTypeId is:
    0 - Your group has joined a battleground queue, but you are not eligible
    1 - Your group has joined the queue for AV
    2 - Your group has joined the queue for WS
    3 - Your group has joined the queue for AB
    4 - Your group has joined the queue for NA
    5 - Your group has joined the queue for BE Arena
    6 - Your group has joined the queue for All Arenas
    7 - Your group has joined the queue for EotS*/
    data->Initialize(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
    *data << uint32(bgTypeId);
}

void BattleGroundMgr::BuildUpdateWorldStatePacket(WorldPacket *data, uint32 field, uint32 value)
{
    data->Initialize(SMSG_UPDATE_WORLD_STATE, 4+4);
    *data << uint32(field);
    *data << uint32(value);
}

void BattleGroundMgr::BuildPlaySoundPacket(WorldPacket *data, uint32 soundid)
{
    data->Initialize(SMSG_PLAY_SOUND, 4);
    *data << uint32(soundid);
}

void BattleGroundMgr::BuildPlayerLeftBattleGroundPacket(WorldPacket *data, Player *plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_LEFT, 8);
    *data << uint64(plr->GetGUID());
}

void BattleGroundMgr::BuildPlayerJoinedBattleGroundPacket(WorldPacket *data, Player *plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_JOINED, 8);
    *data << plr->GetObjectGuid();
}

void BattleGroundMgr::InvitePlayer(Player* plr, uint32 bgInstanceGUID, BattleGroundTypeId bgTypeId, uint32 team)
{
    // set invited player counters:
    BattleGround* bg = GetBattleGround(bgInstanceGUID, bgTypeId);
    if(!bg)
        return;
    bg->IncreaseInvitedCount(team);

    plr->SetInviteForBattleGroundQueueType(BGQueueTypeId(bg->GetTypeID(),bg->GetArenaType()), bgInstanceGUID);

    // set the arena teams for rated matches
    if(bg->isArena() && bg->isRated())
    {
        switch(bg->GetArenaType())
        {
        case ARENA_TYPE_2v2:
            bg->SetArenaTeamIdForTeam(team, plr->GetArenaTeamId(0));
            break;
        case ARENA_TYPE_3v3:
            bg->SetArenaTeamIdForTeam(team, plr->GetArenaTeamId(1));
            break;
        case ARENA_TYPE_5v5:
            bg->SetArenaTeamIdForTeam(team, plr->GetArenaTeamId(2));
            break;
        default:
            break;
        }
    }

    // create invite events:
    //add events to player's counters ---- this is not good way - there should be something like global event processor, where we should add those events
    BGQueueInviteEvent* inviteEvent = new BGQueueInviteEvent(plr->GetGUID(), bgInstanceGUID, bgTypeId);
    plr->m_Events.AddEvent(inviteEvent, plr->m_Events.CalculateTime(INVITATION_REMIND_TIME));
    BGQueueRemoveEvent* removeEvent = new BGQueueRemoveEvent(plr->GetGUID(), bgInstanceGUID, bgTypeId, team);
    plr->m_Events.AddEvent(removeEvent, plr->m_Events.CalculateTime(INVITE_ACCEPT_WAIT_TIME));
}

BattleGround * BattleGroundMgr::GetBattleGround(uint32 InstanceID, BattleGroundTypeId bgTypeId)
{
    //search if needed
    BattleGroundSet::iterator itr;
    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        for(uint32 i = BATTLEGROUND_AV; i < MAX_BATTLEGROUND_TYPE_ID; i++)
        {
            itr = m_BattleGrounds[i].find(InstanceID);
            if (itr != m_BattleGrounds[i].end())
                return itr->second;
        }
        return NULL;
    }
    itr = m_BattleGrounds[bgTypeId].find(InstanceID);
    return ( (itr != m_BattleGrounds[bgTypeId].end()) ? itr->second : NULL );
}

BattleGround * BattleGroundMgr::GetBattleGroundTemplate(BattleGroundTypeId bgTypeId)
{
    //map is sorted and we can be sure that lowest instance id has only BG template
    return m_BattleGrounds[bgTypeId].empty() ? NULL : m_BattleGrounds[bgTypeId].begin()->second;
}

// create a new battleground that will really be used to play
BattleGround * BattleGroundMgr::CreateNewBattleGround(BattleGroundTypeId bgTypeId, BattleGroundBracketId bracket_id, uint8 arenaType, bool isRated)
{
    // get the template BG
    BattleGround *bg_template = GetBattleGroundTemplate(bgTypeId);
    if (!bg_template)
    {
        sLog.outError("BattleGround: CreateNewBattleGround - bg template not found for %u", bgTypeId);
        return NULL;
    }

    //for arenas there is random map used
    if (bg_template->isArena())
    {
        BattleGroundTypeId arenas[] = {BATTLEGROUND_NA, BATTLEGROUND_BE, BATTLEGROUND_RL};
        uint32 arena_num = urand(0,2);
        bgTypeId = arenas[arena_num];
        bg_template = GetBattleGroundTemplate(bgTypeId);
        if (!bg_template)
        {
            sLog.outError("BattleGround: CreateNewBattleGround - bg template not found for %u", bgTypeId);
            return NULL;
        }
    }

    BattleGround *bg = NULL;
    // create a copy of the BG template
    switch(bgTypeId)
    {
        case BATTLEGROUND_AV:
            bg = new BattleGroundAV(*(BattleGroundAV*)bg_template);
            break;
        case BATTLEGROUND_WS:
            bg = new BattleGroundWS(*(BattleGroundWS*)bg_template);
            break;
        case BATTLEGROUND_AB:
            bg = new BattleGroundAB(*(BattleGroundAB*)bg_template);
            break;
        case BATTLEGROUND_NA:
            bg = new BattleGroundNA(*(BattleGroundNA*)bg_template);
            break;
        case BATTLEGROUND_BE:
            bg = new BattleGroundBE(*(BattleGroundBE*)bg_template);
            break;
        case BATTLEGROUND_AA:
            bg = new BattleGroundAA(*(BattleGroundAA*)bg_template);
            break;
        case BATTLEGROUND_EY:
            bg = new BattleGroundEY(*(BattleGroundEY*)bg_template);
            break;
        case BATTLEGROUND_RL:
            bg = new BattleGroundRL(*(BattleGroundRL*)bg_template);
            break;
        default:
            //error, but it is handled few lines above
            return 0;
    }

    // generate a new instance id
    bg->SetInstanceID(sMapMgr.GenerateInstanceId());         // set instance id

    // reset the new bg (set status to status_wait_queue from status_none)
    bg->Reset();

    // start the joining of the bg
    bg->SetStatus(STATUS_WAIT_JOIN);
    bg->SetBracketId(bracket_id);
    bg->SetArenaType(arenaType);
    bg->SetRated(isRated);

    // add BG to free slot queue
    bg->AddToBGFreeSlotQueue();

    // add bg to update list
    AddBattleGround(bg->GetInstanceID(), bg->GetTypeID(), bg);

    return bg;
}

// used to create the BG templates
uint32 BattleGroundMgr::CreateBattleGround(BattleGroundTypeId bgTypeId, bool IsArena, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam, uint32 LevelMin, uint32 LevelMax, char* BattleGroundName, uint32 MapID, float Team1StartLocX, float Team1StartLocY, float Team1StartLocZ, float Team1StartLocO, float Team2StartLocX, float Team2StartLocY, float Team2StartLocZ, float Team2StartLocO)
{
    // Create the BG
    BattleGround *bg = NULL;
    switch(bgTypeId)
    {
        case BATTLEGROUND_AV: bg = new BattleGroundAV; break;
        case BATTLEGROUND_WS: bg = new BattleGroundWS; break;
        case BATTLEGROUND_AB: bg = new BattleGroundAB; break;
        case BATTLEGROUND_NA: bg = new BattleGroundNA; break;
        case BATTLEGROUND_BE: bg = new BattleGroundBE; break;
        case BATTLEGROUND_AA: bg = new BattleGroundAA; break;
        case BATTLEGROUND_EY: bg = new BattleGroundEY; break;
        case BATTLEGROUND_RL: bg = new BattleGroundRL; break;
        default:              bg = new BattleGround;   break;                           // placeholder for non implemented BG
    }

    bg->SetMapId(MapID);
    bg->SetTypeID(bgTypeId);
    bg->SetInstanceID(0);
    bg->SetArenaorBGType(IsArena);
    bg->SetMinPlayersPerTeam(MinPlayersPerTeam);
    bg->SetMaxPlayersPerTeam(MaxPlayersPerTeam);
    bg->SetMinPlayers(MinPlayersPerTeam * 2);
    bg->SetMaxPlayers(MaxPlayersPerTeam * 2);
    bg->SetName(BattleGroundName);
    bg->SetTeamStartLoc(ALLIANCE, Team1StartLocX, Team1StartLocY, Team1StartLocZ, Team1StartLocO);
    bg->SetTeamStartLoc(HORDE,    Team2StartLocX, Team2StartLocY, Team2StartLocZ, Team2StartLocO);
    bg->SetLevelRange(LevelMin, LevelMax);

    // add bg to update list
    AddBattleGround(bg->GetInstanceID(), bg->GetTypeID(), bg);

    // return some not-null value, bgTypeId is good enough for me
    return bgTypeId;
}

void BattleGroundMgr::CreateInitialBattleGrounds()
{
    uint32 count = 0;

    //                                                0   1                 2                 3      4      5                6              7             8
    QueryResult *result = WorldDatabase.Query("SELECT id, MinPlayersPerTeam,MaxPlayersPerTeam,MinLvl,MaxLvl,AllianceStartLoc,AllianceStartO,HordeStartLoc,HordeStartO FROM battleground_template");

    if (!result)
    {
        barGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 battlegrounds. DB table `battleground_template` is empty.");
        return;
    }

    barGoLink bar((int)result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();
        bar.step();

        uint32 bgTypeID_ = fields[0].GetUInt32();

        // can be overwrite by values from DB
        BattlemasterListEntry const *bl = sBattlemasterListStore.LookupEntry(bgTypeID_);
        if (!bl)
        {
            sLog.outError("Battleground ID %u not found in BattlemasterList.dbc. Battleground not created.", bgTypeID_);
            continue;
        }

        BattleGroundTypeId bgTypeID = BattleGroundTypeId(bgTypeID_);

        bool IsArena = (bl->type == TYPE_ARENA);
        uint32 MinPlayersPerTeam = fields[1].GetUInt32();
        uint32 MaxPlayersPerTeam = fields[2].GetUInt32();
        uint32 MinLvl = fields[3].GetUInt32();
        uint32 MaxLvl = fields[4].GetUInt32();

        //check values from DB
        if (MaxPlayersPerTeam == 0 || MinPlayersPerTeam == 0 || MinPlayersPerTeam > MaxPlayersPerTeam)
        {
            MaxPlayersPerTeam = bl->maxplayersperteam;
            MinPlayersPerTeam = bl->maxplayersperteam / 2;
        }
        if (MinLvl == 0 || MaxLvl == 0 || MinLvl > MaxLvl)
        {
            MinLvl = bl->minlvl;
            MaxLvl = bl->maxlvl;
        }

        float AStartLoc[4];
        float HStartLoc[4];

        uint32 start1 = fields[5].GetUInt32();

        WorldSafeLocsEntry const* start = sWorldSafeLocsStore.LookupEntry(start1);
        if (start)
        {
            AStartLoc[0] = start->x;
            AStartLoc[1] = start->y;
            AStartLoc[2] = start->z;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else if (bgTypeID == BATTLEGROUND_AA)
        {
            AStartLoc[0] = 0;
            AStartLoc[1] = 0;
            AStartLoc[2] = 0;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u have nonexistent WorldSafeLocs.dbc id %u in field `AllianceStartLoc`. BG not created.", bgTypeID, start1);
            continue;
        }

        uint32 start2 = fields[7].GetUInt32();

        start = sWorldSafeLocsStore.LookupEntry(start2);
        if (start)
        {
            HStartLoc[0] = start->x;
            HStartLoc[1] = start->y;
            HStartLoc[2] = start->z;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else if (bgTypeID == BATTLEGROUND_AA)
        {
            HStartLoc[0] = 0;
            HStartLoc[1] = 0;
            HStartLoc[2] = 0;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u have nonexistent WorldSafeLocs.dbc id %u in field `HordeStartLoc`. BG not created.", bgTypeID, start2);
            continue;
        }

        //sLog.outDetail("Creating battleground %s, %u-%u", bl->name[sWorld.GetDBClang()], MinLvl, MaxLvl);
        if (!CreateBattleGround(bgTypeID, IsArena, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl, bl->name[sWorld.GetDefaultDbcLocale()], bl->mapid[0], AStartLoc[0], AStartLoc[1], AStartLoc[2], AStartLoc[3], HStartLoc[0], HStartLoc[1], HStartLoc[2], HStartLoc[3]))
            continue;

        ++count;
    } while (result->NextRow());

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u battlegrounds", count );
}

void BattleGroundMgr::InitAutomaticArenaPointDistribution()
{
    if (sWorld.getConfig(CONFIG_BOOL_ARENA_AUTO_DISTRIBUTE_POINTS))
    {
        DEBUG_LOG("Initializing Automatic Arena Point Distribution");
        QueryResult * result = CharacterDatabase.Query("SELECT NextArenaPointDistributionTime FROM saved_variables");
        if (!result)
        {
            DEBUG_LOG("Battleground: Next arena point distribution time not found in SavedVariables, reseting it now.");
            m_NextAutoDistributionTime = time_t(sWorld.GetGameTime() + BATTLEGROUND_ARENA_POINT_DISTRIBUTION_DAY * sWorld.getConfig(CONFIG_UINT32_ARENA_AUTO_DISTRIBUTE_INTERVAL_DAYS));
            CharacterDatabase.PExecute("INSERT INTO saved_variables (NextArenaPointDistributionTime) VALUES ('"UI64FMTD"')", uint64(m_NextAutoDistributionTime));
        }
        else
        {
            m_NextAutoDistributionTime = time_t((*result)[0].GetUInt64());
            delete result;
        }
        DEBUG_LOG("Automatic Arena Point Distribution initialized.");
    }
}

void BattleGroundMgr::DistributeArenaPoints()
{
    // used to distribute arena points based on last week's stats
    sWorld.SendWorldText(LANG_DIST_ARENA_POINTS_START);

    sWorld.SendWorldText(LANG_DIST_ARENA_POINTS_ONLINE_START);

    //temporary structure for storing maximum points to add values for all players
    std::map<uint32, uint32> PlayerPoints;

    //at first update all points for all team members
    for(ObjectMgr::ArenaTeamMap::iterator team_itr = sObjectMgr.GetArenaTeamMapBegin(); team_itr != sObjectMgr.GetArenaTeamMapEnd(); ++team_itr)
    {
        if (ArenaTeam * at = team_itr->second)
        {
            at->UpdateArenaPointsHelper(PlayerPoints);
        }
    }

    //cycle that gives points to all players
    for (std::map<uint32, uint32>::iterator plr_itr = PlayerPoints.begin(); plr_itr != PlayerPoints.end(); ++plr_itr)
    {
        //update to database
        CharacterDatabase.PExecute("UPDATE characters SET arena_pending_points = '%u' WHERE guid = '%u'", plr_itr->second, plr_itr->first);
        //add points if player is online
        if (Player* pl = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, plr_itr->first)))
            pl->ModifyArenaPoints(plr_itr->second);
    }

    PlayerPoints.clear();

    sWorld.SendWorldText(LANG_DIST_ARENA_POINTS_ONLINE_END);

    sWorld.SendWorldText(LANG_DIST_ARENA_POINTS_TEAM_START);
    for(ObjectMgr::ArenaTeamMap::iterator titr = sObjectMgr.GetArenaTeamMapBegin(); titr != sObjectMgr.GetArenaTeamMapEnd(); ++titr)
    {
        if (ArenaTeam * at = titr->second)
        {
            at->FinishWeek();                              // set played this week etc values to 0 in memory, too
            at->SaveToDB();                                // save changes
            at->NotifyStatsChanged();                      // notify the players of the changes
        }
    }

    sWorld.SendWorldText(LANG_DIST_ARENA_POINTS_TEAM_END);

    sWorld.SendWorldText(LANG_DIST_ARENA_POINTS_END);
}

void BattleGroundMgr::BuildBattleGroundListPacket(WorldPacket *data, ObjectGuid guid, Player* plr, BattleGroundTypeId bgTypeId)
{
    if (!plr)
        return;

    uint32 PlayerLevel = plr->getLevel();

    data->Initialize(SMSG_BATTLEFIELD_LIST);
    *data << guid;                                          // battlemaster guid
    *data << uint32(bgTypeId);                              // battleground id
    if(bgTypeId == BATTLEGROUND_AA)                         // arena
    {
        *data << uint8(5);                                  // unk
        *data << uint32(0);                                 // unk
    }
    else                                                    // battleground
    {
        *data << uint8(0x00);                               // unk

        size_t count_pos = data->wpos();
        uint32 count = 0;
        *data << uint32(0);                                 // number of bg instances

        for(BattleGroundSet::iterator itr = m_BattleGrounds[bgTypeId].begin(); itr != m_BattleGrounds[bgTypeId].end(); ++itr)
        {
              // skip sending battleground template
            if( itr == m_BattleGrounds[bgTypeId].begin() )
                continue;
            if( PlayerLevel >= itr->second->GetMinLevel() && PlayerLevel <= itr->second->GetMaxLevel() )
            {
                *data << uint32(itr->second->GetInstanceID());
                ++count;
            }
        }
        data->put<uint32>( count_pos , count);
    }
}

void BattleGroundMgr::SendToBattleGround(Player *pl, uint32 instanceId, BattleGroundTypeId bgTypeId)
{
    BattleGround *bg = GetBattleGround(instanceId, bgTypeId);
    if (bg)
    {
        uint32 mapid = bg->GetMapId();
        float x, y, z, O;
        uint32 team = pl->GetBGTeam();
        if (team==0)
            team = pl->GetTeam();
        bg->GetTeamStartLoc(team, x, y, z, O);

        DETAIL_LOG("BATTLEGROUND: Sending %s to map %u, X %f, Y %f, Z %f, O %f", pl->GetName(), mapid, x, y, z, O);
        pl->TeleportTo(mapid, x, y, z, O);
    }
    else
    {
        sLog.outError("player %u trying to port to nonexistent bg instance %u",pl->GetGUIDLow(), instanceId);
    }
}

bool BattleGroundMgr::IsArenaType(BattleGroundTypeId bgTypeId)
{
    return ( bgTypeId == BATTLEGROUND_AA ||
        bgTypeId == BATTLEGROUND_BE ||
        bgTypeId == BATTLEGROUND_NA ||
        bgTypeId == BATTLEGROUND_RL );
}

BattleGroundQueueTypeId BattleGroundMgr::BGQueueTypeId(BattleGroundTypeId bgTypeId, uint8 arenaType)
{
    switch(bgTypeId)
    {
        case BATTLEGROUND_WS:
            return BATTLEGROUND_QUEUE_WS;
        case BATTLEGROUND_AB:
            return BATTLEGROUND_QUEUE_AB;
        case BATTLEGROUND_AV:
            return BATTLEGROUND_QUEUE_AV;
        case BATTLEGROUND_EY:
            return BATTLEGROUND_QUEUE_EY;
        case BATTLEGROUND_AA:
        case BATTLEGROUND_NA:
        case BATTLEGROUND_RL:
        case BATTLEGROUND_BE:
            switch(arenaType)
            {
                case ARENA_TYPE_2v2:
                    return BATTLEGROUND_QUEUE_2v2;
                case ARENA_TYPE_3v3:
                    return BATTLEGROUND_QUEUE_3v3;
                case ARENA_TYPE_5v5:
                    return BATTLEGROUND_QUEUE_5v5;
                default:
                    return BATTLEGROUND_QUEUE_NONE;
            }
        default:
            return BATTLEGROUND_QUEUE_NONE;
    }
}

BattleGroundTypeId BattleGroundMgr::BGTemplateId(BattleGroundQueueTypeId bgQueueTypeId)
{
    switch(bgQueueTypeId)
    {
        case BATTLEGROUND_QUEUE_WS:
            return BATTLEGROUND_WS;
        case BATTLEGROUND_QUEUE_AB:
            return BATTLEGROUND_AB;
        case BATTLEGROUND_QUEUE_AV:
            return BATTLEGROUND_AV;
        case BATTLEGROUND_QUEUE_EY:
            return BATTLEGROUND_EY;
        case BATTLEGROUND_QUEUE_2v2:
        case BATTLEGROUND_QUEUE_3v3:
        case BATTLEGROUND_QUEUE_5v5:
            return BATTLEGROUND_AA;
        default:
            return BattleGroundTypeId(0);                   // used for unknown template (it exist and do nothing)
    }
}

uint8 BattleGroundMgr::BGArenaType(BattleGroundQueueTypeId bgQueueTypeId)
{
    switch(bgQueueTypeId)
    {
        case BATTLEGROUND_QUEUE_2v2:
            return ARENA_TYPE_2v2;
        case BATTLEGROUND_QUEUE_3v3:
            return ARENA_TYPE_3v3;
        case BATTLEGROUND_QUEUE_5v5:
            return ARENA_TYPE_5v5;
        default:
            return 0;
    }
}

void BattleGroundMgr::ToggleTesting()
{
    m_Testing = !m_Testing;
    if (m_Testing)
        sWorld.SendWorldText(LANG_DEBUG_BG_ON);
    else
        sWorld.SendWorldText(LANG_DEBUG_BG_OFF);
}

void BattleGroundMgr::ToggleArenaTesting()
{
    m_ArenaTesting = !m_ArenaTesting;
    if (m_ArenaTesting)
        sWorld.SendWorldText(LANG_DEBUG_ARENA_ON);
    else
        sWorld.SendWorldText(LANG_DEBUG_ARENA_OFF);
}

uint32 BattleGroundMgr::GetMaxRatingDifference() const
{
    // this is for stupid people who can't use brain and set max rating difference to 0
    uint32 diff = sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_RATING_DIFFERENCE);
    if (diff == 0)
        diff = 5000;
    return diff;
}

uint32 BattleGroundMgr::GetRatingDiscardTimer() const
{
    return sWorld.getConfig(CONFIG_UINT32_ARENA_RATING_DISCARD_TIMER);
}

uint32 BattleGroundMgr::GetPrematureFinishTime() const
{
    return sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_PREMATURE_FINISH_TIMER);
}

void BattleGroundMgr::LoadBattleMastersEntry()
{
    mBattleMastersMap.clear();                              // need for reload case

    QueryResult *result = WorldDatabase.Query( "SELECT entry,bg_template FROM battlemaster_entry" );

    uint32 count = 0;

    if (!result)
    {
        barGoLink bar( 1 );
        bar.step();

        sLog.outString();
        sLog.outString( ">> Loaded 0 battlemaster entries - table is empty!" );
        return;
    }

    barGoLink bar( (int)result->GetRowCount() );

    do
    {
        ++count;
        bar.step();

        Field *fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        uint32 bgTypeId  = fields[1].GetUInt32();
        if (!sBattlemasterListStore.LookupEntry(bgTypeId))
        {
            sLog.outErrorDb("Table `battlemaster_entry` contain entry %u for nonexistent battleground type %u, ignored.",entry,bgTypeId);
            continue;
        }

        mBattleMastersMap[entry] = BattleGroundTypeId(bgTypeId);

    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u battlemaster entries", count );
}

HolidayIds BattleGroundMgr::BGTypeToWeekendHolidayId(BattleGroundTypeId bgTypeId)
{
    switch (bgTypeId)
    {
        case BATTLEGROUND_AV: return HOLIDAY_CALL_TO_ARMS_AV;
        case BATTLEGROUND_EY: return HOLIDAY_CALL_TO_ARMS_EY;
        case BATTLEGROUND_WS: return HOLIDAY_CALL_TO_ARMS_WS;
        case BATTLEGROUND_AB: return HOLIDAY_CALL_TO_ARMS_AB;
        default: return HOLIDAY_NONE;
    }
}

BattleGroundTypeId BattleGroundMgr::WeekendHolidayIdToBGType(HolidayIds holiday)
{
    switch (holiday)
    {
        case HOLIDAY_CALL_TO_ARMS_AV: return BATTLEGROUND_AV;
        case HOLIDAY_CALL_TO_ARMS_EY: return BATTLEGROUND_EY;
        case HOLIDAY_CALL_TO_ARMS_WS: return BATTLEGROUND_WS;
        case HOLIDAY_CALL_TO_ARMS_AB: return BATTLEGROUND_AB;
        default: return BATTLEGROUND_TYPE_NONE;
    }
}

bool BattleGroundMgr::IsBGWeekend(BattleGroundTypeId bgTypeId)
{
    return IsHolidayActive(BGTypeToWeekendHolidayId(bgTypeId));
}

void BattleGroundMgr::LoadBattleEventIndexes()
{
    BattleGroundEventIdx events;
    events.event1 = BG_EVENT_NONE;
    events.event2 = BG_EVENT_NONE;
    m_GameObjectBattleEventIndexMap.clear();             // need for reload case
    m_GameObjectBattleEventIndexMap[-1] = events;
    m_CreatureBattleEventIndexMap.clear();               // need for reload case
    m_CreatureBattleEventIndexMap[-1] = events;

    uint32 count = 0;

    QueryResult *result =
        //                           0         1           2                3                4              5           6
        WorldDatabase.Query( "SELECT data.typ, data.guid1, data.ev1 AS ev1, data.ev2 AS ev2, data.map AS m, data.guid2, description.map, "
        //                              7                  8                   9
                                      "description.event1, description.event2, description.description "
                                 "FROM "
                                    "(SELECT '1' AS typ, a.guid AS guid1, a.event1 AS ev1, a.event2 AS ev2, b.map AS map, b.guid AS guid2 "
                                        "FROM gameobject_battleground AS a "
                                        "LEFT OUTER JOIN gameobject AS b ON a.guid = b.guid "
                                     "UNION "
                                     "SELECT '2' AS typ, a.guid AS guid1, a.event1 AS ev1, a.event2 AS ev2, b.map AS map, b.guid AS guid2 "
                                        "FROM creature_battleground AS a "
                                        "LEFT OUTER JOIN creature AS b ON a.guid = b.guid "
                                    ") data "
                                    "RIGHT OUTER JOIN battleground_events AS description ON data.map = description.map "
                                        "AND data.ev1 = description.event1 AND data.ev2 = description.event2 "
        // full outer join doesn't work in mysql :-/ so just UNION-select the same again and add a left outer join
                              "UNION "
                              "SELECT data.typ, data.guid1, data.ev1, data.ev2, data.map, data.guid2, description.map, "
                                      "description.event1, description.event2, description.description "
                                 "FROM "
                                    "(SELECT '1' AS typ, a.guid AS guid1, a.event1 AS ev1, a.event2 AS ev2, b.map AS map, b.guid AS guid2 "
                                        "FROM gameobject_battleground AS a "
                                        "LEFT OUTER JOIN gameobject AS b ON a.guid = b.guid "
                                     "UNION "
                                     "SELECT '2' AS typ, a.guid AS guid1, a.event1 AS ev1, a.event2 AS ev2, b.map AS map, b.guid AS guid2 "
                                        "FROM creature_battleground AS a "
                                        "LEFT OUTER JOIN creature AS b ON a.guid = b.guid "
                                    ") data "
                                    "LEFT OUTER JOIN battleground_events AS description ON data.map = description.map "
                                        "AND data.ev1 = description.event1 AND data.ev2 = description.event2 "
                              "ORDER BY m, ev1, ev2" );
    if(!result)
    {
        barGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 battleground eventindexes.");
        return;
    }

    barGoLink bar((int)result->GetRowCount());

    do
    {
        bar.step();
        Field *fields = result->Fetch();
        if (fields[2].GetUInt8() == BG_EVENT_NONE || fields[3].GetUInt8() == BG_EVENT_NONE)
            continue;                                       // we don't need to add those to the eventmap

        bool gameobject         = (fields[0].GetUInt8() == 1);
        uint32 dbTableGuidLow   = fields[1].GetUInt32();
        events.event1           = fields[2].GetUInt8();
        events.event2           = fields[3].GetUInt8();
        uint32 map              = fields[4].GetUInt32();

        uint32 desc_map = fields[6].GetUInt32();
        uint8 desc_event1 = fields[7].GetUInt8();
        uint8 desc_event2 = fields[8].GetUInt8();
        const char *description = fields[9].GetString();

        // checking for NULL - through right outer join this will mean following:
        if (fields[5].GetUInt32() != dbTableGuidLow)
        {
            sLog.outErrorDb("BattleGroundEvent: %s with nonexistent guid %u for event: map:%u, event1:%u, event2:%u (\"%s\")",
                (gameobject) ? "gameobject" : "creature", dbTableGuidLow, map, events.event1, events.event2, description);
            continue;
        }

        // checking for NULL - through full outer join this can mean 2 things:
        if (desc_map != map)
        {
            // there is an event missing
            if (dbTableGuidLow == 0)
            {
                sLog.outErrorDb("BattleGroundEvent: missing db-data for map:%u, event1:%u, event2:%u (\"%s\")", desc_map, desc_event1, desc_event2, description);
                continue;
            }
            // we have an event which shouldn't exist
            else
            {
                sLog.outErrorDb("BattleGroundEvent: %s with guid %u is registered, for a nonexistent event: map:%u, event1:%u, event2:%u",
                    (gameobject) ? "gameobject" : "creature", dbTableGuidLow, map, events.event1, events.event2);
                continue;
            }
        }

        if (gameobject)
            m_GameObjectBattleEventIndexMap[dbTableGuidLow] = events;
        else
            m_CreatureBattleEventIndexMap[dbTableGuidLow] = events;

        ++count;

    } while(result->NextRow());

    sLog.outString();
    sLog.outString( ">> Loaded %u battleground eventindexes", count);
    delete result;
}
