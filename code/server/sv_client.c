/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// sv_client.c -- server code for dealing with clients

#include "server.h"
#include "../qcommon/md4.h"

static void SV_CloseDownload( client_t *cl );

/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

If we are authorizing, a challenge request will cause a packet
to be sent to the authorize server.

When an authorizeip is returned, a challenge response will be
sent to that ip.

ioquake3: we added a possibility for clients to add a challenge
to their packets, to make it more difficult for malicious servers
to hi-jack client connections.
Also, the auth stuff is completely disabled for com_standalone games
as well as IPv6 connections, since there is no way to use the
v4-only auth server for these new types of connections.
=================
*/
void SV_GetChallenge(netadr_t from)
{
	int		i;
	int		oldest;
	int		oldestTime;
	const char *clientChallenge = Cmd_Argv(1);
	challenge_t	*challenge;

	// ignore if we are in single player
	if ( Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}

	oldest = 0;
	oldestTime = 0x7fffffff;

	// see if we already have a challenge for this ip
	challenge = &svs.challenges[0];
	for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
		if (!challenge->connected && NET_CompareAdr( from, challenge->adr ) ) {
			break;
		}
		if ( challenge->time < oldestTime ) {
			oldestTime = challenge->time;
			oldest = i;
		}
	}

	if (i == MAX_CHALLENGES)
	{
		// this is the first time this client has asked for a challenge
		challenge = &svs.challenges[oldest];
		challenge->clientChallenge = 0;
		challenge->adr = from;
		challenge->firstTime = svs.time;
		challenge->time = svs.time;
		challenge->connected = qfalse;
	}

	// always generate a new challenge number, so the client cannot circumvent sv_maxping
	challenge->challenge = ( (rand() << 16) ^ rand() ) ^ svs.time;
	challenge->wasrefused = qfalse;


#ifndef STANDALONE
	// Drop the authorize stuff if this client is coming in via v6 as the auth server does not support ipv6.
	// Drop also for addresses coming in on local LAN and for stand-alone games independent from id's assets.
	if(challenge->adr.type == NA_IP && !Cvar_VariableIntegerValue("com_standalone") && !Sys_IsLANAddress(from))
	{
		// look up the authorize server's IP
		if (svs.authorizeAddress.type == NA_BAD)
		{
			Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
			
			if (NET_StringToAdr(AUTHORIZE_SERVER_NAME, &svs.authorizeAddress, NA_IP))
			{
				svs.authorizeAddress.port = BigShort( PORT_AUTHORIZE );
				Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
					svs.authorizeAddress.ip[0], svs.authorizeAddress.ip[1],
					svs.authorizeAddress.ip[2], svs.authorizeAddress.ip[3],
					BigShort( svs.authorizeAddress.port ) );
			}
		}

		// we couldn't contact the auth server, let them in.
		if(svs.authorizeAddress.type == NA_BAD)
			Com_Printf("Couldn't resolve auth server address\n");

		// if they have been challenging for a long time and we
		// haven't heard anything from the authorize server, go ahead and
		// let them in, assuming the id server is down
		else if(svs.time - challenge->firstTime > AUTHORIZE_TIMEOUT)
			Com_DPrintf( "authorize server timed out\n" );
		else
		{
			// otherwise send their ip to the authorize server
			cvar_t	*fs;
			char	game[1024];

			// If the client provided us with a client challenge, store it...
			if(*clientChallenge)
				challenge->clientChallenge = atoi(clientChallenge);
			
			Com_DPrintf( "sending getIpAuthorize for %s\n", NET_AdrToString( from ));
		
			strcpy(game, BASEGAME);
			fs = Cvar_Get ("fs_game", "", CVAR_INIT|CVAR_SYSTEMINFO );
			if (fs && fs->string[0] != 0) {
				strcpy(game, fs->string);
			}
			
			// the 0 is for backwards compatibility with obsolete sv_allowanonymous flags
			// getIpAuthorize <challenge> <IP> <game> 0 <auth-flag>
			NET_OutOfBandPrint( NS_SERVER, svs.authorizeAddress,
				"getIpAuthorize %i %i.%i.%i.%i %s 0 %s",  challenge->challenge,
				from.ip[0], from.ip[1], from.ip[2], from.ip[3], game, sv_strictAuth->string );
			
			return;
		}
	}
#endif

	challenge->pingTime = svs.time;
	NET_OutOfBandPrint( NS_SERVER, challenge->adr, "challengeResponse %i %s", challenge->challenge, clientChallenge);
}

#ifndef STANDALONE
/*
====================
SV_AuthorizeIpPacket

A packet has been returned from the authorize server.
If we have a challenge adr for that ip, send the
challengeResponse to it
====================
*/
void SV_AuthorizeIpPacket( netadr_t from ) {
	int		challenge;
	int		i;
	char	*s;
	char	*r;
	challenge_t *challengeptr;

	if ( !NET_CompareBaseAdr( from, svs.authorizeAddress ) ) {
		Com_Printf( "SV_AuthorizeIpPacket: not from authorize server\n" );
		return;
	}

	challenge = atoi( Cmd_Argv( 1 ) );

	for (i = 0 ; i < MAX_CHALLENGES ; i++) {
		if ( svs.challenges[i].challenge == challenge ) {
			break;
		}
	}
	if ( i == MAX_CHALLENGES ) {
		Com_Printf( "SV_AuthorizeIpPacket: challenge not found\n" );
		return;
	}
	
	challengeptr = &svs.challenges[i];

	// send a packet back to the original client
	challengeptr->pingTime = svs.time;
	s = Cmd_Argv( 2 );
	r = Cmd_Argv( 3 );			// reason

	if ( !Q_stricmp( s, "demo" ) ) {
		// they are a demo client trying to connect to a real server
		NET_OutOfBandPrint( NS_SERVER, challengeptr->adr, "print\nServer is not a demo server\n" );
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( challengeptr, 0, sizeof( *challengeptr ) );
		return;
	}
	if ( !Q_stricmp( s, "accept" ) ) {
		NET_OutOfBandPrint(NS_SERVER, challengeptr->adr,
			"challengeResponse %d %d", challengeptr->challenge, challengeptr->clientChallenge);
		return;
	}
	if ( !Q_stricmp( s, "unknown" ) ) {
		if (!r) {
			NET_OutOfBandPrint( NS_SERVER, challengeptr->adr, "print\nAwaiting CD key authorization\n" );
		} else {
			NET_OutOfBandPrint( NS_SERVER, challengeptr->adr, "print\n%s\n", r);
		}
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( challengeptr, 0, sizeof( *challengeptr ) );
		return;
	}

	// authorization failed
	if (!r) {
		NET_OutOfBandPrint( NS_SERVER, challengeptr->adr, "print\nSomeone is using this CD Key\n" );
	} else {
		NET_OutOfBandPrint( NS_SERVER, challengeptr->adr, "print\n%s\n", r );
	}

	// clear the challenge record so it won't timeout and let them through
	Com_Memset( challengeptr, 0, sizeof(*challengeptr) );
}
#endif

