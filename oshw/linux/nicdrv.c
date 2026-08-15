/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief
 * EtherCAT RAW socket driver.
 *
 * Low level interface functions to send and receive EtherCAT packets.
 * EtherCAT has the property that packets are only send by the master,
 * and the send packets always return in the receive buffer.
 * There can be multiple packets "on the wire" before they return.
 * To combine the received packets with the original send packets a buffer
 * system is installed. The identifier is put in the index item of the
 * EtherCAT header. The index is stored and compared when a frame is received.
 * If there is a match the packet can be combined with the transmit packet
 * and returned to the higher level function.
 *
 * The socket layer can exhibit a reversal in the packet order (rare).
 * If the Tx order is A-B-C the return order could be A-C-B. The indexed buffer
 * will reorder the packets automatically.
 *
 * The "redundant" option will configure two sockets and two NIC interfaces.
 * Slaves are connected to both interfaces, one on the IN port and one on the
 * OUT port. Packets are send via both interfaces. Any one of the connections
 * (also an interconnect) can be removed and the slaves are still serviced with
 * packets. The software layer will detect the possible failure modes and
 * compensate. If needed the packets from interface A are resent through interface B.
 * This layer if fully transparent for the higher layers.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <netpacket/packet.h>
#include <pthread.h>

#ifdef USE_XENOMAI_EVL
#include <evl/net/net.h>
#else
#include <poll.h>
#endif

#include "oshw.h"
#include "osal.h"

/** Redundancy modes */
enum
{
   /** No redundancy, single NIC mode */
   ECT_RED_NONE,
   /** Double redundant NIC connection */
   ECT_RED_DOUBLE
};

/** Primary source MAC address used for EtherCAT.
 * This address is not the MAC address used from the NIC.
 * EtherCAT does not care about MAC addressing, but it is used here to
 * differentiate the route the packet traverses through the EtherCAT
 * segment. This is needed to find out the packet flow in redundant
 * configurations. */
const uint16 priMAC[3] = EC_PRIMARY_MAC_ARRAY;
/** Secondary source MAC address used for EtherCAT. */
const uint16 secMAC[3] = EC_SECONDARY_MAC_ARRAY;

/** second MAC word is used for identification */
#define RX_PRIM priMAC[1]
/** second MAC word is used for identification */
#define RX_SEC  secMAC[1]

static void ecx_clear_rxbufstat(int *rxbufstat)
{
   int i;
   for (i = 0; i < EC_MAXBUF; i++)
   {
      rxbufstat[i] = EC_BUF_EMPTY;
   }
}

