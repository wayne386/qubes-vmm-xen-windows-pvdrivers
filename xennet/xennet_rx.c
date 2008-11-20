/*
PV Net Driver for Windows Xen HVM Domains
Copyright (C) 2007 James Harper
Copyright (C) 2007 Andrew Grover <andy.grover@oracle.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "xennet.h"

// Called at DISPATCH_LEVEL with rx lock held
static NDIS_STATUS
XenNet_RxBufferAlloc(struct xennet_info *xi)
{
  unsigned short id;
  PMDL mdl;
  ULONG i, notify;
  ULONG batch_target;
  RING_IDX req_prod = xi->rx.req_prod_pvt;
  netif_rx_request_t *req;

//KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  batch_target = xi->rx_target - (req_prod - xi->rx.rsp_cons);

  //if (batch_target < (xi->rx_target >> 2))
  //  return NDIS_STATUS_SUCCESS; /* only refill if we are less than 3/4 full already */

  for (i = 0; i < batch_target; i++)
  {
    if (xi->rx_id_free == 0)
    {
      KdPrint((__DRIVER_NAME "     Added %d out of %d buffers to rx ring (ran out of id's)\n", i, batch_target));
      break;
    }
    mdl = XenFreelist_GetPage(&xi->rx_freelist);
    if (!mdl)
    {
      KdPrint((__DRIVER_NAME "     Added %d out of %d buffers to rx ring (no free pages)\n", i, batch_target));
      break;
    }
    xi->rx_id_free--;

    /* Give to netback */
    id = (USHORT)((req_prod + i) & (NET_RX_RING_SIZE - 1));
    ASSERT(xi->rx_mdls[id] == NULL);
    xi->rx_mdls[id] = mdl;
    req = RING_GET_REQUEST(&xi->rx, req_prod + i);
    req->gref = get_grant_ref(mdl);
    ASSERT(req->gref != INVALID_GRANT_REF);
    req->id = id;
  }
  KeMemoryBarrier();
  xi->rx.req_prod_pvt = req_prod + i;
  RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&xi->rx, notify);
  if (notify)
  {
    xi->vectors.EvtChn_Notify(xi->vectors.context, xi->event_channel);
  }

//  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));

  return NDIS_STATUS_SUCCESS;
}

static PNDIS_PACKET
get_packet_from_freelist(struct xennet_info *xi)
{
  NDIS_STATUS status;
  PNDIS_PACKET packet;

  //ASSERT(!KeTestSpinLock(&xi->rx_lock));

  if (!xi->rx_packet_free)
  {
    NdisAllocatePacket(&status, &packet, xi->packet_pool);
    if (status != NDIS_STATUS_SUCCESS)
    {
      KdPrint((__DRIVER_NAME "     cannot allocate packet\n"));
      return NULL;
    }
    NDIS_SET_PACKET_HEADER_SIZE(packet, XN_HDR_SIZE);
  }
  else
  {
    xi->rx_packet_free--;
    packet = xi->rx_packet_list[xi->rx_packet_free];
  }
  return packet;
}

static VOID
put_packet_on_freelist(struct xennet_info *xi, PNDIS_PACKET packet)
{
  PNDIS_TCP_IP_CHECKSUM_PACKET_INFO csum_info;
  //ASSERT(!KeTestSpinLock(&xi->rx_lock));

  if (xi->rx_packet_free == NET_RX_RING_SIZE * 2)
  {
    KdPrint((__DRIVER_NAME "     packet free list full - releasing packet\n"));
    NdisFreePacket(packet);
    return;
  }
  csum_info = (PNDIS_TCP_IP_CHECKSUM_PACKET_INFO)&NDIS_PER_PACKET_INFO_FROM_PACKET(
    packet, TcpIpChecksumPacketInfo);
  csum_info->Value = 0;
  xi->rx_packet_list[xi->rx_packet_free] = packet;
  xi->rx_packet_free++;
}

static VOID
packet_freelist_dispose(struct xennet_info *xi)
{
  while(xi->rx_packet_free != 0)
  {
    xi->rx_packet_free--;
    NdisFreePacket(xi->rx_packet_list[xi->rx_packet_free]);
  }
}