/*
==================
SV_IsBanned

Check whether a certain address is banned
==================
*/

static qboolean SV_IsBanned(netadr_t *from, qboolean isexception)
{
	int index;
	serverBan_t *curban;
	
	if(!isexception)
	{
		// If this is a query for a ban, first check whether the client is excepted
		if(SV_IsBanned(from, qtrue))
			return qfalse;
	}
	
	for(index = 0; index < serverBansCount; index++)
	{
		curban = &serverBans[index];
		
		if(curban->isexception == isexception)
		{
			if(NET_CompareBaseAdrMask(curban->ip, *from, curban->subnet))
				return qtrue;
		}
	}
	
	return qfalse;
}

char *SV_GetBannedReason(netadr_t *from)
{
	int index;
	serverBan_t *curban;
	
	for(index = 0; index < serverBansCount; index++)
	{
		curban = &serverBans[index];
		
		if(NET_CompareBaseAdrMask(curban->ip, *from, curban->subnet))
			return curban->reason;
		
	}
	
	return sv_bandefaultreason->string;
}

/*
==================
SV_ApproveGuid

Returns a false value if and only if the client with this cl_guid
should not be allowed to enter the server.

A cl_guid string must have length 32 and consist of characters
'0' through '9' and 'A' through 'F'.
==================
*/
qboolean SV_ApproveGuid( const char *guid) {
	int	i;
	char	c;
	int	length;

	if (sv_requireValidGuid->integer > 0) {
		length = strlen(guid); // could avoid this extra linear-time computation
		if (length != 32) { return qfalse; }
		for (i = 31; i >= 0;) {
			c = guid[i--];
			if (!(('0' <= c && c <= '9') ||
				('A' <= c && c <= 'F'))) {
				return qfalse;
			}
		}
	}
	return qtrue;
}

int SV_GetQueuePosition( netadr_t from ) {
	int i;
	for(i=0;i<QueueCount;i++) {
		if (NET_CompareBaseAdr( from, Queue[i])) {
			return i;
		}
	}
	return -1;
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/
#define MAX_QUEUE 10
void SV_DirectConnect( netadr_t from ) {
	char		userinfo[MAX_INFO_STRING];
	int			i;
	client_t	*cl, *newcl;
	client_t	temp;
	sharedEntity_t *ent;
	int			clientNum;
	int			version;
	int			qport;
	int			challenge;
	char		*password;
	int			startIndex;
	intptr_t		denied;
	int			count;
	char		*ip;
	char *reason;

	Com_DPrintf ("SVC_DirectConnect ()\n");
	
	// Check whether this client is banned.
	if(SV_IsBanned(&from, qfalse))
	{
		
		NET_OutOfBandPrint(NS_SERVER, from, "print\n%s\n", SV_GetBannedReason(&from)); //@mission: ban reason
		return;
	}

	Q_strncpyz( userinfo, Cmd_Argv(1), sizeof(userinfo) );

	version = atoi( Info_ValueForKey( userinfo, "protocol" ) );
	if ( version != PROTOCOL_VERSION ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nServer uses protocol version %i.\n", PROTOCOL_VERSION );
		Com_DPrintf ("    rejected connect from version %i\n", version);
		return;
	}

	challenge = atoi( Info_ValueForKey( userinfo, "challenge" ) );
	qport = atoi( Info_ValueForKey( userinfo, "qport" ) );

	// quick reject
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport 
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			if (( svs.time - cl->lastConnectTime) 
				< (sv_reconnectlimit->integer * 1000)) {
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
				return;
			}
			break;
		}
	}
	
	// don't let "ip" overflow userinfo string
	if ( NET_IsLocalAddress (from) )
		ip = "localhost";
	else
		ip = (char *)NET_AdrToString( from );
	if( ( strlen( ip ) + strlen( userinfo ) + 4 ) >= MAX_INFO_STRING ) {
		NET_OutOfBandPrint( NS_SERVER, from,
			"print\nUserinfo string length exceeded.  "
			"Try removing setu cvars from your config.\n" );
		return;
	}
	Info_SetValueForKey( userinfo, "ip", ip );

	// block connections for invalid GUIDs
	if (!SV_ApproveGuid(Info_ValueForKey(userinfo, "cl_guid"))) {
		NET_OutOfBandPrint(NS_SERVER, from, "print\nGet legit, bro.\n");
		Com_DPrintf("Invalid cl_guid, rejected connect from %s\n", NET_AdrToString (from));
		return;
	}

	// block connections from qport 1337
	if (sv_block1337->integer > 0 && qport == 1337) {
		NET_OutOfBandPrint(NS_SERVER, from, "print\nThis server is not for wussies.\n");
		Com_DPrintf("1337 qport, rejected connect from %s\n", NET_AdrToString(from));
		return;
	}

	// see if the challenge is valid (LAN clients don't need to challenge)
	if (!NET_IsLocalAddress(from))
	{
		int ping;
		challenge_t *challengeptr;

		for (i=0; i<MAX_CHALLENGES; i++)
		{
			if (NET_CompareAdr(from, svs.challenges[i].adr))
			{
				if(challenge == svs.challenges[i].challenge)
					break;
			}
		}

		if (i == MAX_CHALLENGES)
		{
			NET_OutOfBandPrint( NS_SERVER, from, "print\nNo or bad challenge for your address.\n" );
			return;
		}
	
		challengeptr = &svs.challenges[i];
		
		if(challengeptr->wasrefused)
		{
			// Return silently, so that error messages written by the server keep being displayed.
			return;
		}

		ping = svs.time - challengeptr->pingTime;

		// never reject a LAN client based on ping
		if ( !Sys_IsLANAddress( from ) ) {
			if ( sv_minPing->value && ping < sv_minPing->value ) {
				NET_OutOfBandPrint( NS_SERVER, from, "print\nServer is for high pings only\n" );
				Com_DPrintf ("Client %i rejected on a too low ping\n", i);
				challengeptr->wasrefused = qtrue;
				return;
			}
			if ( sv_maxPing->value && ping > sv_maxPing->value ) {
				NET_OutOfBandPrint( NS_SERVER, from, "print\nServer is for low pings only\n" );
				Com_DPrintf ("Client %i rejected on a too high ping\n", i);
				challengeptr->wasrefused = qtrue;
				return;
			}
		}

		Com_Printf("Client %i connecting with %i challenge ping\n", i, ping);
		challengeptr->connected = qtrue;
	}

	newcl = &temp;
	Com_Memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this ip, reuse it
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport 
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
			newcl = cl;

			// this doesn't work because it nukes the players userinfo