/** Basic setup to connect NIC to socket.
 * @param[in] port        = port context struct
 * @param[in] ifname      = Name of NIC device, f.e. "eth0"
 * @param[in] secondary   = if >0 then use secondary stack instead of primary
 * @return >0 if succeeded
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
   int i;
   int r, rval, ifindex;
   struct ifreq ifr;
   struct sockaddr_ll sll;
   int *psock;
#ifdef USE_XENOMAI_EVL
   int *pdevfd;
   int devfd, portfd;
   struct evl_net_devstat devs;
#else
   pthread_mutexattr_t mutexattr;
#endif

   rval = 0;
   if (secondary)
   {
      /* secondary port struct available? */
      if (port->redport)
      {
         /* when using secondary socket it is automatically a redundant setup */
         psock = &(port->redport->sockhandle);
         *psock = -1;
#ifdef USE_XENOMAI_EVL
         pdevfd = &(port->redport->evl_netdevfd);
         port->redport->evl_netdevfd = -1;
#endif
         port->redstate = ECT_RED_DOUBLE;
         port->redport->stack.sock = &(port->redport->sockhandle);
         port->redport->stack.txbuf = &(port->txbuf);
         port->redport->stack.txbuflength = &(port->txbuflength);
         port->redport->stack.tempbuf = &(port->redport->tempinbuf);
         port->redport->stack.rxbuf = &(port->redport->rxbuf);
         port->redport->stack.rxbufstat = &(port->redport->rxbufstat);
         port->redport->stack.rxsa = &(port->redport->rxsa);
         ecx_clear_rxbufstat(&(port->redport->rxbufstat[0]));
      }
      else
      {
         /* fail */
         return 0;
      }
   }
   else
   {
#ifdef USE_XENOMAI_EVL
      if (evl_new_mutex(&(port->getindex_mutex), "soem-getindex-%d", (int)getpid()) < 0 ||
          evl_new_mutex(&(port->tx_mutex), "soem-tx-%d", (int)getpid()) < 0 ||
          evl_new_mutex(&(port->rx_mutex), "soem-rx-%d", (int)getpid()) < 0)
      {
         return 0;
      }
      port->evl_netdevfd = -1;
#else
      pthread_mutexattr_init(&mutexattr);
      pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
      pthread_mutex_init(&(port->getindex_mutex), &mutexattr);
      pthread_mutex_init(&(port->tx_mutex), &mutexattr);
      pthread_mutex_init(&(port->rx_mutex), &mutexattr);
#endif
      port->sockhandle = -1;
      port->lastidx = 0;
      port->redstate = ECT_RED_NONE;
      port->stack.sock = &(port->sockhandle);
      port->stack.txbuf = &(port->txbuf);
      port->stack.txbuflength = &(port->txbuflength);
      port->stack.tempbuf = &(port->tempinbuf);
      port->stack.rxbuf = &(port->rxbuf);
      port->stack.rxbufstat = &(port->rxbufstat);
      port->stack.rxsa = &(port->rxsa);
      ecx_clear_rxbufstat(&(port->rxbufstat[0]));
      psock = &(port->sockhandle);
#ifdef USE_XENOMAI_EVL
      pdevfd = &(port->evl_netdevfd);
#endif
   }

#ifdef USE_XENOMAI_EVL
   devfd = evl_net_open_dev(ifname);
   if (devfd < 0)
   {
      EC_PRINT("ecx_setupnic: evl_net_open_dev(%s) failed: %d -- "
               "is CONFIG_EVL_NET enabled in the running kernel?\n",
               ifname, devfd);
      return 0;
   }
   *pdevfd = devfd;

   portfd = evl_net_enable_port(devfd, 0, 0);
   if (portfd < 0)
   {
      EC_PRINT("ecx_setupnic: evl_net_enable_port(%s) failed: %d\n", ifname, portfd);
      return 0;
   }

   if (evl_net_set_filter(devfd, EVL_ECAT_FILTER_PATH) < 0)
   {
      EC_PRINT("ecx_setupnic: evl_net_set_filter(%s, %s) failed -- "
               "EtherCAT frames will most likely NOT be diverted to the "
               "out-of-band stack.\n",
               ifname, EVL_ECAT_FILTER_PATH);
   }

   if (evl_net_query_dev(devfd, &devs) == 0 && !devs.oob_capable)
   {
      EC_PRINT("ecx_setupnic: WARNING - NIC driver for %s is not out-of-band capable.\n", ifname);
   }

   /* we use RAW packet socket, with packet type ETH_P_ECAT, extended with SOCK_OOB */
   *psock = socket(PF_PACKET, SOCK_RAW | SOCK_OOB, htons(ETH_P_ECAT));
#else
   /* we use RAW packet socket, with packet type ETH_P_ECAT */
   *psock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));