static PNDIS_PACKET
XenNet_MakePacket(struct xennet_info *xi)
{
  PNDIS_PACKET packet;
  PUCHAR in_buffer;
  PNDIS_BUFFER out_mdl;
  PUCHAR out_buffer;
  USHORT out_offset;
  USHORT out_remaining;
  USHORT length;
  USHORT new_ip4_length;
  USHORT i;

//  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  if (!xi->rxpi.split_required)
  {
    packet = get_packet_from_freelist(xi);
    if (packet == NULL)
    {
      /* buffers will be freed in MakePackets */
      return NULL;
    }
    xi->rx_outstanding++;
    for (i = 0; i < xi->rxpi.mdl_count; i++)
      NdisChainBufferAtBack(packet, xi->rxpi.mdls[i]);

    NDIS_SET_PACKET_STATUS(packet, NDIS_STATUS_SUCCESS);
  }
  else
  {
    out_mdl = XenFreelist_GetPage(&xi->rx_freelist);
    if (!out_mdl)
      return NULL;
    packet = get_packet_from_freelist(xi);
    if (packet == NULL)
    {
      XenFreelist_PutPage(&xi->rx_freelist, out_mdl);
      return NULL;
    }
    xi->rx_outstanding++;
    out_buffer = MmGetMdlVirtualAddress(out_mdl);
    out_offset = XN_HDR_SIZE + xi->rxpi.ip4_header_length + xi->rxpi.tcp_header_length;
    out_remaining = min(xi->rxpi.mss, xi->rxpi.tcp_remaining);
    NdisAdjustBufferLength(out_mdl, out_offset + out_remaining);
    memcpy(out_buffer, xi->rxpi.header, out_offset);
    new_ip4_length = out_remaining + xi->rxpi.ip4_header_length + xi->rxpi.tcp_header_length;
    SET_NET_USHORT(&out_buffer[XN_HDR_SIZE + 2], new_ip4_length);
    SET_NET_ULONG(&out_buffer[XN_HDR_SIZE + xi->rxpi.ip4_header_length + 4], xi->rxpi.tcp_seq);
    xi->rxpi.tcp_seq += out_remaining;
    xi->rxpi.tcp_remaining = xi->rxpi.tcp_remaining - out_remaining;
    do 
    {
      ASSERT(xi->rxpi.curr_mdl < xi->rxpi.mdl_count);
      in_buffer = XenNet_GetData(&xi->rxpi, out_remaining, &length);
      memcpy(&out_buffer[out_offset], in_buffer, length);
      out_remaining = out_remaining - length;
      out_offset = out_offset + length;
    } while (out_remaining != 0);
    NdisChainBufferAtBack(packet, out_mdl);
    XenNet_SumIpHeader(out_buffer, xi->rxpi.ip4_header_length);
    NDIS_SET_PACKET_STATUS(packet, NDIS_STATUS_SUCCESS);
  }
//  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ " (%p)\n", packet));
  
  return packet;
}

/*
 Windows appears to insist that the checksum on received packets is correct, and won't
 believe us when we lie about it, which happens when the packet is generated on the
 same bridge in Dom0. Doh!
 This is only for TCP and UDP packets. IP checksums appear to be correct anyways.
*/