//			// disconnect the client from the game first so any flags the
//			// player might have are dropped
//			VM_Call( gvm, GAME_CLIENT_DISCONNECT, newcl - svs.clients );
			//
			goto gotnewcl;
		}
	}

	// find a client slot
	// if "sv_privateClients" is set > 0, then that number
	// of client slots will be reserved for connections that
	// have "password" set to the value of "sv_privatePassword"
	// Info requests will report the maxclients as if the private
	// slots didn't exist, to prevent people from trying to connect
	// to a full server.
	// This is to allow us to reserve a couple slots here on our
	// servers so we can play without having to kick people.

	// check for privateClient password
	password = Info_ValueForKey( userinfo, "password" );
	if ( !strcmp( password, sv_privatePassword->string ) ) {
		startIndex = 0;
	} else {
		// skip past the reserved slots
		startIndex = sv_privateClients->integer;
	}

	newcl = NULL;
	int quepos;
	for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
		cl = &svs.clients[i];
		if (cl->state == CS_FREE) {
			if (QueueCount) {
				quepos = SV_GetQueuePosition(from);
				if (quepos == 0) {
					QueueLast[quepos] =svs.time-4000;
					newcl = cl;
					break;
				}
			} else {
				newcl = cl;
				break;
			}
		}
	}
	
	
	if ( !newcl ) {
		if ( NET_IsLocalAddress( from ) ) {
			count = 0;
			for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
				cl = &svs.clients[i];
				if (cl->netchan.remoteAddress.type == NA_BOT) {
					count++;
				}
			}
			// if they're all bots
			if (count >= sv_maxclients->integer - startIndex) {
				SV_DropClient(&svs.clients[sv_maxclients->integer - 1], "only bots on server");
				newcl = &svs.clients[sv_maxclients->integer - 1];
			}
			else {
				Com_Error( ERR_FATAL, "server is full on local connect\n" );
				return;
			}
		}
		else {
			if (QueueCount >= MAX_QUEUE) {
				NET_OutOfBandPrint( NS_SERVER, from, "print\nServer is full.\n" );
				Com_DPrintf ("Rejected a connection.\n");
			} else {
				int pos;
				pos = SV_GetQueuePosition(from);
				if (pos >= 0) {
					QueueLast[pos] = svs.time;
					NET_OutOfBandPrint( NS_SERVER, from, "print\n\nYou are %d/%d in the Queue.\n", pos+1, MAX_QUEUE);
				} else {
					Queue[QueueCount] = from;
					QueueLast[QueueCount] = svs.time;
					int place = QueueCount;
					QueueCount++;
					NET_OutOfBandPrint( NS_SERVER, from, "print\n\nYou are %d/%d in the Queue.\n", place+1, MAX_QUEUE);
				}
			}
			return;
		}
	}

	// we got a newcl, so reset the reliableSequence and reliableAcknowledge
	cl->reliableAcknowledge = 0;
	cl->reliableSequence = 0;

gotnewcl:	
	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	*newcl = temp;
	clientNum = newcl - svs.clients;
	ent = SV_GentityNum( clientNum );
	newcl->gentity = ent;

	// save the challenge
	newcl->challenge = challenge;

	// save the address
	Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);
	// init the netchan queue
	newcl->netchan_end_queue = &newcl->netchan_start_queue;

	// clear server-side demo recording
	newcl->demo_recording = qfalse;
	newcl->demo_file = -1;
	newcl->demo_waiting = qfalse;
	newcl->demo_backoff = 1;
	newcl->demo_deltas = 0;

	// save the userinfo
	Q_strncpyz( newcl->userinfo, userinfo, sizeof(newcl->userinfo) );

	// get the game a chance to reject this connection or modify the userinfo
	denied = VM_Call( gvm, GAME_CLIENT_CONNECT, clientNum, qtrue, qfalse ); // firstTime = qtrue
	if ( denied ) {
		// we can't just use VM_ArgPtr, because that is only valid inside a VM_Call
		char *str = VM_ExplicitArgPtr( gvm, denied );

		NET_OutOfBandPrint( NS_SERVER, from, "print\n%s\n", str );
		Com_DPrintf ("Game rejected a connection: %s.\n", str);
		return;
	}

	SV_UserinfoChanged( newcl );

	// send the connect packet to the client
	NET_OutOfBandPrint( NS_SERVER, from, "connectResponse" );

	Com_DPrintf( "Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name );

	newcl->state = CS_CONNECTED;
	newcl->nextSnapshotTime = svs.time;
	newcl->lastPacketTime = svs.time;
	newcl->lastConnectTime = svs.time;
	
	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	newcl->gamestateMessageNum = -1;

	// if this was the first client on the server, or the last client
	// the server can hold, send a heartbeat to the master.
	count = 0;
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
		}
	}
	if ( count == 1 || count == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	challenge_t	*challenge;
	const qboolean isBot = drop->netchan.remoteAddress.type == NA_BOT;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	if ( !isBot ) {
		// see if we already have a challenge for this ip
		challenge = &svs.challenges[0];

		for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++)
		{
			if(NET_CompareAdr(drop->netchan.remoteAddress, challenge->adr))
			{
				Com_Memset(challenge, 0, sizeof(*challenge));
				break;
			}
		}
	}

	// Kill any download
	SV_CloseDownload( drop );

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );

	if (drop->download)	{
		FS_FCloseFile( drop->download );
		drop->download = 0;
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", reason);

	if ( isBot ) {
		SV_BotFreeClient( drop - svs.clients );
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );
	
	if ( isBot ) {
		// bots shouldn't go zombie, as there's no real net connection.
		drop->state = CS_FREE;
	} else {
		Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
		drop->state = CS_ZOMBIE;		// become free in a few seconds
	}

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}
	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
