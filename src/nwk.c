/**
 * @file nwk.c
 * @brief Network and Transport layers.
 * @compiler CC65
 * @author Paul Gardner-Stephen (paul@m-e-g-a.org)
 * derived from:
 * @author Bruno Basseto (bruno@wise-ware.org)
 */

#include <stdio.h>

#include "memory.h"
#include "debug.h"

// On MEGA65 we have deep enough stack we don't need to schedule sending
// ACKs, we can just send them immediately.
// #define INSTANT_ACK
// Report various things on serial monitor interface
// #define DEBUG_ACK

// Enable ICMP PING if desired.
//#define ENABLE_ICMP

/*
  Use a graduated timeout that starts out fast, and then slows down,
  so that we spread our timeouts over a longer preiod of time, but
  at the same time, we don't wait forever for initial retries on a
  local LAN
*/
#define SOCKET_TIMEOUT(S) (TIMEOUT_TCP + 32*(RETRIES_TCP - S->retry))
//#define DEBUG_TCP_RETRIES


/********************************************************************************
 ********************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 1995-2013 Bruno Basseto (bruno@wise-ware.org).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ********************************************************************************
 ********************************************************************************/

#include <string.h>
#include "weeip.h"
#include "checksum.h"
#include "eth.h"
#include "arp.h"

#include "memory.h"

/**
 * Message header buffer.
 */
HEADER _header;

IPV4 ip_broadcast;                  ///< Subnetwork broadcast address

static uint16_t data_size,data_ofs;
static _uint32_t seq;
#define TCPH(X) _header.t.tcp.X
#define ICMPH(X) _header.t.icmp.X
#define UDPH(X) _header.t.udp.X
#define IPH(X) _header.ip.X

// Smaller MTU to save memory
#define MTU 1000
extern unsigned char tx_frame_buf[MTU];
extern unsigned short eth_tx_len;

/**
 * Packet counter.
 */
uint16_t id;

/**
 * Local IP address.
 */
IPV4 ip_local;

/**
 * Default header.
 */
#define WINDOW_SIZE_OFFSET 34
byte_t default_header[] = {
   0x45, 0x08, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x40, 0x06,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x50, 0x00,
   // TCP Window size
   0x06, 0x00, // ~1.5KB by default
   0x00, 0x00, 0x00, 0x00
};

/**
 * Flags received within last packet.
 */
byte_t _flags;

void remove_rx_data(SOCKET *_sckt);


/**
 * TCP timing control task.
 * Called periodically at a rate defined by TICK_TCP.
 */
byte_t nwk_tick (byte_t sig)
{
   static byte_t t=0;

   /*
    * Loop all sockets.
    */
   for_each(_sockets, _sckt) {
      if(_sckt->type != SOCKET_TCP) continue;               // UDP socket or unused.


      //      if(_sckt->time == 0) continue;                        // does not have timing requirements.

      /*
       * Do socket timing.
       */
      if (_sckt->time) _sckt->time--;
      if(_sckt->time == 0) {
         /*
          * Timeout.
          * Check retransmissions.
          */
         if(_sckt->retry) {
#ifdef DEBUG_TCP_RETRIES
	   printf("tcp retry %d\n",_sckt->retry);
#endif
            _sckt->retry--;
	    _sckt->time = SOCKET_TIMEOUT(_sckt);
            switch(_sckt->state) {
               case _SYN_SENT:
               case _ACK_REC:
                  _sckt->toSend = SYN;
                  break;
               case _SYN_REC:
                  _sckt->toSend = SYN | ACK;
                  break;
               case _ACK_WAIT:
                  _sckt->toSend = ACK | PSH;
                   break;
               case _FIN_SENT:
               case _FIN_ACK_REC:
                  _sckt->toSend = ACK;
#ifdef DEBUG_ACK
		  debug_msg("Asserting ACK: _FIN_ACK_REC state");
#endif
                  break;
               case _FIN_REC:
                  _sckt->toSend = FIN | ACK;
#ifdef DEBUG_ACK
		  debug_msg("Asserting ACK: _FIN_REC state");
#endif
                  break;
               default:
		 _sckt->time = SOCKET_TIMEOUT(_sckt);
                  _sckt->timeout = FALSE;
                  break;
            }
            if(_sckt->toSend) {
               /*
                * Force nwk_upstream() to execute.
                */
	      _sckt->timeout = TRUE;
#ifdef INSTANT_ACK
	      nwk_upstream(0);
#endif
#ifdef DEBUG_ACK
	      debug_msg("scheduling nwk_upstream 0 0");
#endif
	      task_cancel(nwk_upstream);
	      task_add(nwk_upstream, 0, 0,"upstream");
            } 
        } else {
            /*
             * Too much retransmissions.
             * Socket down.
             */
            _sckt->state = _IDLE;
	    _sckt->callback(WEEIP_EV_DISCONNECT);
	    remove_rx_data(_sckt);
         }
      }
   } 
   
   /*
    * Reschedule task for periodic execution.
    */
   task_add(nwk_tick, TICK_TCP, 0,"nwktick");
   return 0;
}