static BOOLEAN
XenNet_SumPacketData(
  packet_info_t *pi,
  PNDIS_PACKET packet,
  BOOLEAN set_csum
)
{
  USHORT i;
  PUCHAR buffer;
  PMDL mdl;
  UINT total_length;
  UINT buffer_length;
  USHORT buffer_offset;
  ULONG csum;
  PUSHORT csum_ptr;
  USHORT remaining;
  USHORT ip4_length;
  
//  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  NdisGetFirstBufferFromPacketSafe(packet, &mdl, &buffer, &buffer_length, &total_length, NormalPagePriority);
  ASSERT(mdl);

  ip4_length = GET_NET_PUSHORT(&buffer[XN_HDR_SIZE + 2]);

  if ((USHORT)(ip4_length + XN_HDR_SIZE) != total_length)
  {
    KdPrint((__DRIVER_NAME "     Size Mismatch %d (ip4_length + XN_HDR_SIZE) != %d (total_length)\n", ip4_length + XN_HDR_SIZE, total_length));
  }

  switch (pi->ip_proto)
  {
  case 6:
    csum_ptr = (USHORT *)&buffer[XN_HDR_SIZE + pi->ip4_header_length + 16];
    break;
  case 17:
    csum_ptr = (USHORT *)&buffer[XN_HDR_SIZE + pi->ip4_header_length + 6];
    break;
  default:
    KdPrint((__DRIVER_NAME "     Don't know how to calc sum for IP Proto %d\n", pi->ip_proto));
    return FALSE; // should never happen
  }

  if (set_csum)  
    *csum_ptr = 0;

  csum = 0;
  csum += GET_NET_PUSHORT(&buffer[XN_HDR_SIZE + 12]) + GET_NET_PUSHORT(&buffer[XN_HDR_SIZE + 14]); // src
  csum += GET_NET_PUSHORT(&buffer[XN_HDR_SIZE + 16]) + GET_NET_PUSHORT(&buffer[XN_HDR_SIZE + 18]); // dst
  csum += ((USHORT)buffer[XN_HDR_SIZE + 9]);

  remaining = ip4_length - pi->ip4_header_length;

  csum += remaining;

  for (buffer_offset = i = XN_HDR_SIZE + pi->ip4_header_length; i < total_length - 1; i += 2, buffer_offset += 2)
  {
    /* don't include the checksum field itself in the calculation */
    if ((pi->ip_proto == 6 && i == XN_HDR_SIZE + pi->ip4_header_length + 16) || (pi->ip_proto == 17 && i == XN_HDR_SIZE + pi->ip4_header_length + 6))
      continue;
    if (buffer_offset == buffer_length - 1) // deal with a buffer ending on an odd byte boundary
    {
      csum += (USHORT)buffer[buffer_offset] << 8;
      NdisGetNextBuffer(mdl, &mdl);
      if (mdl == NULL)
      {
        KdPrint((__DRIVER_NAME "     Ran out of buffers\n"));
        return FALSE; // should never happen
      }
      NdisQueryBufferSafe(mdl, &buffer, &buffer_length, NormalPagePriority);
      csum += ((USHORT)buffer[0]);
      buffer_offset = (USHORT)-1;
    }
    else
    {
      if (buffer_offset == buffer_length)
      {
//        KdPrint((__DRIVER_NAME "     New buffer - aligned...\n"));
        NdisGetNextBuffer(mdl, &mdl);
        if (mdl == NULL)
        {
          KdPrint((__DRIVER_NAME "     Ran out of buffers\n"));
          return FALSE; // should never happen
        }
        NdisQueryBufferSafe(mdl, (PVOID) &buffer, &buffer_length, NormalPagePriority);
        buffer_offset = 0;
      }
      csum += GET_NET_PUSHORT(&buffer[buffer_offset]);
    }
  }
  if (i != total_length) // last odd byte
  {
    csum += ((USHORT)buffer[buffer_offset] << 8);
  }
  while (csum & 0xFFFF0000)
    csum = (csum & 0xFFFF) + (csum >> 16);
  
  if (set_csum)
    *csum_ptr = (USHORT)~GET_NET_USHORT((USHORT)csum);
  else
    return (BOOLEAN)(*csum_ptr == (USHORT)~GET_NET_USHORT((USHORT)csum));

  return TRUE;
//  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
}