static void SV_SendClientGameState( client_t *client ) {
	int			start;
	entityState_t	*base, nullstate;
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

 	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	client->state = CS_PRIMED;
	client->pureAuthentic = 0;
	client->gotCP = qfalse;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( &msg, svc_configstring );
			MSG_WriteShort( &msg, start );
			MSG_WriteBigString( &msg, sv.configstrings[start] );
		}
	}

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, base, qtrue );
	}

	MSG_WriteByte( &msg, svc_EOF );

	MSG_WriteLong( &msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( &msg, sv.checksumFeed);

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

/*
===================
SV_UrT_FreeForAll_Kludge

In UrT when g_gametype is switched to "Free for All" from
a team-based gametype, bad things happen: Players will be
on colored teams but colored teams don't exist so they do
not show up on the score board. They also seem to change
colors when shot. Fun and all, but not very convenient if
you want to run a mixed-mode server. Thus this kludge.
===================
*/
static void SV_UrT_FreeForAll_Kludge(client_t *client)
{
	int slot, team;
	playerState_t *state;

	if (Cvar_VariableValue("g_gametype") == GT_FFA) {
		slot = client - svs.clients;
		state = SV_GameClientNum(slot);
		team = state->persistant[PERS_TEAM];
		Com_DPrintf("SV_UrT_FreeForAll_Kludge() found team %i for player %i\n", team, slot);

		if (team == TEAM_RED || team == TEAM_BLUE) {
			Cmd_ExecuteString (va("forceteam %i spectator", slot));
			Cmd_ExecuteString (va("forceteam %i ffa", slot));
			Com_Printf("SV_UrT_FreeForAll_Kludge() forced player %i to team ffa\n", slot);
		}
	}
}

/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->deltaMessage = -1;
	client->nextSnapshotTime = svs.time;	// generate a snapshot immediately
	client->lastUsercmd = *cmd;

	// call the game begin function
	VM_Call( gvm, GAME_CLIENT_BEGIN, client - svs.clients );

	// this has to be called *after* the UrT game code; it's funny: before the
	// UrT game code runs, you're actually on team 0 as you should be for FFA;
	// the UrT game code forces you back on your old team because it's insane;
	// then we force it back (if we have to) in the kludge; wow :-/ [mad]
	SV_UrT_FreeForAll_Kludge(client);
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free( cl->downloadBlocks[i] );
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
static void SV_StopDownload_f( client_t *cl ) {
	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", (int) (cl - svs.clients), cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished
==================
*/
static void SV_DoneDownload_f( client_t *cl ) {
	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
static void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", (int) (cl - svs.clients), block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", (int) (cl - svs.clients), cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
static void SV_BeginDownload_f( client_t *cl ) {

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data 
==================
*/
void SV_WriteDownloadToClient( client_t *cl , msg_t *msg )
{
	int curindex;
	int rate;
	int blockspersnap;
	int idPack = 0, missionPack = 0, unreferenced = 1;
	char errorMessage[1024];
	char pakbuf[MAX_QPATH], *pakptr;
	int numRefPaks;

	if (!*cl->downloadName)
		return;	// Nothing being downloaded

	if (!cl->download) {
 		// Chop off filename extension.
		Com_sprintf(pakbuf, sizeof(pakbuf), "%s", cl->downloadName);
		pakptr = Q_strrchr(pakbuf, '.');
		
		if(pakptr)
		{
			*pakptr = '\0';

			// Check for pk3 filename extension
			if(!Q_stricmp(pakptr + 1, "pk3"))
			{
				const char *referencedPaks = FS_ReferencedPakNames();

				// Check whether the file appears in the list of referenced
				// paks to prevent downloading of arbitrary files.
				Cmd_TokenizeStringIgnoreQuotes(referencedPaks);
				numRefPaks = Cmd_Argc();

				for(curindex = 0; curindex < numRefPaks; curindex++)
				{
					if(!FS_FilenameCompare(Cmd_Argv(curindex), pakbuf))
					{
						unreferenced = 0;

						// now that we know the file is referenced,
						// check whether it's legal to download it.
						missionPack = FS_idPak(pakbuf, "missionpack");
						idPack = missionPack || FS_idPak(pakbuf, BASEGAME);

						break;
					}
				}
			}
		}

		cl->download = 0;

		// We open the file here
		if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
			(sv_allowDownload->integer & DLF_NO_UDP) ||
			idPack || unreferenced ||
			( cl->downloadSize = FS_SV_FOpenFileRead( cl->downloadName, &cl->download ) ) < 0 ) {
			// cannot auto-download file
			if(unreferenced)
			{
				Com_Printf("clientDownload: %d : \"%s\" is not referenced and cannot be downloaded.\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" is not referenced and cannot be downloaded.", cl->downloadName);
			}
			else if (idPack) {
				Com_Printf("clientDownload: %d : \"%s\" cannot download id pk3 files\n", (int) (cl - svs.clients), cl->downloadName);
				if (missionPack) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload Team Arena file \"%s\"\n"
									"The Team Arena mission pack can be found in your local game store.", cl->downloadName);
				}
				else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload id pk3 file \"%s\"", cl->downloadName);
				}
			}
			else if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
				(sv_allowDownload->integer & DLF_NO_UDP) ) {

				Com_Printf("clientDownload: %d : \"%s\" download disabled", (int) (cl - svs.clients), cl->downloadName);
				if (sv_pure->integer) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
                    "The server you are connecting to is not a pure server, "
                    "set autodownload to No in your settings and you might be "
                    "able to join the game anyway.\n", cl->downloadName);
				}
			} else {
        // NOTE TTimo this is NOT supposed to happen unless bug in our filesystem scheme?
        //   if the pk3 is referenced, it must have been found somewhere in the filesystem
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;
			
			if(cl->download)
				FS_FCloseFile(cl->download);
			
			return;
		}
 
		Com_Printf( "clientDownload: %d : beginning \"%s\"\n", (int) (cl - svs.clients), cl->downloadName );
		
		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = Z_Malloc( MAX_DOWNLOAD_BLKSIZE );

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	// Loop up to window size times based on how many blocks we can fit in the
	// client snapMsec and rate

	// based on the rate, how many bytes can we fit in the snapMsec time of the client
	// normal rate / snapshotMsec calculation
	rate = cl->rate;
	if ( sv_maxRate->integer ) {
		if ( sv_maxRate->integer < 1000 ) {
			Cvar_Set( "sv_MaxRate", "1000" );
		}
		if ( sv_maxRate->integer < rate ) {
			rate = sv_maxRate->integer;
		}
	}
	if ( sv_minRate->integer ) {
		if ( sv_minRate->integer < 1000 )
			Cvar_Set( "sv_minRate", "1000" );
		if ( sv_minRate->integer > rate )
			rate = sv_minRate->integer;
	}

	if (!rate) {
		blockspersnap = 1;
	} else {
		blockspersnap = ( (rate * cl->snapshotMsec) / 1000 + MAX_DOWNLOAD_BLKSIZE ) /
			MAX_DOWNLOAD_BLKSIZE;
	}

	if (blockspersnap < 0)
		blockspersnap = 1;

	while (blockspersnap--) {

		// Write out the next section of the file, if we have already reached our window,
		// automatically start retransmitting

		if (cl->downloadClientBlock == cl->downloadCurrentBlock)
			return; // Nothing to transmit

		if (cl->downloadXmitBlock == cl->downloadCurrentBlock) {
			// We have transmitted the complete window, should we start resending?

			//FIXME:  This uses a hardcoded one second timeout for lost blocks
			//the timeout should be based on client rate somehow
			if (svs.time - cl->downloadSendTime > 1000)
				cl->downloadXmitBlock = cl->downloadClientBlock;
			else
				return;
		}

		// Send current block
		curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, cl->downloadXmitBlock );

		// block zero is special, contains file size
		if ( cl->downloadXmitBlock == 0 )
			MSG_WriteLong( msg, cl->downloadSize );
 
		MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

		// Write the block
		if ( cl->downloadBlockSize[curindex] ) {
			MSG_WriteData( msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex] );
		}

		Com_DPrintf( "clientDownload: %d : writing block %d\n", (int) (cl - svs.clients), cl->downloadXmitBlock );

		// Move on to the next block
		// It will get sent with next snap shot.  The rate will keep us in line.
		cl->downloadXmitBlock++;

		cl->downloadSendTime = svs.time;
	}
}