#endif

   if (*psock < 0)
      return 0;

   r = 0;
   i = 1;
   r |= setsockopt(*psock, SOL_SOCKET, SO_DONTROUTE, &i, sizeof(i));

   /* connect socket to NIC by name */
   strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
   ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
   r |= ioctl(*psock, SIOCGIFINDEX, &ifr);
   ifindex = ifr.ifr_ifindex;

   /* reset flags of NIC interface */
   ifr.ifr_flags = 0;
   r |= ioctl(*psock, SIOCGIFFLAGS, &ifr);

   /* set flags of NIC interface, here promiscuous and broadcast */
   ifr.ifr_flags = ifr.ifr_flags | IFF_PROMISC | IFF_BROADCAST;
   r |= ioctl(*psock, SIOCSIFFLAGS, &ifr);

   /* bind socket to protocol, in this case RAW EtherCAT */
   memset((void *)&sll, 0, sizeof(sll));
   sll.sll_family = AF_PACKET;
   sll.sll_ifindex = ifindex;
   sll.sll_protocol = htons(ETH_P_ECAT);
   r |= bind(*psock, (struct sockaddr *)&sll, sizeof(sll));
   /* setup ethernet headers in tx buffers so we don't have to repeat it */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));
   if (r == 0) rval = 1;

   return rval;
}

/** Close sockets used
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port)
{
   if (port->sockhandle >= 0)
      close(port->sockhandle);
#ifdef USE_XENOMAI_EVL
   if (port->evl_netdevfd >= 0)
   {
      evl_net_disable_port(port->evl_netdevfd);
      close(port->evl_netdevfd);
   }
#endif
   if (port->redport)
   {
      if (port->redport->sockhandle >= 0)
         close(port->redport->sockhandle);
#ifdef USE_XENOMAI_EVL
      if (port->redport->evl_netdevfd >= 0)
      {
         evl_net_disable_port(port->redport->evl_netdevfd);
         close(port->redport->evl_netdevfd);
      }
#endif
   }

   return 0;
}

/** Fill buffer with ethernet header structure.
 * Destination MAC is always broadcast.
 * Ethertype is always ETH_P_ECAT.
 * @param[out] p = buffer
 */
void ec_setupheader(void *p)
{
   ec_etherheadert *bp;
   bp = p;
   bp->da0 = htons(0xffff);
   bp->da1 = htons(0xffff);
   bp->da2 = htons(0xffff);
   bp->sa0 = htons(priMAC[0]);
   bp->sa1 = htons(priMAC[1]);
   bp->sa2 = htons(priMAC[2]);
   bp->etype = htons(ETH_P_ECAT);
}

/** Get new frame identifier index and allocate corresponding rx buffer.
 * @param[in] port        = port context struct
 * @return new index.
 */
uint8 ecx_getindex(ecx_portt *port)
{
   uint8 idx;
   uint8 cnt;

#ifdef USE_XENOMAI_EVL
   evl_lock_mutex(&(port->getindex_mutex));
#else
   pthread_mutex_lock(&(port->getindex_mutex));
#endif

   idx = port->lastidx + 1;
   /* index can't be larger than buffer array */
   if (idx >= EC_MAXBUF)
   {
      idx = 0;
   }
   cnt = 0;
   /* try to find unused index */
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF)
      {
         idx = 0;
      }
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

#ifdef USE_XENOMAI_EVL
   evl_unlock_mutex(&(port->getindex_mutex));
#else
   pthread_mutex_unlock(&(port->getindex_mutex));
#endif

   return idx;
}

/** Set rx buffer status.
 * @param[in] port        = port context struct
 * @param[in] idx      = index in buffer array
 * @param[in] bufstat  = status to set
 */
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = bufstat;
}

/** Transmit buffer over socket (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx         = index in tx buffer array
 * @param[in] stacknumber  = 0=Primary 1=Secondary stack
 * @return socket send result
 */
int ecx_outframe(ecx_portt *port, uint8 idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;
#ifdef USE_XENOMAI_EVL
   struct oob_msghdr msghdr;
   struct iovec iov;
#endif

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;

#ifdef USE_XENOMAI_EVL
   iov.iov_base = (*stack->txbuf)[idx];
   iov.iov_len = lp;
   memset(&msghdr, 0, sizeof(msghdr));
   msghdr.msg_iov = &iov;
   msghdr.msg_iovlen = 1;
   rval = oob_sendmsg(*stack->sock, &msghdr, NULL, MSG_DONTWAIT);
#else
   rval = send(*stack->sock, (*stack->txbuf)[idx], lp, 0);
#endif
   if (rval == -1)
   {
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
   }

   return rval;
}