static ULONG
XenNet_MakePackets(
  struct xennet_info *xi,
  PLIST_ENTRY rx_packet_list
)
{
  USHORT i;
  ULONG packet_count = 0;
  PNDIS_PACKET packet;
  PLIST_ENTRY entry;
  UCHAR psh;
  PNDIS_TCP_IP_CHECKSUM_PACKET_INFO csum_info;
  ULONG parse_result;  

//  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "(packets = %p, packet_count = %d)\n", packets, *packet_count_p));

  parse_result = XenNet_ParsePacketHeader(&xi->rxpi);
  
  if ((xi->packet_filter & NDIS_PACKET_TYPE_MULTICAST)
    && !(xi->packet_filter & NDIS_PACKET_TYPE_ALL_MULTICAST)
    && (xi->rxpi.header[0] & 0x01)
    && !(xi->rxpi.header[0] == 0xFF && xi->rxpi.header[1] == 0xFF && xi->rxpi.header[2] == 0xFF
        && xi->rxpi.header[3] == 0xFF && xi->rxpi.header[4] == 0xFF && xi->rxpi.header[5] == 0xFF))
  {
    for (i = 0; i < xi->multicast_list_size; i++)
    {
      if (memcmp(xi->multicast_list[i], xi->rxpi.header, 6) == 0)
        break;
    }
    if (i == xi->multicast_list_size)
      goto done;
  }
  switch (xi->rxpi.ip_proto)
  {
  case 6:  // TCP
    if (xi->rxpi.split_required)
      break;
    // fallthrough
  case 17:  // UDP
    packet = XenNet_MakePacket(xi);
    if (packet == NULL)
    {
      KdPrint((__DRIVER_NAME "     Ran out of packets\n"));
      xi->stat_rx_no_buffer++;
      packet_count = 0;
      goto done;
    }
    if (parse_result == PARSE_OK)
    {
      if (xi->rxpi.csum_blank)
        XenNet_SumPacketData(&xi->rxpi, packet, TRUE);
      csum_info = (PNDIS_TCP_IP_CHECKSUM_PACKET_INFO)&NDIS_PER_PACKET_INFO_FROM_PACKET(
        packet, TcpIpChecksumPacketInfo);
      if (xi->rxpi.csum_blank || xi->rxpi.data_validated)
      {
        if (xi->setting_csum.V4Receive.TcpChecksum && xi->rxpi.ip_proto == 6)
          csum_info->Receive.NdisPacketTcpChecksumSucceeded = TRUE;
        if (xi->setting_csum.V4Receive.UdpChecksum && xi->rxpi.ip_proto == 17)
          csum_info->Receive.NdisPacketUdpChecksumSucceeded = TRUE;
      }
      else if (!xi->config_csum_rx_check)
      {
        if (xi->setting_csum.V4Receive.TcpChecksum && xi->rxpi.ip_proto == 6)
        {
          if (XenNet_SumPacketData(&xi->rxpi, packet, FALSE))
            csum_info->Receive.NdisPacketTcpChecksumSucceeded = TRUE;
          else
            csum_info->Receive.NdisPacketTcpChecksumFailed = TRUE;
        }
        if (xi->setting_csum.V4Receive.UdpChecksum && xi->rxpi.ip_proto == 17)
        {
          if (XenNet_SumPacketData(&xi->rxpi, packet, FALSE))
            csum_info->Receive.NdisPacketUdpChecksumSucceeded = TRUE;
          else
            csum_info->Receive.NdisPacketUdpChecksumFailed = TRUE;
        }
      }
    }
    entry = (PLIST_ENTRY)&packet->MiniportReservedEx[sizeof(PVOID)];
    InsertTailList(rx_packet_list, entry);
    XenNet_ClearPacketInfo(&xi->rxpi);
    return 1;
  default:
    packet = XenNet_MakePacket(xi);
    if (packet == NULL)
    {
      KdPrint((__DRIVER_NAME "     Ran out of packets\n"));
      xi->stat_rx_no_buffer++;
      packet_count = 0;
      goto done;
    }
    entry = (PLIST_ENTRY)&packet->MiniportReservedEx[sizeof(PVOID)];
    InsertTailList(rx_packet_list, entry);
    XenNet_ClearPacketInfo(&xi->rxpi);
    return 1;
  }

  xi->rxpi.tcp_remaining = xi->rxpi.tcp_length;
  if (MmGetMdlByteCount(xi->rxpi.mdls[0]) > (ULONG)(XN_HDR_SIZE + xi->rxpi.ip4_header_length + xi->rxpi.tcp_header_length))
    xi->rxpi.curr_mdl_offset = XN_HDR_SIZE + xi->rxpi.ip4_header_length + xi->rxpi.tcp_header_length;
  else
    xi->rxpi.curr_mdl = 1;

  /* we can make certain assumptions here as the following code is only for tcp4 */
  psh = xi->rxpi.header[XN_HDR_SIZE + xi->rxpi.ip4_header_length + 13] & 8;
  while (xi->rxpi.tcp_remaining)
  {
    PUCHAR buffer;
    PMDL mdl;
    UINT total_length;
    UINT buffer_length;
    packet = XenNet_MakePacket(xi);
    if (!packet)
    {
      KdPrint((__DRIVER_NAME "     Ran out of packets\n"));
      xi->stat_rx_no_buffer++;
      break; /* we are out of memory - just drop the packets */
    }
    if (xi->setting_csum.V4Receive.TcpChecksum)
    {
      csum_info = (PNDIS_TCP_IP_CHECKSUM_PACKET_INFO)&NDIS_PER_PACKET_INFO_FROM_PACKET(
        packet, TcpIpChecksumPacketInfo);
      csum_info->Receive.NdisPacketTcpChecksumSucceeded = TRUE;
    }
    if (psh)
    {
      NdisGetFirstBufferFromPacketSafe(packet, &mdl, &buffer, &buffer_length, &total_length, NormalPagePriority);
      if (xi->rxpi.tcp_remaining)
      {
        buffer[XN_HDR_SIZE + xi->rxpi.ip4_header_length + 13] &= ~8;
      }
      else
      {
        buffer[XN_HDR_SIZE + xi->rxpi.ip4_header_length + 13] |= 8;
      }
    }
    XenNet_SumPacketData(&xi->rxpi, packet, TRUE);
    entry = (PLIST_ENTRY)&packet->MiniportReservedEx[sizeof(PVOID)];
    InsertTailList(rx_packet_list, entry);
    packet_count++;
  }

done:
  for (i = 0; i < xi->rxpi.mdl_count; i++)
  {
    NdisAdjustBufferLength(xi->rxpi.mdls[i], PAGE_SIZE);
    XenFreelist_PutPage(&xi->rx_freelist, xi->rxpi.mdls[i]);
  }
  XenNet_ClearPacketInfo(&xi->rxpi);
//  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ " (split)\n"));
  return packet_count;
}