#ifdef USE_VOIP
/*
==================
SV_WriteVoipToClient

Check to see if there is any VoIP queued for a client, and send if there is.
==================
*/
void SV_WriteVoipToClient( client_t *cl, msg_t *msg )
{
	voipServerPacket_t *packet = &cl->voipPacket[0];
	int totalbytes = 0;
	int i;

	if (*cl->downloadName) {
		cl->queuedVoipPackets = 0;
		return;  // no VoIP allowed if download is going, to save bandwidth.
	}

	// Write as many VoIP packets as we reasonably can...
	for (i = 0; i < cl->queuedVoipPackets; i++, packet++) {
		totalbytes += packet->len;
		if (totalbytes > MAX_DOWNLOAD_BLKSIZE)
			break;

		// You have to start with a svc_EOF, so legacy clients drop the
		//  rest of this packet. Otherwise, those without VoIP support will
		//  see the svc_voip command, then panic and disconnect.
		// Generally we don't send VoIP packets to legacy clients, but this
		//  serves as both a safety measure and a means to keep demo files
		//  compatible.
		MSG_WriteByte( msg, svc_EOF );
		MSG_WriteByte( msg, svc_extension );
		MSG_WriteByte( msg, svc_voip );
		MSG_WriteShort( msg, packet->sender );
		MSG_WriteByte( msg, (byte) packet->generation );
		MSG_WriteLong( msg, packet->sequence );
		MSG_WriteByte( msg, packet->frames );
		MSG_WriteShort( msg, packet->len );
		MSG_WriteData( msg, packet->data, packet->len );
	}

	// !!! FIXME: I hate this queue system.
	cl->queuedVoipPackets -= i;
	if (cl->queuedVoipPackets > 0) {
		memmove( &cl->voipPacket[0], &cl->voipPacket[i],
		         sizeof (voipServerPacket_t) * i);
	}
}
#endif


/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
static void SV_Disconnect_f( client_t *cl ) {
	Com_Printf("SV_Disconnect_f\n");
	// stop server-side demo (if any)
	if (cl->demo_recording) {
		Cbuf_ExecuteText(EXEC_NOW, va("stopserverdemo %d", (int)(cl-svs.clients)));
	}
	SV_DropClient( cl, "disconnected" );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from 
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( sv_pure->integer != 0 ) {

		bGood = qtrue;
		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		bGood = (FS_FileIsInPAK("vm/cgame.qvm", &nChkSum1) == 1);
		if (bGood)
			bGood = (FS_FileIsInPAK("vm/ui.qvm", &nChkSum2) == 1);

		nClientPaks = Cmd_Argc();

		// start at arg 2 ( skip serverId cl_paks )
		nCurArg = 1;

		pArg = Cmd_Argv(nCurArg++);
		if(!pArg) {
			bGood = qfalse;
		}
		else
		{
			// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=475
			// we may get incoming cp sequences from a previous checksumFeed, which we need to ignore
			// since serverId is a frame count, it always goes up
			if (atoi(pArg) < sv.checksumFeedServerId)
			{
				Com_DPrintf("ignoring outdated cp command from client %s\n", cl->name);
				return;
			}
		}
	
		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		cl->gotCP = qtrue;

		if (bGood) {
			cl->pureAuthentic = 1;
		} 
		else {
			cl->pureAuthentic = 0;
			cl->nextSnapshotTime = -1;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure client detected. Invalid .PK3 files referenced!" );
		}
	}
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
	cl->gotCP = qfalse;
}

