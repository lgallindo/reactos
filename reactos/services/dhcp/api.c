/* $Id: $
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             subsys/system/dhcp/api.c
 * PURPOSE:          DHCP client api handlers
 * PROGRAMMER:       arty
 */

#include "rosdhcp.h"
#include <winsock2.h>
#include <iphlpapi.h>

static CRITICAL_SECTION ApiCriticalSection;

VOID ApiInit() {
    InitializeCriticalSection( &ApiCriticalSection );
}

VOID ApiLock() {
    EnterCriticalSection( &ApiCriticalSection );
}

VOID ApiUnlock() {
    LeaveCriticalSection( &ApiCriticalSection );
}

/* This represents the service portion of the DHCP client API */

DWORD DSLeaseIpAddress( PipeSendFunc Send, COMM_DHCP_REQ *Req ) {
    COMM_DHCP_REPLY Reply;
    PDHCP_ADAPTER Adapter;

    ApiLock();

    Adapter = AdapterFindIndex( Req->AdapterIndex );

    Reply.Reply = Adapter ? 1 : 0;

    if( Adapter ) {
        add_protocol( Adapter->DhclientInfo.name,
                      Adapter->DhclientInfo.rfdesc, got_one,
                      &Adapter->DhclientInfo );
	Adapter->DhclientInfo.client->state = S_INIT;
	state_reboot(&Adapter->DhclientInfo);
    }

    ApiUnlock();

    return Send( &Reply );
}

DWORD DSQueryHWInfo( PipeSendFunc Send, COMM_DHCP_REQ *Req ) {
    COMM_DHCP_REPLY Reply;
    PDHCP_ADAPTER Adapter;

    ApiLock();

    Adapter = AdapterFindIndex( Req->AdapterIndex );

    Reply.QueryHWInfo.AdapterIndex = Req->AdapterIndex;
    Reply.QueryHWInfo.MediaType = Adapter->IfMib.dwType;
    Reply.QueryHWInfo.Mtu = Adapter->IfMib.dwMtu;
    Reply.QueryHWInfo.Speed = Adapter->IfMib.dwSpeed;

    ApiUnlock();

    return Send( &Reply );
}

DWORD DSReleaseIpAddressLease( PipeSendFunc Send, COMM_DHCP_REQ *Req ) {
    COMM_DHCP_REPLY Reply;
    PDHCP_ADAPTER Adapter;

    ApiLock();

    Adapter = AdapterFindIndex( Req->AdapterIndex );

    Reply.Reply = Adapter ? 1 : 0;

    if( Adapter ) {
        DeleteIPAddress( Adapter->NteContext );
        remove_protocol( find_protocol_by_adapter( &Adapter->DhclientInfo ) );
    }

    ApiUnlock();

    return Send( &Reply );
}

DWORD DSRenewIpAddressLease( PipeSendFunc Send, COMM_DHCP_REQ *Req ) {
    COMM_DHCP_REPLY Reply;
    PDHCP_ADAPTER Adapter;

    ApiLock();

    Adapter = AdapterFindIndex( Req->AdapterIndex );

    Reply.Reply = Adapter ? 1 : 0;

    if( !Adapter || Adapter->DhclientState.state != S_BOUND ) {
        Reply.Reply = 0;
        return Send( &Reply );
    }

    Adapter->DhclientState.state = S_BOUND;

    send_discover( &Adapter->DhclientInfo );
    state_bound( &Adapter->DhclientInfo );

    ApiUnlock();

    return Send( &Reply );
}

DWORD DSStaticRefreshParams( PipeSendFunc Send, COMM_DHCP_REQ *Req ) {
    NTSTATUS Status;
    COMM_DHCP_REPLY Reply;
    PDHCP_ADAPTER Adapter;

    ApiLock();

    Adapter = AdapterFindIndex( Req->AdapterIndex );

    Reply.Reply = Adapter ? 1 : 0;

    if( Adapter ) {
        DeleteIPAddress( Adapter->NteContext );
        Adapter->DhclientState.state = S_STATIC;
        remove_protocol( find_protocol_by_adapter( &Adapter->DhclientInfo ) );
        Status = AddIPAddress( Req->Body.StaticRefreshParams.IPAddress,
                               Req->Body.StaticRefreshParams.Netmask,
                               Req->AdapterIndex,
                               &Adapter->NteContext,
                               &Adapter->NteInstance );
        Reply.Reply = NT_SUCCESS(Status);
    }

    ApiUnlock();

    return Send( &Reply );
}