typedef struct {
  struct xennet_info *xi;
  BOOLEAN is_timer;
} sync_context_t;

static BOOLEAN
XenNet_RxQueueDpcSynchronized(PVOID context)
{
  sync_context_t *sc = context;
  BOOLEAN result;
  
  if (!sc->is_timer && sc->xi->config_rx_interrupt_moderation)
  {
    /* if an is_timer dpc is queued it will muck things up for us, so make sure we requeue a !is_timer dpc */
    KeRemoveQueueDpc(&sc->xi->rx_dpc);
  }
  sc->xi->last_dpc_isr = FALSE;
  KeQuerySystemTime(&sc->xi->last_dpc_scheduled);
  result = KeInsertQueueDpc(&sc->xi->rx_dpc, UlongToPtr(sc->is_timer), NULL);
  
  return TRUE;
}

#define MAXIMUM_PACKETS_PER_INDICATE 32
#define MAX_PACKETS_PER_INTERRUPT 32

static VOID
XenNet_RxTimerDpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
  struct xennet_info *xi = context;
  sync_context_t sc;

  UNREFERENCED_PARAMETER(dpc);
  UNREFERENCED_PARAMETER(arg1);
  UNREFERENCED_PARAMETER(arg2);
  
  sc.xi = xi;
  sc.is_timer = TRUE;
#pragma warning(suppress:4054) /* no way around this... */
  NdisMSynchronizeWithInterrupt(&xi->interrupt, (PVOID)XenNet_RxQueueDpcSynchronized, &sc);
}