void remove_rx_data(SOCKET *_sckt)
{
  if (!_sckt->rx_data) {
    _sckt->rx_data=0;
    return;
  }
  if (_sckt->rx_oo_start) {
    lcopy((unsigned long)_sckt->rx + _sckt->rx_data, (unsigned long)_sckt->rx,
	  _sckt->rx_oo_end - _sckt->rx_data);
    _sckt->rx_oo_start -= _sckt->rx_data;
    _sckt->rx_oo_end -= _sckt->rx_data;
  }
  _sckt->rx_data=0;
  
}

void compute_window_size(SOCKET *_sckt)
{
  unsigned short available_window=0;
  // Now patch the header to take account of how much buffer space we _actually_ have available
  if (_sckt->rx_data>available_window) available_window=_sckt->rx_data;
  if (_sckt->rx_oo_end>available_window) available_window=_sckt->rx_oo_end;
  available_window=_sckt->rx_size - available_window;
  default_header[WINDOW_SIZE_OFFSET+0]=available_window>>8;
  default_header[WINDOW_SIZE_OFFSET+1]=available_window>>0;
}

/**
 * Network upstream task. Send outgoing network messages.
 */
byte_t nwk_upstream (byte_t sig)
{
      
#ifdef DEBUG_ACK
   debug_msg("nwk_upstream called.");
#endif
   
   if(!eth_clear_to_send()) {
      /*
       * Ethernet not ready.
       * Delay task execution.
       */
     //     printf("ETH TX wait\n");
#ifdef DEBUG_ACK
     debug_msg("scheduling nwk_upstream 2 0");
#endif
     task_add(nwk_upstream, 2, 0,"upstream");
      return 0;
   }
   
   /*
    * Search for pending messages.
    */
   for_each(_sockets, _sckt) {     
      if(!_sckt->toSend) continue;                          // no message to send for this socket.

#ifdef DEBUG_ACK
      debug_msg("nwk_upstream sending a packet for socket");
#endif
      
      /*
       * Pending message found, send it.
       */
      checksum_init();

      compute_window_size(_sckt);
      
      // XXX Correct buffer offset processing to handle variable
      // header lengths
      lcopy((uint32_t)default_header,(uint32_t)_header.b,40);

      IPH(id) = HTONS(id);
      id++;

      IPH(source).d = ip_local.d;
      IPH(destination).d = _sckt->remIP.d;
      TCPH(source) = _sckt->port;
      TCPH(destination) = _sckt->remPort;
      
      /*
       * Check payload area in _sckt->tx.
       */
      if(_sckt->toSend & PSH) {
         data_size = _sckt->tx_size;
         ip_checksum((byte_t*)_sckt->tx, data_size);
      } else data_size = 0;
      
      if(_sckt->type == SOCKET_TCP) {
         /*
          * TCP message header.
          */
         IPH(length) = HTONS((40 + data_size));
         TCPH(flags) = _sckt->toSend;

         /*
          * Check sequence numbers.
          */
         seq.d = _sckt->seq.d;
         if(_sckt->timeout) {
            /*
             * Retransmission.
             * Use old sequence number.
             */
            if(data_size) seq.d -= data_size;

	    // XXX Why on earth do we subtract one here?
	    // This messes up connections sometimes, because
	    // we SYN, get SYN+ACK, and have sent a 2nd SYN
	    // again before we read the SYN+ACK. But in the process
	    // we have of course decremented the sequence number
	    // by one, so the other side gets VERY confused, and says
	    // RST!
	    //            if(_sckt->toSend & (SYN | FIN)) seq.d--;
         }

         TCPH(n_seq).b[0] = seq.b[3];
         TCPH(n_seq).b[1] = seq.b[2];
         TCPH(n_seq).b[2] = seq.b[1];
         TCPH(n_seq).b[3] = seq.b[0];
         TCPH(n_ack).b[0] = _sckt->remSeq.b[3];
         TCPH(n_ack).b[1] = _sckt->remSeq.b[2];
         TCPH(n_ack).b[2] = _sckt->remSeq.b[1];
         TCPH(n_ack).b[3] = _sckt->remSeq.b[0];

	 if (_sckt->remSeq.d-_sckt->remSeqStart.d)
	   //	   printf("ACKing %ld\n",_sckt->remSeq.d-_sckt->remSeqStart.d+data_size);

         if(!_sckt->timeout) {
            /*
             * Update sequence number data.
             */
            if(data_size) seq.d += data_size;
            if(_sckt->toSend & (SYN | FIN)) seq.d++;
            _sckt->seq.d = seq.d;
         }

         /*
          * Update TCP checksum information.
          */
         TCPH(checksum) = 0;
         ip_checksum(&_header.b[12], 8 + sizeof(TCP_HDR));
         add_checksum(IP_PROTO_TCP);
         add_checksum(data_size + sizeof(TCP_HDR));
         TCPH(checksum) = checksum_result();
      } else {
         /*
          * UDP message header.
          */
         IPH(protocol) = IP_PROTO_UDP;
         IPH(length) = HTONS((28 + data_size));
         UDPH(length) = HTONS((8 + data_size));

         /*
          * Update UDP checksum information.
          */
         UDPH(checksum) = 0;
         ip_checksum(&_header.b[12], 8 + sizeof(UDP_HDR));
         add_checksum(IP_PROTO_UDP);
         add_checksum(data_size + sizeof(UDP_HDR));
         UDPH(checksum) = checksum_result();

         /*
          * Tell UDP that data was sent (no acknowledge).
          */
	 _sckt->callback(WEEIP_EV_DATA_SENT);
	 remove_rx_data(_sckt);
      }
      
      /*
       * Update IP checksum information.
       */
      checksum_init();
      ip_checksum((byte_t*)&_header, 20);
      IPH(checksum) = checksum_result();
      
      /*
       * Send IP packet.
       */
      if(eth_ip_send()) {
         if(data_size) eth_write((byte_t*)_sckt->tx, data_size);
#ifdef DEBUG_ACK
	 debug_msg("eth_packet_send() called");
#endif
         eth_packet_send();

	 _sckt->toSend = 0;
	 _sckt->timeout = FALSE;
	 _sckt->time = SOCKET_TIMEOUT(_sckt);
	 
      } else {
	// Sending the IP packet failed, possibly because there was no ARP
	// entry for the requested IP, if it is on the local network.

	// So we don't clear the status that we need to send
      }

      /*
       * Reschedule 50ms later for eventual further processing.
       */
#ifdef DEBUG_ACK
     debug_msg("scheduling nwk_upstream 5 0");
#endif
     task_add(nwk_upstream, 5, 0,"upstream");
   }
   
   /*
    * Job done, finish.
    */
   return 0;
}