/** Transmit buffer over socket (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx = index in tx buffer array
 * @return socket send result
 */
int ecx_outframe_red(ecx_portt *port, uint8 idx)
{
   ec_comt *datagramP;
   ec_etherheadert *ehp;
   int rval;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   /* rewrite MAC source address 1 to primary */
   ehp->sa1 = htons(priMAC[1]);
   /* transmit over primary socket*/
   rval = ecx_outframe(port, idx, 0);
   if (port->redstate != ECT_RED_NONE)
   {
#ifdef USE_XENOMAI_EVL
      struct oob_msghdr msghdr;
      struct iovec iov;
      evl_lock_mutex(&(port->tx_mutex));
#else
      pthread_mutex_lock(&(port->tx_mutex));
#endif
      ehp = (ec_etherheadert *)&(port->txbuf2);
      /* use dummy frame for secondary socket transmit (BRD) */
      datagramP = (ec_comt *)&(port->txbuf2[ETH_HEADERSIZE]);
      /* write index to frame */
      datagramP->index = idx;
      /* rewrite MAC source address 1 to secondary */
      ehp->sa1 = htons(secMAC[1]);
      /* transmit over secondary socket */
      port->redport->rxbufstat[idx] = EC_BUF_TX;

#ifdef USE_XENOMAI_EVL
      iov.iov_base = &(port->txbuf2);
      iov.iov_len = port->txbuflength2;
      memset(&msghdr, 0, sizeof(msghdr));
      msghdr.msg_iov = &iov;
      msghdr.msg_iovlen = 1;
      if (oob_sendmsg(port->redport->sockhandle, &msghdr, NULL, MSG_DONTWAIT) == -1)
#else
      if (send(port->redport->sockhandle, &(port->txbuf2), port->txbuflength2, 0) == -1)
#endif
      {
         port->redport->rxbufstat[idx] = EC_BUF_EMPTY;
      }
#ifdef USE_XENOMAI_EVL
      evl_unlock_mutex(&(port->tx_mutex));
#else
      pthread_mutex_unlock(&(port->tx_mutex));
#endif
   }

   return rval;
}

/** Non blocking read of socket. Put frame in temporary buffer.
 * @param[in] port        = port context struct
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return >0 if frame is available and read
 */
static int ecx_recvpkt(ecx_portt *port, int stacknumber)
{
   int lp, bytesrx;
   ec_stackT *stack;
#ifdef USE_XENOMAI_EVL
   struct oob_msghdr msghdr;
   struct iovec iov;
#endif

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   lp = sizeof(port->tempinbuf);

#ifdef USE_XENOMAI_EVL
   iov.iov_base = (*stack->tempbuf);
   iov.iov_len = lp;
   memset(&msghdr, 0, sizeof(msghdr));
   msghdr.msg_iov = &iov;
   msghdr.msg_iovlen = 1;
   bytesrx = oob_recvmsg(*stack->sock, &msghdr, NULL, MSG_DONTWAIT);
#else
   bytesrx = recv(*stack->sock, (*stack->tempbuf), lp, MSG_DONTWAIT);
#endif
   port->tempinbufs = bytesrx;

   return (bytesrx > 0);
}

/** Non blocking receive frame function.
 * @param[in] port        = port context struct
 * @param[in] idx         = requested index of frame
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME or EC_OTHERFRAME.
 */