// Called at DISPATCH_LEVEL
//NDIS_STATUS
//XenNet_RxBufferCheck(struct xennet_info *xi, BOOLEAN is_timer)
static VOID
XenNet_RxBufferCheck(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
  struct xennet_info *xi = context;
  RING_IDX cons, prod;
  LIST_ENTRY rx_packet_list;
  PLIST_ENTRY entry;
  PNDIS_PACKET packets[MAXIMUM_PACKETS_PER_INDICATE];
  ULONG packet_count = 0;
  PMDL mdl;
  struct netif_rx_response *rxrsp = NULL;
  struct netif_extra_info *ei;
  USHORT id;
  int more_to_do = FALSE;
  int page_count = 0;
  ULONG event = 1;
  BOOLEAN is_timer = (BOOLEAN)PtrToUlong(arg1);
  BOOLEAN set_timer = FALSE;

  UNREFERENCED_PARAMETER(dpc);
  UNREFERENCED_PARAMETER(arg1);
  UNREFERENCED_PARAMETER(arg2);

  if (is_timer) 
    KdPrint((__DRIVER_NAME "     RX Timer\n"));
  //KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  ASSERT(xi->connected);

  KeAcquireSpinLockAtDpcLevel(&xi->rx_lock);

  InitializeListHead(&rx_packet_list);

  if (xi->config_rx_interrupt_moderation)
    KeCancelTimer(&xi->rx_timer);

  do {
    prod = xi->rx.sring->rsp_prod;
//KdPrint((__DRIVER_NAME "     prod - cons = %d\n", prod - xi->rx.rsp_cons));    
    KeMemoryBarrier(); /* Ensure we see responses up to 'prod'. */

    for (cons = xi->rx.rsp_cons; cons != prod && packet_count < MAX_PACKETS_PER_INTERRUPT; cons++)
    {
      id = (USHORT)(cons & (NET_RX_RING_SIZE - 1));
      ASSERT(xi->rx_mdls[id]);
      mdl = xi->rx_mdls[id];
      xi->rx_mdls[id] = NULL;
      xi->rx_id_free++;
      if (xi->rxpi.extra_info)
      {
        XenFreelist_PutPage(&xi->rx_freelist, mdl);
        ei = (struct netif_extra_info *)RING_GET_RESPONSE(&xi->rx, cons);
        xi->rxpi.extra_info = (BOOLEAN)!!(ei->flags & XEN_NETIF_EXTRA_FLAG_MORE);
        switch (ei->type)
        {
        case XEN_NETIF_EXTRA_TYPE_GSO:
          switch (ei->u.gso.type)
          {
          case XEN_NETIF_GSO_TYPE_TCPV4:
            xi->rxpi.mss = ei->u.gso.size;
            // TODO - put this assertion somewhere ASSERT(header_len + xi->rxpi.mss <= PAGE_SIZE); // this limits MTU to PAGE_SIZE - XN_HEADER_LEN
            break;
          default:
            KdPrint((__DRIVER_NAME "     Unknown GSO type (%d) detected\n", ei->u.gso.type));
            break;
          }
          break;
        default:
          KdPrint((__DRIVER_NAME "     Unknown extra info type (%d) detected\n", ei->type));
          break;
        }
      }
      else
      {
        rxrsp = RING_GET_RESPONSE(&xi->rx, cons);
        if (rxrsp->status <= 0
          || rxrsp->offset + rxrsp->status > PAGE_SIZE)
        {
          KdPrint((__DRIVER_NAME ": Error: rxrsp offset %d, size %d\n",
            rxrsp->offset, rxrsp->status));
          ASSERT(!xi->rxpi.extra_info);
          XenFreelist_PutPage(&xi->rx_freelist, mdl);
          continue;
        }
        ASSERT(rxrsp->id == id);
        if (!xi->rxpi.more_frags) // handling the packet's 1st buffer
        {
          if (rxrsp->flags & NETRXF_csum_blank)
            xi->rxpi.csum_blank = TRUE;
          if (rxrsp->flags & NETRXF_data_validated)
            xi->rxpi.data_validated = TRUE;
        }
        
        if (!xi->rxpi.mdl_count || MmGetMdlByteCount(xi->rxpi.mdls[xi->rxpi.mdl_count - 1]) == PAGE_SIZE)
        {
          /* first buffer or no room in current buffer */
          NdisAdjustBufferLength(mdl, rxrsp->status);
          xi->rxpi.mdls[xi->rxpi.mdl_count++] = mdl;
        }
        else if (MmGetMdlByteCount(xi->rxpi.mdls[xi->rxpi.mdl_count - 1]) + rxrsp->status <= PAGE_SIZE)
        {
          /* this buffer fits entirely in current buffer */
          PMDL dst_mdl = xi->rxpi.mdls[xi->rxpi.mdl_count - 1];
          PUCHAR dst_addr = MmGetMdlVirtualAddress(dst_mdl);
          PUCHAR src_addr = MmGetMdlVirtualAddress(mdl);
          dst_addr += MmGetMdlByteCount(dst_mdl);
          memcpy(dst_addr, src_addr, rxrsp->status);
          NdisAdjustBufferLength(dst_mdl, MmGetMdlByteCount(dst_mdl) + rxrsp->status);
          XenFreelist_PutPage(&xi->rx_freelist, mdl);
        }
        else
        {
          /* this buffer doesn't fit entirely in current buffer */
          PMDL dst_mdl = xi->rxpi.mdls[xi->rxpi.mdl_count - 1];
          PUCHAR dst_addr = MmGetMdlVirtualAddress(dst_mdl);
          PUCHAR src_addr = MmGetMdlVirtualAddress(mdl);
          ULONG copy_size = PAGE_SIZE - MmGetMdlByteCount(dst_mdl);
          dst_addr += MmGetMdlByteCount(dst_mdl);
          memcpy(dst_addr, src_addr, copy_size);
          NdisAdjustBufferLength(dst_mdl, PAGE_SIZE);
          dst_addr = src_addr;
          src_addr += copy_size;
          copy_size = rxrsp->status - copy_size;
          memmove(dst_addr, src_addr, copy_size); /* use memmove because the regions overlap */
          NdisAdjustBufferLength(mdl, copy_size);
          xi->rxpi.mdls[xi->rxpi.mdl_count++] = mdl;
        }
        xi->rxpi.extra_info = (BOOLEAN)!!(rxrsp->flags & NETRXF_extra_info);
        xi->rxpi.more_frags = (BOOLEAN)!!(rxrsp->flags & NETRXF_more_data);
        xi->rxpi.total_length = xi->rxpi.total_length + rxrsp->status;
      }

      /* Packet done, add it to the list */
      if (!xi->rxpi.more_frags && !xi->rxpi.extra_info)
      {
        packet_count += XenNet_MakePackets(xi, &rx_packet_list);
      }
      if (!more_to_do) {
        page_count++; /* only interested in the number of pages available after the first time around */
      }
    }
    xi->rx.rsp_cons = cons;

    if (!more_to_do)
    {
      /* if we were called on the timer then turn off moderation */
      if (is_timer)
        xi->avg_page_count = 0;
      else if (page_count != 0) /* if page_count == 0 then the interrupt wasn't really for us so it's not fair for it to affect the averages... */
        xi->avg_page_count = (xi->avg_page_count * 7 + page_count * 128) / 8;
    }
    if (packet_count >= MAX_PACKETS_PER_INTERRUPT)
      break;
    more_to_do = RING_HAS_UNCONSUMED_RESPONSES(&xi->rx);
    if (!more_to_do)
    {
      if (xi->config_rx_interrupt_moderation)
        event = min(max(1, xi->avg_page_count * 3 / 4 / 128), 128);
      else
        event = 1;
      xi->rx.sring->rsp_event = xi->rx.rsp_cons + event;
      mb();
      more_to_do = RING_HAS_UNCONSUMED_RESPONSES(&xi->rx);
    }
  } while (more_to_do);

  if (xi->rxpi.more_frags || xi->rxpi.extra_info)
    KdPrint((__DRIVER_NAME "     Partial receive (more_frags = %d, extra_info = %d, total_length = %d, mdl_count = %d)\n", xi->rxpi.more_frags, xi->rxpi.extra_info, xi->rxpi.total_length, xi->rxpi.mdl_count));

  /* Give netback more buffers */
  XenNet_RxBufferAlloc(xi);

  //KdPrint((__DRIVER_NAME "     packet_count = %d, page_count = %d, avg_page_count = %d, event = %d\n", packet_count, page_count, xi->avg_page_count / 128, event));

  if (packet_count >= MAX_PACKETS_PER_INTERRUPT)
  {
    /* fire again immediately */
    sync_context_t sc;
    sc.xi = xi;
    sc.is_timer = FALSE;
#pragma warning(suppress:4054) /* no way around this... */
    NdisMSynchronizeWithInterrupt(&xi->interrupt, (PVOID)XenNet_RxQueueDpcSynchronized, &sc);
  }
  else if (xi->config_rx_interrupt_moderation)
  {
    if (event > 1)
    {
      set_timer = TRUE;
    }
  }
  KeReleaseSpinLockFromDpcLevel(&xi->rx_lock);

  entry = RemoveHeadList(&rx_packet_list);
  packet_count = 0;
  while (entry != &rx_packet_list)
  {
    PNDIS_PACKET packet = CONTAINING_RECORD(entry, NDIS_PACKET, MiniportReservedEx[sizeof(PVOID)]);
    PVOID *addr;
    UINT buffer_length;
    UINT total_length;
    NdisGetFirstBufferFromPacketSafe(packet, &mdl, &addr, &buffer_length, &total_length, NormalPagePriority);
    ASSERT(total_length <= xi->config_mtu + XN_HDR_SIZE);
    packets[packet_count++] = packet;
    entry = RemoveHeadList(&rx_packet_list);
    if (packet_count == MAXIMUM_PACKETS_PER_INDICATE || entry == &rx_packet_list)
    {
      NdisMIndicateReceivePacket(xi->adapter_handle, packets, packet_count);
      packet_count = 0;
    }
  }
  /* set the timer after we have indicated the packets, as indicating can take a significant amount of time */
  if (set_timer)
  {
    LARGE_INTEGER due_time;
    due_time.QuadPart = -10 * 1000 * 10; /* 10ms */
    KeSetTimer(&xi->rx_timer, due_time, &xi->rx_timer_dpc);
  }
  //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
}