/*
==================
SV_SendUserinfoToAlphaHub

Send userinfo string to hub server if we were able to resolve it.
Hub messages are authenticated using a secret key and MD4 digests
(mostly because MD4 was available in ioq3 already).
==================
*/
void SV_SendUserinfoToAlphaHub(const char *userinfo)
{
	int length;

	// we use this buffer for both computing the MD4 and then the final
	// message we send; the MD4 hexdigest is 32 chars long, less than
	// MAX_CVAR_VALUE_STRING, so we're good (but still add safety); the
	// contents are "key-or-MD4\nuserinfo\nthelonguserinfodata" which
	// should explain the length calculation
	char buffer[MAX_CVAR_VALUE_STRING+1+8+1+MAX_INFO_STRING+4];

	// these buffers are for the actual MD4 and its hexdigest, each with
	// a small safety margin
	char md4[16+2];
	char digest[32+2];

	if (svs.alphaHubAddress.type != NA_BAD) {
		// initialize the buffers (paranoid!)
		memset(buffer, 0, sizeof(buffer));
		memset(md4, 0, sizeof(md4));
		memset(digest, 0, sizeof(digest));

		// key + payload to authenticate
		length = Q_snprintf(buffer, sizeof(buffer)-2, "%s\nuserinfo\n%s",
				sv_alphaHubKey->string, userinfo);
		assert(length != -1);
		assert(length <= sizeof(buffer)-2);
		assert(length == strlen(buffer));

		// [mad] silly (byte*) casts to avoid silly warnings;
		// the types in ioq3 are a mess...
		mdfour((byte*) md4, (byte*) buffer, length);
		mdfour_hex((byte*) md4, digest);
		assert(strlen(digest) == 32);

		// MD4 hexdigest + payload to send
		length = Q_snprintf(buffer, sizeof(buffer)-2, "%s\nuserinfo\n%s",
				digest, userinfo);
		assert(length != -1);
		assert(length <= sizeof(buffer)-2);

		NET_OutOfBandPrint(NS_SERVER, svs.alphaHubAddress,
			"%s", buffer);

		Com_DPrintf("Sent userinfo to |ALPHA| Hub.\n");
	}
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val;
	char	*ip;
	int		i;
	int	len;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && com_dedicated->integer != 2 && sv_lanForceRate->integer == 1) {
		cl->rate = 99999;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (strlen(val)) {
			i = atoi(val);
			cl->rate = i;
			if (cl->rate < 1000) {
				cl->rate = 1000;
			} else if (cl->rate > 90000) {
				cl->rate = 90000;
			}
		} else {
			cl->rate = 3000;
		}
	}
	val = Info_ValueForKey (cl->userinfo, "handicap");
	if (strlen(val)) {
		i = atoi(val);
		if (i<=0 || i>100 || strlen(val) > 4) {
			Info_SetValueForKey( cl->userinfo, "handicap", "100" );
		}
	}

	// snaps command
	val = Info_ValueForKey (cl->userinfo, "snaps");
	if (strlen(val)) {
		i = atoi(val);
		if ( i < 1 ) {
			i = 1;
		} else if ( i > sv_fps->integer ) {
			i = sv_fps->integer;
		}
		cl->snapshotMsec = 1000/i;
	} else {
		cl->snapshotMsec = 50;
	}
	
#ifdef USE_VOIP
	// in the future, (val) will be a protocol version string, so only
	//  accept explicitly 1, not generally non-zero.
	val = Info_ValueForKey (cl->userinfo, "cl_voip");
	cl->hasVoip = (atoi(val) == 1) ? qtrue : qfalse;
#endif

	// TTimo
	// maintain the IP information
	// the banning code relies on this being consistently present
	if( NET_IsLocalAddress(cl->netchan.remoteAddress) )
		ip = "localhost";
	else
		ip = (char*)NET_AdrToString( cl->netchan.remoteAddress );

	val = Info_ValueForKey( cl->userinfo, "ip" );
	if( val[0] )
		len = strlen( ip ) - strlen( val ) + strlen( cl->userinfo );
	else
		len = strlen( ip ) + 4 + strlen( cl->userinfo );

	if( len >= MAX_INFO_STRING )
		SV_DropClient( cl, "userinfo string length exceeded" );
	else
		Info_SetValueForKey( cl->userinfo, "ip", ip );

	SV_SendUserinfoToAlphaHub(cl->userinfo);
}


/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	// rate limit how often players can change their userinfo
	netadr_t from = cl->netchan.remoteAddress;
	if ( cl->state == CS_ACTIVE && from.type != NA_BOT && SVC_RateLimitAddress( from, sv_userinfoDelayBurst->integer, sv_userinfoDelayMillis->integer ) ) {
		Com_DPrintf( "SV_UpdateUserinfo_f: rate limit from %s exceeded, dropping request\n",
			NET_AdrToString( from ) );
		SV_SendServerCommand(cl, "print \"^7You cannot change your userinfo that quickly.\"");
		return;
	}

	Q_strncpyz( cl->userinfo, Cmd_Argv(1), sizeof(cl->userinfo) );

	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, cl - svs.clients );
}


#ifdef USE_VOIP
static
void SV_UpdateVoipIgnore(client_t *cl, const char *idstr, qboolean ignore)
{
	if ((*idstr >= '0') && (*idstr <= '9')) {
		const int id = atoi(idstr);
		if ((id >= 0) && (id < MAX_CLIENTS)) {
			cl->ignoreVoipFromClient[id] = ignore;
		}
	}
}

/*
==================
SV_Voip_f
==================
*/
static void SV_Voip_f( client_t *cl ) {
	const char *cmd = Cmd_Argv(1);
	if (strcmp(cmd, "ignore") == 0) {
		SV_UpdateVoipIgnore(cl, Cmd_Argv(2), qtrue);
	} else if (strcmp(cmd, "unignore") == 0) {
		SV_UpdateVoipIgnore(cl, Cmd_Argv(2), qfalse);
	} else if (strcmp(cmd, "muteall") == 0) {
		cl->muteAllVoip = qtrue;
	} else if (strcmp(cmd, "unmuteall") == 0) {
		cl->muteAllVoip = qfalse;
	}
}
#endif


typedef struct {
	char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},

#ifdef USE_VOIP
	{"voip", SV_Voip_f},
#endif

	{NULL, NULL}
};

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/