int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber)
{
   uint16 l;
   int rval;
   uint8 idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];
   /* check if requested index is already in buffer ? */
   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      /* return WKC */
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      /* mark as completed */
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else
   {
#ifdef USE_XENOMAI_EVL
      evl_lock_mutex(&(port->rx_mutex));
#else
      pthread_mutex_lock(&(port->rx_mutex));
#endif
      /* check again if requested index is already in buffer ?
       * other task might have reveived it befor we grabbed mutex */
      if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
      {
         l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
         /* return WKC */
         rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
         /* mark as completed */
         (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
      }
      /* non blocking call to retrieve frame from socket */
      else if (ecx_recvpkt(port, stacknumber))
      {
         rval = EC_OTHERFRAME;
         ehp = (ec_etherheadert *)(stack->tempbuf);
         /* check if it is an EtherCAT frame */
         if (ehp->etype == htons(ETH_P_ECAT))
         {
            stack->rxcnt++;
            ecp = (ec_comt *)(&(*stack->tempbuf)[ETH_HEADERSIZE]);
            l = etohs(ecp->elength) & 0x0fff;
            idxf = ecp->index;
            /* found index equals requested index ? */
            if (idxf == idx)
            {
               /* yes, put it in the buffer array (strip ethernet header) */
               memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
               /* return WKC */
               rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
               /* mark as completed */
               (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
               /* store MAC source word 1 for redundant routing info */
               (*stack->rxsa)[idx] = ntohs(ehp->sa1);
            }
            else
            {
               /* check if index exist and someone is waiting for it */
               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  /* put it in the buffer array (strip ethernet header) */
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  /* mark as received */
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = ntohs(ehp->sa1);
               }
               else
               {
                  /* strange things happened */
               }
            }
         }
      }
#ifdef USE_XENOMAI_EVL
      evl_unlock_mutex(&(port->rx_mutex));
#else
      pthread_mutex_unlock(&(port->rx_mutex));
#endif
   }

   /* WKC if matching frame found */
   return rval;
}

/** Blocking redundant receive frame function.
 * @param[in] port        = port context struct
 * @param[in] idx = requested index of frame
 * @param[in] timer = absolute timeout time
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
static int ecx_waitinframe_red(ecx_portt *port, uint8 idx, osal_timert *timer)
{
   osal_timert timer2;
   int wkc = EC_NOFRAME;
   int wkc2 = EC_NOFRAME;
   int primrx, secrx;

   /* if not in redundant mode then always assume secondary is OK */
   if (port->redstate == ECT_RED_NONE)
      wkc2 = 0;

#ifdef USE_XENOMAI_EVL
   do
   {
      evl_usleep(50);
      /* only read frame if not already in */
      if (wkc <= EC_NOFRAME)
         wkc = ecx_inframe(port, idx, 0);
      /* only try secondary if in redundant mode */
      if (port->redstate != ECT_RED_NONE)
      {
         /* only read frame if not already in */
         if (wkc2 <= EC_NOFRAME)
            wkc2 = ecx_inframe(port, idx, 1);
      }
      /* wait for both frames to arrive or timeout */
   } while (((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME)) && !osal_timer_is_expired(timer));
#else
   /* use ppoll to reduce busy_polling */
   struct pollfd fds[2];
   struct pollfd *fdsp;
   int poll_err = 0;
   struct timespec timeout_spec = {0, 0};
   timeout_spec.tv_nsec = 50 * 1000;
   ec_stackT *stack;
   stack = &(port->stack);
   fds[0].fd = *stack->sock;
   fds[0].events = POLLIN;
   int pollcnt = 1;
   if (port->redstate != ECT_RED_NONE)
   {
      pollcnt = 2;
      stack = &(port->redport->stack);
      fds[1].fd = *stack->sock;
      fds[1].events = POLLIN;
   }
   fdsp = &fds[0];
   do
   {
      poll_err = ppoll(fdsp, pollcnt, &timeout_spec, NULL);
      if (poll_err >= 0)
      {
         /* only read frame if not already in */
         if (wkc <= EC_NOFRAME)
            wkc = ecx_inframe(port, idx, 0);
         /* only try secondary if in redundant mode */
         if (port->redstate != ECT_RED_NONE)
         {
            /* only read frame if not already in */
            if (wkc2 <= EC_NOFRAME)
               wkc2 = ecx_inframe(port, idx, 1);
         }
      }
      /* wait for both frames to arrive or timeout */
   } while (((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME)) && !osal_timer_is_expired(timer));