/* called at DISPATCH_LEVEL */
/* it's okay for return packet to be called while resume_state != RUNNING as the packet will simply be added back to the freelist, the grants will be fixed later */
VOID DDKAPI
XenNet_ReturnPacket(
  IN NDIS_HANDLE MiniportAdapterContext,
  IN PNDIS_PACKET Packet
  )
{
  struct xennet_info *xi = MiniportAdapterContext;
  PMDL mdl;

  KeAcquireSpinLockAtDpcLevel(&xi->rx_lock);

  NdisUnchainBufferAtBack(Packet, &mdl);
  while (mdl)
  {
    NdisAdjustBufferLength(mdl, PAGE_SIZE);
    XenFreelist_PutPage(&xi->rx_freelist, mdl);
    NdisUnchainBufferAtBack(Packet, &mdl);
  }

  put_packet_on_freelist(xi, Packet);
  xi->rx_outstanding--;
  
  if (!xi->rx_outstanding && xi->rx_shutting_down)
    KeSetEvent(&xi->packet_returned_event, IO_NO_INCREMENT, FALSE);

  KeReleaseSpinLockFromDpcLevel(&xi->rx_lock);

  //  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
}

/*
   Free all Rx buffers (on halt, for example) 
   The ring must be stopped at this point.
*/