// The value below is how many extra characters we reserve for every instance of '$' in a
// ut_radio, say, or similar client command.  Some jump maps have very long $location's.
// On these maps, it may be possible to crash the server if a carefully-crafted
// client command is sent.  The constant below may require further tweaking.  For example,
// a text of "$location" would have a total computed length of 25, because "$location" has
// 9 characters, and we increment that by 16 for the '$'.
#define STRLEN_INCREMENT_PER_DOLLAR_VAR 16

// Don't allow more than this many dollared-strings (e.g. $location) in a client command
// such as ut_radio and say.  Keep this value low for safety, in case some things like
// $location expand to very large strings in some maps.  There is really no reason to have
// more than 6 dollar vars (such as $weapon or $location) in things you tell other people.
#define MAX_DOLLAR_VARS 6

// When a radio text (as in "ut_radio 1 1 text") is sent, weird things start to happen
// when the text gets to be greater than 118 in length.  When the text is really large the
// server will crash.  There is an in-between gray zone above 118, but I don't really want
// to go there.  This is the maximum length of radio text that can be sent, taking into
// account increments due to presence of '$'.
#define MAX_RADIO_STRLEN 118

// Don't allow more than this text length in a command such as say.  I pulled this
// value out of my ass because I don't really know exactly when problems start to happen.
// This value takes into account increments due to the presence of '$'.
#define MAX_SAY_STRLEN 256

void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {
	ucmd_t	*u;
	qboolean bProcessed = qfalse;
	int	argsFromOneMaxlen;
	int	charCount;
	int	dollarCount;
	int	i;
	char	*arg;
	qboolean exploitDetected;
	
	Cmd_TokenizeString( s );

	// see if it is a server level command
	for (u=ucmds ; u->name ; u++) {
		if (!strcmp (Cmd_Argv(0), u->name) ) {
			u->func( cl );
			bProcessed = qtrue;
			break;
		}
	}

	if (clientOK) {
		// pass unknown strings to the game
		if (!u->name && sv.state == SS_GAME) {
			Cmd_Args_Sanitize();

			argsFromOneMaxlen = -1;
			if (Q_stricmp("say", Cmd_Argv(0)) == 0 ||
					Q_stricmp("say_team", Cmd_Argv(0)) == 0) {
				argsFromOneMaxlen = MAX_SAY_STRLEN;
			}
			else if (Q_stricmp("tell", Cmd_Argv(0)) == 0) {
				// A command will look like "tell 12 hi" or "tell foo hi".  The "12"
				// and "foo" in the examples will be counted towards MAX_SAY_STRLEN,
				// plus the space.
				argsFromOneMaxlen = MAX_SAY_STRLEN;
			}
			else if (Q_stricmp("ut_radio", Cmd_Argv(0)) == 0) {
				// We add 4 to this value because in a command such as
				// "ut_radio 1 1 affirmative", the args at indices 1 and 2 each
				// have length 1 and there is a space after them.
				argsFromOneMaxlen = MAX_RADIO_STRLEN + 4;
			}
			if (argsFromOneMaxlen >= 0) {
				exploitDetected = qfalse;
				charCount = 0;
				dollarCount = 0;
				for (i = Cmd_Argc() - 1; i >= 1; i--) {
					arg = Cmd_Argv(i);
					while (*arg) {
						if (++charCount > argsFromOneMaxlen) {
							exploitDetected = qtrue; break;
						}
						if (*arg == '$') {
							if (++dollarCount > MAX_DOLLAR_VARS) {
								exploitDetected = qtrue; break;
							}
							charCount += STRLEN_INCREMENT_PER_DOLLAR_VAR;
							if (charCount > argsFromOneMaxlen) {
								exploitDetected = qtrue; break;
							}
						}
						arg++;
					}
					if (exploitDetected) { break; }
					if (i != 1) { // Cmd_ArgsFrom() will add space
						if (++charCount > argsFromOneMaxlen) {
							exploitDetected = qtrue; break;
						}
					}
				}
				if (exploitDetected) {
					Com_Printf("Buffer overflow exploit radio/say, possible attempt from %s\n",
						NET_AdrToString(cl->netchan.remoteAddress));
					SV_DropClient(cl, "talks too much");
					return;
				}
			}

			VM_Call( gvm, GAME_CLIENT_COMMAND, cl - svs.clients );
		}
	}
	else if (!bProcessed)
		Com_DPrintf( "client text ignored for %s: %s\n", cl->name, Cmd_Argv(0) );
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name, 
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players.  If we decide that we want to stall
	// the command, we will stop processing the rest of the packet,
	// including the usercmd.  This causes flooders to lag themselves
	// but not other people
	// We don't do this when the client hasn't been active yet since its
	// normal to spam a lot of commands when downloading
	if ( !com_cl_running->integer && 
		cl->state >= CS_ACTIVE &&
		sv_floodProtect->integer && 
		svs.time < cl->nextReliableTime ) {
		// ignore any other text messages from this client but let them keep playing
		// TTimo - moved the ignored verbose to the actual processing in SV_ExecuteClientCommand, only printing if the core doesn't intercept
		clientOk = qfalse;
	} 

	// don't allow another command for one second
	cl->nextReliableTime = svs.time + 1000;

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	return qtrue;		// continue procesing
}


//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	VM_Call( gvm, GAME_CLIENT_THINK, cl - svs.clients );
}