#endif

   /* only do redundant functions when in redundant mode */
   if (port->redstate != ECT_RED_NONE)
   {
      /* primrx if the received MAC source on primary socket */
      primrx = 0;
      if (wkc > EC_NOFRAME) primrx = port->rxsa[idx];
      /* secrx if the received MAC source on psecondary socket */
      secrx = 0;
      if (wkc2 > EC_NOFRAME) secrx = port->redport->rxsa[idx];

      /* primary socket got secondary frame and secondary socket got primary frame */
      /* normal situation in redundant mode */
      if (((primrx == RX_SEC) && (secrx == RX_PRIM)))
      {
         /* copy secondary buffer to primary */
         memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         wkc = wkc2;
      }
      /* primary socket got nothing or primary frame, and secondary socket got secondary frame */
      /* we need to resend TX packet */
      if (((primrx == 0) && (secrx == RX_SEC)) ||
          ((primrx == RX_PRIM) && (secrx == RX_SEC)))
      {
         /* If both primary and secondary have partial connection retransmit the primary received
          * frame over the secondary socket. The result from the secondary received frame is a combined
          * frame that traversed all slaves in standard order. */
         if ((primrx == RX_PRIM) && (secrx == RX_SEC))
         {
            /* copy primary rx to tx buffer */
            memcpy(&(port->txbuf[idx][ETH_HEADERSIZE]), &(port->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         }
         osal_timer_start(&timer2, EC_TIMEOUTRET);
         /* resend secondary tx */
         ecx_outframe(port, idx, 1);
         do
         {
            /* retrieve frame */
            wkc2 = ecx_inframe(port, idx, 1);
         } while ((wkc2 <= EC_NOFRAME) && !osal_timer_is_expired(&timer2));
         if (wkc2 > EC_NOFRAME)
         {
            /* copy secondary result to primary rx buffer */
            memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
            wkc = wkc2;
         }
      }
   }

   /* return WKC or EC_NOFRAME */
   return wkc;
}

/** Blocking receive frame function. Calls ec_waitinframe_red().
 * @param[in] port        = port context struct
 * @param[in] idx       = requested index of frame
 * @param[in] timeout   = timeout in us
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout)
{
   int wkc;
   osal_timert timer;

   osal_timer_start(&timer, timeout);
   wkc = ecx_waitinframe_red(port, idx, &timer);

   return wkc;
}

/** Blocking send and receive frame function. Used for non processdata frames.
 * A datagram is build into a frame and transmitted via this function. It waits
 * for an answer and returns the workcounter. The function retries if time is
 * left and the result is WKC=0 or no frame received.
 *
 * The function calls ec_outframe_red() and ec_waitinframe_red().
 *
 * @param[in] port        = port context struct
 * @param[in] idx      = index of frame
 * @param[in] timeout  = timeout in us
 * @return Workcounter or EC_NOFRAME
 */
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer1, timer2;

   osal_timer_start(&timer1, timeout);
   do
   {
      /* tx frame on primary and if in redundant mode a dummy on secondary */
      ecx_outframe_red(port, idx);
      if (timeout < EC_TIMEOUTRET)
      {
         osal_timer_start(&timer2, timeout);
      }
      else
      {
         /* normally use partial timeout for rx */
         osal_timer_start(&timer2, EC_TIMEOUTRET);
      }
      /* get frame from primary or if in redundant mode possibly from secondary */
      wkc = ecx_waitinframe_red(port, idx, &timer2);
      /* wait for answer with WKC>=0 or otherwise retry until timeout */
   } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(&timer1));

   return wkc;
}