static void
XenNet_RxBufferFree(struct xennet_info *xi)
{
  int i;
  PMDL mdl;

  XenFreelist_Dispose(&xi->rx_freelist);

  ASSERT(!xi->connected);

  for (i = 0; i < NET_RX_RING_SIZE; i++)
  {
    if (!xi->rx_mdls[i])
      continue;

    mdl = xi->rx_mdls[i];
    NdisAdjustBufferLength(mdl, PAGE_SIZE);
    XenFreelist_PutPage(&xi->rx_freelist, mdl);
  }
}

VOID
XenNet_RxResumeStart(xennet_info_t *xi)
{
  int i;
  KIRQL old_irql;

  KeAcquireSpinLock(&xi->rx_lock, &old_irql);
  for (i = 0; i < NET_RX_RING_SIZE; i++)
  {
    if (xi->rx_mdls[i])
    {
      XenFreelist_PutPage(&xi->rx_freelist, xi->rx_mdls[i]);
      xi->rx_mdls[i] = NULL;
    }
  }
  XenFreelist_ResumeStart(&xi->rx_freelist);
  xi->rx_id_free = NET_RX_RING_SIZE;
  xi->rx_outstanding = 0;
  KeReleaseSpinLock(&xi->rx_lock, old_irql);
}

VOID
XenNet_RxResumeEnd(xennet_info_t *xi)
{
  KIRQL old_irql;

  KeAcquireSpinLock(&xi->rx_lock, &old_irql);
  XenFreelist_ResumeEnd(&xi->rx_freelist);
  XenNet_RxBufferAlloc(xi);
  KeReleaseSpinLock(&xi->rx_lock, old_irql);
}

#if 0
/* Called at DISPATCH LEVEL */
static VOID DDKAPI
XenNet_RxTimer(
  PVOID SystemSpecific1,
  PVOID FunctionContext,
  PVOID SystemSpecific2,
  PVOID SystemSpecific3
)
{
  struct xennet_info *xi = FunctionContext;

  UNREFERENCED_PARAMETER(SystemSpecific1);
  UNREFERENCED_PARAMETER(SystemSpecific2);
  UNREFERENCED_PARAMETER(SystemSpecific3);

  if (xi->connected && !xi->inactive && xi->device_state->resume_state == RESUME_STATE_RUNNING)
  {
    KdPrint((__DRIVER_NAME "     RX Timer\n"));
    XenNet_RxBufferCheck(xi, TRUE);
  }
}
#endif

BOOLEAN
XenNet_RxInit(xennet_info_t *xi)
{
  int i;

  FUNCTION_ENTER();

  KeInitializeEvent(&xi->packet_returned_event, SynchronizationEvent, FALSE);
  KeInitializeTimer(&xi->rx_timer);
  KeInitializeDpc(&xi->rx_dpc, XenNet_RxBufferCheck, xi);
  KeSetTargetProcessorDpc(&xi->rx_dpc, 0);
  //KeSetImportanceDpc(&xi->rx_dpc, HighImportance);
  KeInitializeDpc(&xi->rx_timer_dpc, XenNet_RxTimerDpc, xi);
  //NdisMInitializeTimer(&xi->rx_timer, xi->adapter_handle, XenNet_RxTimer, xi);
  xi->avg_page_count = 0;

  xi->rx_shutting_down = FALSE;
  
  xi->rx_id_free = NET_RX_RING_SIZE;

  for (i = 0; i < NET_RX_RING_SIZE; i++)
  {
    xi->rx_mdls[i] = NULL;
  }

  xi->rx_outstanding = 0;
  XenFreelist_Init(xi, &xi->rx_freelist, &xi->rx_lock);

  XenNet_RxBufferAlloc(xi);

  FUNCTION_EXIT();

  return TRUE;
}

BOOLEAN
XenNet_RxShutdown(xennet_info_t *xi)
{
  KIRQL OldIrql;

  FUNCTION_ENTER();

  KeAcquireSpinLock(&xi->rx_lock, &OldIrql);
  xi->rx_shutting_down = TRUE;
  KeReleaseSpinLock(&xi->rx_lock, OldIrql);

  if (xi->config_rx_interrupt_moderation)
  {
    KeCancelTimer(&xi->rx_timer);
  }

  while (xi->rx_outstanding)
  {
    KdPrint((__DRIVER_NAME "     Waiting for all packets to be returned\n"));
    KeWaitForSingleObject(&xi->packet_returned_event, Executive, KernelMode, FALSE, NULL);
  }

  KeAcquireSpinLock(&xi->rx_lock, &OldIrql);

  XenNet_RxBufferFree(xi);

  XenFreelist_Dispose(&xi->rx_freelist);

  packet_freelist_dispose(xi);

  KeReleaseSpinLock(&xi->rx_lock, OldIrql);

  FUNCTION_EXIT();

  return TRUE;
}