unsigned long byte_order_swap_d(unsigned long in)
{
  unsigned long out;
  ((unsigned char *)&out)[0]=((unsigned char *)&in)[3];
  ((unsigned char *)&out)[1]=((unsigned char *)&in)[2];
  ((unsigned char *)&out)[2]=((unsigned char *)&in)[1];
  ((unsigned char *)&out)[3]=((unsigned char *)&in)[0];
  return out;
}

void nwk_schedule_oo_ack(SOCKET *_sckt)
{
  /*
   * Out of order, send our number.
   */
#if 0
  printf("request OOO ack: %ld != %ld\n",
	 byte_order_swap_d(TCPH(n_seq.d))-_sckt->remSeqStart.d,_sckt->remSeq.d-_sckt->remSeqStart.d);
#endif  
  _sckt->toSend = ACK;
#ifdef INSTANT_ACK
  nwk_upstream(0);
#endif
#ifdef DEBUG_ACK
  debug_msg("asserting ack: Out-of-order rx");
  debug_msg("scheduling nwk_upstream 0 0");
#endif
  task_cancel(nwk_upstream);
  task_add(nwk_upstream, 0, 0,"upstream");
}

/**
 * Network downstream processing.
 * Parse incoming network messages.
 */
void nwk_downstream(void)
{
   WEEIP_EVENT ev;
   _uint32_t rel_sequence;
   static unsigned char i;

   ev = WEEIP_EV_NONE;

   /*
    * Packet size.
    */
   if(IPH(ver_length) != 0x45) goto drop;
   data_size = IPH(length);
   data_size = NTOHS(data_size);

   /*
    * Checksum.
    */
   checksum_init();
   ip_checksum((byte_t*)&_header, 20);
   if(chks.u != 0xffff) goto drop;

#if 0
   printf("\nI am %d.%d.%d.%d\n",ip_local.b[0],ip_local.b[1],ip_local.b[2],ip_local.b[3]);
   printf("P=%02x: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d\n",
	  IPH(protocol),
	  IPH(source).b[0],IPH(source).b[1],IPH(source).b[2],IPH(source).b[3],
	  NTOHS(TCPH(source)),
	  IPH(destination).b[0],IPH(destination).b[1],IPH(destination).b[2],IPH(destination).b[3],
	  NTOHS(TCPH(destination)));
#endif   
   /*
    * Destination address.
    */
   if(IPH(destination).d != 0xffffffffL)                       // broadcast.
      if(IPH(destination).d != ip_local.d)                     // unicast.
	if(IPH(destination).d != ip_broadcast.d)                     // unicast.
	  if (ip_local.d != 0x0000000L)                          // Waiting for DHCP configuration
	    goto drop;                                           // not for us.

   if(IPH(protocol) == IP_PROTO_ICMP) goto parse_icmp;

   /*
    * Search for a waiting socket.
    */
   for_each(_sockets, _sckt) {
      if(_sckt->type == SOCKET_FREE) continue;                 // unused socket.
      if(_sckt->port != TCPH(destination)) continue;           // another port.
      if(_sckt->type == SOCKET_UDP) {                          // another protocol.
         if(IPH(protocol) != IP_PROTO_UDP) continue;
      } else {
	if(IPH(protocol) != IP_PROTO_TCP) continue;
      }
      if(_sckt->listening) goto found;                         // waiting for a connection.
      // Don't check source if we are bound to broadcast
      if(_sckt->remIP.d!=0xffffffffL) {
	if(_sckt->remIP.d != IPH(source).d) continue;            // another source.
      }
      if(_sckt->remPort != TCPH(source)) continue;             // another port.
      goto found;                                              // found!
   }

   goto drop;                                                  // no socket for the message.

found:
   /*
    * Update socket data.
    */
   //   printf("found socket: source.d=$%08lx\n",IPH(source).d);
   _sckt->remIP.d = IPH(source).d;
   _sckt->remPort = TCPH(source);
   _sckt->listening = FALSE;

   if(IPH(protocol) == IP_PROTO_TCP) goto parse_tcp;

   /*
    * UDP message.
    * Copy data into user socket buffer.
    * Add task for processing.
    */
   data_size -= 28;
   if(_sckt->rx) {
      if(data_size > _sckt->rx_size) data_size = _sckt->rx_size;
      lcopy(ETH_RX_BUFFER+2+14+sizeof(IP_HDR)+8,(uint32_t)_sckt->rx, data_size);
      _sckt->rx_data = data_size;
   }
   
   ev = WEEIP_EV_DATA;
   goto done;

parse_tcp:

   //   printf(":");
   
   /*
    * TCP message.
    * Check flags.
    */
   _flags = 0;
   data_size -= 40;

   if(TCPH(flags) & ACK) {
      /*
       * Test acked sequence number.
       */
     if((_sckt->seq.b[0] != TCPH(n_ack).b[3])
          || (_sckt->seq.b[1] != TCPH(n_ack).b[2])
          || (_sckt->seq.b[2] != TCPH(n_ack).b[1])
          || (_sckt->seq.b[3] != TCPH(n_ack).b[0])) {
           /*
            * Out of order, drop it.
	    * XXX This is when the other side has ACKed a different part
	    * of our stream.
	    * PGS: I think we lock-step and have only one unacknowledged packet
	    * at a time, so can effectively ignore it (but should probably be
	    * clever and re-send?)
            */
       if (_sckt->state>=_CONNECT) {
	 printf("Drop\n");
	 goto drop;
       }
      }
      _flags |= ACK;
   }

   if(TCPH(flags) & SYN) {
      /*
       * Restart of remote sequence number (connection?).
       */
      _sckt->remSeq.b[0] = TCPH(n_seq).b[3];
      _sckt->remSeq.b[1] = TCPH(n_seq).b[2];
      _sckt->remSeq.b[2] = TCPH(n_seq).b[1];
      _sckt->remSeq.b[3] = TCPH(n_seq).b[0];

      // Remember initial remote sequence # for convenient debugging
      _sckt->remSeqStart.d=_sckt->remSeq.d;      
      
      _sckt->remSeq.d++;
      _flags |= SYN;

      //      printf("SYN%d",_sckt->state);
      
   } else {
      /*
       * Test remote sequence number.
       */

     if(data_size > _sckt->rx_size) data_size = _sckt->rx_size;
     // XXX Correct buffer offset processing to handle variable
     // header lengths
     data_ofs=((IPH(ver_length)&0x0f)<<2)+((TCPH(hlen)>>4)<<2);
     
     for(i=0;i<4;i++) rel_sequence.b[i]=TCPH(n_seq.b[3-i]);
     rel_sequence.d-=_sckt->remSeq.d;

#if 0
     printf("\n%5ld: rel_seq=%ld, rx:%d,%d to %d\n",
	    _sckt->remSeq.d-_sckt->remSeqStart.d,
	    rel_sequence.d,
	    _sckt->rx_data,
	    _sckt->rx_oo_start,_sckt->rx_oo_end);
#endif
     
     if (rel_sequence.d>_sckt->rx_size || rel_sequence.d+data_size>_sckt->rx_size) {
       // Ignore segments that we can't possibly handle
       //       printf("drop(a)");
       if (data_size) { nwk_schedule_oo_ack(_sckt); goto drop; }
     } else if (rel_sequence.w[0]==_sckt->rx_data) {
       // Copy to end of data in RX buffer
       //       printf("rx append %d@%d",data_size,_sckt->rx_data);
       if (data_size+_sckt->rx_data>_sckt->rx_size)
	 data_size=_sckt->rx_size-_sckt->rx_data;
       if (data_size) {
	 lcopy(ETH_RX_BUFFER+16+data_ofs,_sckt->rx_data + (uint32_t)_sckt->rx, data_size);
       }
       _sckt->rx_data += data_size;       
     } else if (rel_sequence.w[0]==_sckt->rx_oo_end) {
       // Copy to end of OO data in RX buffer
       // printf("oo append");
       if (data_size+_sckt->rx_oo_end>_sckt->rx_size)
	 data_size=_sckt->rx_size-_sckt->rx_oo_end;
       if (data_size) {
	 lcopy(ETH_RX_BUFFER+16+data_ofs,_sckt->rx_oo_end + (uint32_t)_sckt->rx, data_size);	 
       }
       _sckt->rx_oo_end += data_size;
     } else if ((rel_sequence.w[0]+data_size)==_sckt->rx_oo_start) {
       // Copy to start of OO data in RX buffer
       //       printf("oo prepend");
       if (data_size) {
	 lcopy(ETH_RX_BUFFER+16+data_ofs,
	       rel_sequence.w[0] + ((unsigned long)_sckt->rx), data_size);	 
       }
       _sckt->rx_oo_end += rel_sequence.w[0];
     } else if ((rel_sequence.w[0]+data_size)<_sckt->rx_size&&!_sckt->rx_oo_start) {
       // It belongs in the window, but not at the start, so put in RX OO buffer
       //       printf("oo stash");
       if (data_size) {
	 lcopy(ETH_RX_BUFFER+16+data_ofs,rel_sequence.w[0] + (uint32_t)_sckt->rx, data_size);	 
       }
       _sckt->rx_oo_start = rel_sequence.w[0];
       _sckt->rx_oo_end = rel_sequence.w[0] + data_size;
     } else if (rel_sequence.d) {
       //       printf("drop(b)");
       if (data_size) { nwk_schedule_oo_ack(_sckt); goto drop; }
     }

     //     while(!PEEK(0xD610)) continue; POKE(0xD610,0);
     
     // Merge received data and RX OO area, if possible
     if (_sckt->rx_data&&_sckt->rx_data==_sckt->rx_oo_start) {
       _sckt->rx_data=_sckt->rx_oo_end;
       _sckt->rx_oo_end=0;
       _sckt->rx_oo_start=0;
     }
     
      /*
       * Update stream sequence number.
       */
      _sckt->remSeq.d += _sckt->rx_data;

      // Deliver data to programme
      if (_sckt->rx_data) {
	_sckt->callback(WEEIP_EV_DATA);
	remove_rx_data(_sckt);
      }
      
      // And ACK every packet, because we don't have buffer space for multiple ones,
      // and its thus very easy for the sender to not know where we are upto, and for
      // everything to generally get very confused if we have encountered any out-of-order
      // packets.
      _sckt->toSend = ACK;
      
   }

   // XXX PGS moved this lower down, so that we can check any included data has the correct sequence
   // number, so that we can pass the data up, if required, so that data included in the RST
   // packet doesn't get lost (Boar's Head BBS does this, for example)
   if(TCPH(flags) & RST) {
      /*
       * RST flag received. Force disconnection.
       */
      _sckt->state = _IDLE;
      if (!data_size) {
	// No data, so just disconnect
	ev = WEEIP_EV_DISCONNECT;
      } else {
	// Disconnect AND valid data
	ev = WEEIP_EV_DISCONNECT_WITH_DATA;
      }
      goto done;
   }

   // If FIN flag is set, then we also acknowledge all data so far,
   // plus the FIN flag.
   if(TCPH(flags) & FIN||_sckt->state==_FIN_REC) {
     
     _sckt->remSeq.b[3]=TCPH(n_seq.b[0]);
     _sckt->remSeq.b[2]=TCPH(n_seq.b[1]);
     _sckt->remSeq.b[1]=TCPH(n_seq.b[2]);
     _sckt->remSeq.b[0]=TCPH(n_seq.b[3]);
     
     _sckt->remSeq.d+=data_size;
     _sckt->remSeq.d++;
     _flags |= FIN;
     //      printf("FIN ACK = %ld\n",_sckt->remSeq.d-_sckt->remSeqStart.d);
   }
   
   /*
    * TCP state machine implementation.
    */
   switch(_sckt->state) {
      case _LISTEN:
         if(_flags & SYN) {
            /*
             * Start incoming connection procedure.
             */
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _listen state");
#endif
            _sckt->state = _SYN_REC;
            _sckt->toSend = SYN | ACK;
         }
         break;
         
      case _SYN_SENT:

         if(_flags & (ACK | SYN)) {
            /*
             * Connection established.
             */
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _syn_sent state with syn or ack");
#endif
            _sckt->state = _CONNECT;
            _sckt->toSend = ACK;

	    // Advance sequence # by one to ack the ack
	    _sckt->seq.d++;
	    
            ev = WEEIP_EV_CONNECT;
	    //printf("Saw SYN\n");
	    break;
         }

         if(_flags & SYN) {
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _syn_sent state with syn");
#endif
            _sckt->state = _SYN_REC;
            _sckt->toSend = SYN | ACK;
         }         
         break;
      	
	 
      case _SYN_REC:
         if(_flags & ACK) {
            /*
             * Connection established.
             */
            _sckt->state = _CONNECT;
            ev = WEEIP_EV_CONNECT;
         }
         break;

      case _CONNECT:
      case _ACK_WAIT:
         if(_flags & FIN) {
            /*
             * Start remote disconnection procedure.
             */
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _ack_wait state");
#endif
            _sckt->state = _FIN_REC;
            _sckt->toSend = ACK | FIN;
            ev = WEEIP_EV_DISCONNECT;           // TESTE
            break;
         }

         if((_flags & ACK) && (_sckt->state == _ACK_WAIT)) {
            /*
             * The peer acknowledged the previously sent data.
             */
            _sckt->state = _CONNECT;
         }         

         if(data_size) {
            /*
             * Data received.
             */
#ifdef DEBUG_ACK
	    debug_msg("asserting ack: data received");
#endif
            _sckt->toSend = ACK;            
            ev = WEEIP_EV_DATA;
         }
         break;
         
      case _FIN_SENT:

	if(_flags & (FIN | ACK)) {
            /*
             * Disconnection done.
             */
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _fin_sent state with fin or ack");
#endif
	   _sckt->state = _IDLE;
            _sckt->toSend = ACK;               
            ev = WEEIP_EV_DISCONNECT;
	    // Allow the final state to be selected based on ACK and FIN flag state.
	    //            break;
         }

         if(_flags & ACK) {
            _sckt->state = _FIN_ACK_REC;
         }

         if(_flags & FIN) {
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _fin_ack_rec state with fin");
#endif
            _sckt->state = _FIN_REC;
            _sckt->toSend = ACK;
            break;
         }
	
         break;
	 
      case _FIN_REC:
         if(_flags & ACK) {
            /*
             * Disconnection done.
             */
            _sckt->state = _IDLE;
            ev = WEEIP_EV_DISCONNECT;
         }
         break;

      case _FIN_ACK_REC:
         if(_flags & FIN) {
            /*
             * Disconnection done.
             */
#ifdef DEBUG_ACK
	   debug_msg("asserting ack: _fin_ack_rec state with fin");
#endif
            _sckt->state = _FIN_REC;
            _sckt->toSend = ACK;
            ev = WEEIP_EV_DISCONNECT;
         }         
         break;
         
      default:
         break;
   }

   goto done;

 parse_icmp:

   /*
     Parse ICMP messages.
     Only care about ECHO REQUEST for now
   */
#ifdef ENABLE_ICMP
   if (ICMPH(type)==0x08) {
     if (ICMPH(fcode)==0x00) {
       // ICMP Echo request: 

       // 0. Copy received packet to tx buffer
       lcopy(ETH_RX_BUFFER+2L,(long)tx_frame_buf,14+data_size);

       // 1. Copy Eth src to DST
       lcopy((long)&tx_frame_buf[0+6],(long)&tx_frame_buf[0],6);

       // 2. Put our ETH as src
       lcopy((long)0xD6E9,(long)&tx_frame_buf[0+6],6);
       
       // 3. IP SRC becomes DST
       lcopy((long)&tx_frame_buf[14+12],(long)&tx_frame_buf[14+16],4);
       
       // 4. Put our IP as SRC
       lcopy((long)&ip_local.b[0],(long)&tx_frame_buf[14+12],4);
       
       // 5. Change type from 0x08 (ECHO REQUEST) to 0x00 (ECHO REPLY)
       tx_frame_buf[14+20]=0x00;
       
       // 6. Update ICMP checksum
       tx_frame_buf[14+20+2]=0; tx_frame_buf[14+20+2+1]=0;
       chks.b[0]=0; chks.b[1]=0;
       ip_checksum(&tx_frame_buf[14+20],data_size);
       // XXX For some reason if we do the following assignment as single
       // operation, it crashes the programme. But doing it this way,
       // seems to be fine. Go figure :/
       tx_frame_buf[14+20+2] = (checksum_result());
       tx_frame_buf[14+20+3] = (checksum_result())>>8;
       
       // 7. Update IP checksum
       tx_frame_buf[14+10]=0; tx_frame_buf[14+11]=0;
       chks.b[0]=0; chks.b[1]=0;
       ip_checksum(&tx_frame_buf[14],20);
       *(unsigned short *)&tx_frame_buf[14+10] = checksum_result();

       // Send immediately
       eth_tx_len=14+data_size;
       eth_packet_send();
     }
   }
#endif   

   goto drop;
   
done:
   /*
    * Verify if there are messages to send.
    * Add nwk_upstream() to send messages.
    */
   if(_sckt->toSend) {
      _sckt->retry = RETRIES_TCP;
#ifdef INSTANT_ACK
      nwk_upstream(0);
#endif
#ifdef DEBUG_ACK
      debug_msg("scheduling nwk_upstream 0 0");
#endif
      task_cancel(nwk_upstream);
      task_add(nwk_upstream, 0, 0,"upstream");
   }

   /*
    * Verify event processing.
    * Add socket management task.
    */
   if(ev != WEEIP_EV_NONE) {
     _sckt->callback(ev);
     remove_rx_data(_sckt);
   }

drop:
   return;
}