/*
==================
SV_UserMove

The message usually contains all the movement commands 
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;

	if ( delta ) {
		cl->deltaMessage = cl->messageAcknowledge;
	} else {
		cl->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= cl->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= MSG_HashKey(cl->reliableCommands[ cl->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		oldcmd = cmd;
	}

	// save time for ping calculation
	cl->frames[ cl->messageAcknowledge & PACKET_MASK ].messageAcked = svs.time;

	// TTimo
	// catch the no-cp-yet situation before SV_ClientEnterWorld
	// if CS_ACTIVE, then it's time to trigger a new gamestate emission
	// if not, then we are getting remaining parasite usermove commands, which we should ignore
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0 && !cl->gotCP) {
		if (cl->state == CS_ACTIVE)
		{
			// we didn't get a cp yet, don't assume anything and just send the gamestate all over again
			Com_DPrintf( "%s: didn't get cp command, resending gamestate\n", cl->name);
			SV_SendClientGameState( cl );
		}
		return;
	}			
	
	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, &cmds[0] );
		// the moves can be processed normaly
	}
	
	// a bad cp command was sent, drop the client
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0) {		
		SV_DropClient( cl, "Cannot validate pure client!");
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		return;
	}

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= cl->lastUsercmd.serverTime ) {
			continue;
		}
		SV_ClientThink (cl, &cmds[ i ]);
	}
}


#ifdef USE_VOIP
static
qboolean SV_ShouldIgnoreVoipSender(const client_t *cl)
{
	if (!sv_voip->integer)
		return qtrue;  // VoIP disabled on this server.
	else if (!cl->hasVoip)  // client doesn't have VoIP support?!
		return qtrue;
    
	// !!! FIXME: implement player blacklist.

	return qfalse;  // don't ignore.
}

static
void SV_UserVoip( client_t *cl, msg_t *msg ) {
	const int sender = (int) (cl - svs.clients);
	const int generation = MSG_ReadByte(msg);
	const int sequence = MSG_ReadLong(msg);
	const int frames = MSG_ReadByte(msg);
	const int recip1 = MSG_ReadLong(msg);
	const int recip2 = MSG_ReadLong(msg);
	const int recip3 = MSG_ReadLong(msg);
	const int packetsize = MSG_ReadShort(msg);
	byte encoded[sizeof (cl->voipPacket[0].data)];
	client_t *client = NULL;
	voipServerPacket_t *packet = NULL;
	int i;

	if (generation < 0)
		return;   // short/invalid packet, bail.
	else if (sequence < 0)
		return;   // short/invalid packet, bail.
	else if (frames < 0)
		return;   // short/invalid packet, bail.
	else if (recip1 < 0)
		return;   // short/invalid packet, bail.
	else if (recip2 < 0)
		return;   // short/invalid packet, bail.
	else if (recip3 < 0)
		return;   // short/invalid packet, bail.
	else if (packetsize < 0)
		return;   // short/invalid packet, bail.

	if (packetsize > sizeof (encoded)) {  // overlarge packet?
		int bytesleft = packetsize;
		while (bytesleft) {
			int br = bytesleft;
			if (br > sizeof (encoded))
				br = sizeof (encoded);
			MSG_ReadData(msg, encoded, br);
			bytesleft -= br;
		}
		return;   // overlarge packet, bail.
	}

	MSG_ReadData(msg, encoded, packetsize);

	if (SV_ShouldIgnoreVoipSender(cl))
		return;   // Blacklisted, disabled, etc.

	// !!! FIXME: see if we read past end of msg...

	// !!! FIXME: reject if not speex narrowband codec.
	// !!! FIXME: decide if this is bogus data?

	// (the three recip* values are 31 bits each (ignores sign bit so we can
	//  get a -1 error from MSG_ReadLong() ... ), allowing for 93 clients.)
	assert( sv_maxclients->integer < 93 );

	// decide who needs this VoIP packet sent to them...
	for (i = 0, client = svs.clients; i < sv_maxclients->integer ; i++, client++) {
		if (client->state != CS_ACTIVE)
			continue;  // not in the game yet, don't send to this guy.
		else if (i == sender)
			continue;  // don't send voice packet back to original author.
		else if (!client->hasVoip)
			continue;  // no VoIP support, or support disabled.
		else if (client->muteAllVoip)
			continue;  // client is ignoring everyone.
		else if (client->ignoreVoipFromClient[sender])
			continue;  // client is ignoring this talker.
		else if (*cl->downloadName)   // !!! FIXME: possible to DoS?
			continue;  // no VoIP allowed if downloading, to save bandwidth.
		else if ( ((i >= 0) && (i < 31)) && ((recip1 & (1 << (i-0))) == 0) )
			continue;  // not addressed to this player.
		else if ( ((i >= 31) && (i < 62)) && ((recip2 & (1 << (i-31))) == 0) )
			continue;  // not addressed to this player.
		else if ( ((i >= 62) && (i < 93)) && ((recip3 & (1 << (i-62))) == 0) )
			continue;  // not addressed to this player.

		// Transmit this packet to the client.
		// !!! FIXME: I don't like this queueing system.
		if (client->queuedVoipPackets >= (sizeof (client->voipPacket) / sizeof (client->voipPacket[0]))) {
			Com_Printf("Too many VoIP packets queued for client #%d\n", i);
			continue;  // no room for another packet right now.
		}

		packet = &client->voipPacket[client->queuedVoipPackets];
		packet->sender = sender;
		packet->frames = frames;
		packet->len = packetsize;
		packet->generation = generation;
		packet->sequence = sequence;
		memcpy(packet->data, encoded, packetsize);
		client->queuedVoipPackets++;
	}
}
#endif



/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		return;
	}

	cl->reliableAcknowledge = MSG_ReadLong( msg );

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	if (cl->reliableAcknowledge < cl->reliableSequence - MAX_RELIABLE_COMMANDS) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	// 
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
	//
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=536
	// don't drop as long as previous command was a nextdl, after a dl is done, downloadName is set back to ""
	// but we still need to read the next message to move to next download or send gamestate
	// I don't like this hack though, it must have been working fine at some point, suspecting the fix is somewhere else
	if ( serverId != sv.serverId && !*cl->downloadName && !strstr(cl->lastClientCommandString, "nextdl") ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf("%s : ignoring pre map_restart / outdated client message\n", cl->name);
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		if ( cl->messageAcknowledge > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if( cl->oldServerTime && serverId == sv.serverId ){
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );

		// See if this is an extension command after the EOF, which means we
		//  got data that a legacy server should ignore.
		if ((c == clc_EOF) && (MSG_LookaheadByte( msg ) == clc_extension)) {
			MSG_ReadByte( msg );  // throw the clc_extension byte away.
			c = MSG_ReadByte( msg );  // something legacy servers can't do!
			// sometimes you get a clc_extension at end of stream...dangling
			//  bits in the huffman decoder giving a bogus value?
			if (c == -1) {
				c = clc_EOF;
			}
		}

		if ( c == clc_EOF ) {
			break;
		}

		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );

	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c == clc_voip ) {
#ifdef USE_VOIP
		SV_UserVoip( cl, msg );
#endif
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", (int) (cl - svs.clients) );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}
