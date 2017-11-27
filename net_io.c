// Part of dump1090, a Mode S message decoder for RTLSDR devices.
//
// net_io.c: network handling.
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you may copy, redistribute and/or modify it  
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 2 of the License, or (at your  
// option) any later version.  
//
// This file is distributed in the hope that it will be useful, but  
// WITHOUT ANY WARRANTY; without even the implied warranty of  
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU  
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License  
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// This file incorporates work covered by the following copyright and  
// permission notice:
//
//   Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met:
//
//    *  Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//    *  Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "dump868.h"

/* for PRIX64 */
#include <inttypes.h>

#include <assert.h>

//
// ============================= Networking =============================
//
// Note: here we disregard any kind of good coding practice in favor of
// extreme simplicity, that is:
//
// 1) We only rely on the kernel buffers for our I/O without any kind of
//    user space buffering.
// 2) We don't register any kind of event handler, from time to time a
//    function gets called and we accept new connections. All the rest is
//    handled via non-blocking I/O and manually polling clients to see if
//    they have something new to share with us when reading is needed.


//static int decodeBinMessage(struct client *c, char *p);
//static int decodeHexMessage(struct client *c, char *hex);
//#ifdef ENABLE_WEBSERVER
//static int handleHTTPRequest(struct client *c, char *p);
//#endif

//static void send_raw_heartbeat(struct net_service *service);
static void send_beast_heartbeat(struct net_service *service);
//static void send_sbs_heartbeat(struct net_service *service);

//static void writeFATSVEvent(struct modesMessage *mm, struct aircraft *a);

//
//=========================================================================
//
// Networking "stack" initialization
//

// Init a service with the given read/write characteristics, return the new service.
// Doesn't arrange for the service to listen or connect
struct net_service *serviceInit(const char *descr, struct net_writer *writer, heartbeat_fn hb, const char *sep, read_fn handler)
{
    fprintf(stderr, "Starting service");

    struct net_service *service;

    if (!(service = calloc(sizeof(*service), 1))) {
        fprintf(stderr, "Out of memory allocating service %s\n", descr);
        exit(1);
    }

    service->next = DumpFLARM.services;
    DumpFLARM.services = service;

    service->descr = descr;
    service->listener_count = 0;
    service->connections = 0;
    service->writer = writer;
    service->read_sep = sep;
    service->read_handler = handler;

    if (service->writer) {
        if (! (service->writer->data = malloc(MODES_OUT_BUF_SIZE)) ) {
            fprintf(stderr, "Out of memory allocating output buffer for service %s\n", descr);
            exit(1);
        }

        service->writer->service = service;
        service->writer->dataUsed = 0;
        service->writer->lastWrite = mstime();
        service->writer->send_heartbeat = hb;
    }

    return service;
}

// Create a client attached to the given service using the provided socket FD
struct client *createSocketClient(struct net_service *service, int fd)
{
    anetSetSendBuffer(DumpFLARM.aneterr, fd, (MODES_NET_SNDBUF_SIZE << DumpFLARM.net_sndbuf_size));
    return createGenericClient(service, fd);
}

// Create a client attached to the given service using the provided FD (might not be a socket!)
struct client *createGenericClient(struct net_service *service, int fd)
{
    struct client *c;

    anetNonBlock(DumpFLARM.aneterr, fd);

    if (!(c = (struct client *) malloc(sizeof(*c)))) {
        fprintf(stderr, "Out of memory allocating a new %s network client\n", service->descr);
        exit(1);
    }

    c->service    = service;
    c->next       = DumpFLARM.clients;
    c->fd         = fd;
    c->buflen     = 0;
    DumpFLARM.clients = c;


    fprintf(stderr, "Increasing server connections\n");

    ++service->connections;
    if (service->writer && service->connections == 1) {
        service->writer->lastWrite = mstime(); // suppress heartbeat initially
    }

    return c;
}

// Initiate an outgoing connection which will use the given service.
// Return the new client or NULL if the connection failed
struct client *serviceConnect(struct net_service *service, char *addr, int port)
{
    int s;
    char buf[20];

    // Bleh.
    snprintf(buf, 20, "%d", port);
    s = anetTcpConnect(DumpFLARM.aneterr, addr, buf);
    if (s == ANET_ERR)
        return NULL;

    return createSocketClient(service, s);
}

// Set up the given service to listen on an address/port.
// _exits_ on failure!
void serviceListen(struct net_service *service, char *bind_addr, char *bind_ports)
{
    int *fds = NULL;
    int n = 0;
    char *p, *end;
    char buf[128];

    if (service->listener_count > 0) {
        fprintf(stderr, "Tried to set up the service %s twice!\n", service->descr);
        exit(1);
    }

    if (!bind_ports || !strcmp(bind_ports, "") || !strcmp(bind_ports, "0"))
        return;

    p = bind_ports;
    while (p && *p) {
        int newfds[16];
        int nfds, i;

        end = strpbrk(p, ", ");
        if (!end) {
            strncpy(buf, p, sizeof(buf));
            buf[sizeof(buf)-1] = 0;
            p = NULL;
        } else {
            size_t len = end - p;
            if (len >= sizeof(buf))
                len = sizeof(buf) - 1;
            memcpy(buf, p, len);
            buf[len] = 0;
            p = end + 1;
        }

        nfds = anetTcpServer(DumpFLARM.aneterr, buf, bind_addr, newfds, sizeof(newfds));
        if (nfds == ANET_ERR) {
            fprintf(stderr, "Error opening the listening port %s (%s): %s\n",
                    buf, service->descr, DumpFLARM.aneterr);
            exit(1);
        }

        fds = realloc(fds, (n+nfds) * sizeof(int));
        if (!fds) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }

        for (i = 0; i < nfds; ++i) {
            anetNonBlock(DumpFLARM.aneterr, newfds[i]);
            fds[n++] = newfds[i];
        }
    }

    service->listener_count = n;
    service->listener_fds = fds;
}

//struct net_service *makeBeastInputService(void)
//{
//    return serviceInit("Beast TCP input", NULL, NULL, NULL, decodeBinMessage);
//}

//struct net_service *makeFatsvOutputService(void)
//{
//    return serviceInit("FATSV TCP output", &DumpFLARM.fatsv_out, NULL, NULL, NULL);
//}

void modesInitNet(void) {

    fprintf(stderr, "Starting Net\n");

    struct net_service *s;

    signal(SIGPIPE, SIG_IGN);
    DumpFLARM.clients = NULL;
    DumpFLARM.services = NULL;

    // set up listeners
    //s = serviceInit("Raw TCP output", &DumpFLARM.raw_out, send_raw_heartbeat, NULL, NULL);
    //serviceListen(s, DumpFLARM.net_bind_address, DumpFLARM.net_output_raw_ports);

    s = serviceInit("Beast TCP output", &DumpFLARM.beast_out, send_beast_heartbeat, NULL, NULL);
    serviceListen(s, DumpFLARM.net_bind_address, DumpFLARM.net_output_beast_ports);
//
//    s = serviceInit("Basestation TCP output", &DumpFLARM.sbs_out, send_sbs_heartbeat, NULL, NULL);
//    serviceListen(s, DumpFLARM.net_bind_address, DumpFLARM.net_output_sbs_ports);
//
//    s = serviceInit("Raw TCP input", NULL, NULL, "\n", decodeHexMessage);
//    serviceListen(s, DumpFLARM.net_bind_address, DumpFLARM.net_input_raw_ports);
//
//    s = makeBeastInputService();
//    serviceListen(s, DumpFLARM.net_bind_address, DumpFLARM.net_input_beast_ports);

#ifdef ENABLE_WEBSERVER
//    s = serviceInit("HTTP server", NULL, NULL, "\r\n\r\n", handleHTTPRequest);
//    serviceListen(s, DumpFLARM.net_bind_address, DumpFLARM.net_http_ports);
#endif
}
//
//=========================================================================
//
// This function gets called from time to time when the decoding thread is
// awakened by new data arriving. This usually happens a few times every second
//
static struct client * modesAcceptClients(void) {
    int fd;
    struct net_service *s;

    for (s = DumpFLARM.services; s; s = s->next) {
        int i;
        for (i = 0; i < s->listener_count; ++i) {
            while ((fd = anetTcpAccept(DumpFLARM.aneterr, s->listener_fds[i])) >= 0) {
                createSocketClient(s, fd);
            }
        }
    }

    return DumpFLARM.clients;
}
//
//=========================================================================
//
// On error free the client, collect the structure, adjust maxfd if needed.
//
static void modesCloseClient(struct client *c) {
    if (!c->service) {
        fprintf(stderr, "warning: double close of net client\n");
        return;
    }

    // Clean up, but defer removing from the list until modesNetCleanup().
    // This is because there may be stackframes still pointing at this
    // client (unpredictably: reading from client A may cause client B to
    // be freed)

    close(c->fd);
    c->service->connections--;

    // mark it as inactive and ready to be freed
    c->fd = -1;
    c->service = NULL;
}
//
//=========================================================================
//
// Send the write buffer for the specified writer to all connected clients
//
static void flushWrites(struct net_writer *writer) {
    struct client *c;

    //fprintf(stderr, "flusheWrites\n");

    for (c = DumpFLARM.clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->service == writer->service) {
#ifndef _WIN32
            int nwritten = write(c->fd, writer->data, writer->dataUsed);
#else
            int nwritten = send(c->fd, writer->data, writer->dataUsed, 0 );

            fprintf(stderr, "nwritten is %d\n",nwritten);
#endif
            if (nwritten != writer->dataUsed) {
                modesCloseClient(c);
            }
        }
    }

    writer->dataUsed = 0;
    writer->lastWrite = mstime();
}

// Prepare to write up to 'len' bytes to the given net_writer.
// Returns a pointer to write to, or NULL to skip this write.
static void *prepareWrite(struct net_writer *writer, int len) {
    //if (!writer ||
    //    !writer->service ||
    //    !writer->service->connections ||
    //    !writer->data) {

        if (!writer ||
                !writer->service||
                    !writer->service->connections ) {

        //fprintf(stderr, "Problem with the writer occured\n Connections are: %d",writer->service->connections);
        return NULL;
    }

    if (len > MODES_OUT_BUF_SIZE){

        fprintf(stderr, "length bigger than buffer\n");
        return NULL;
    }


    if (writer->dataUsed + len >= MODES_OUT_BUF_SIZE) {
        // Flush now to free some space
        flushWrites(writer);
    }

    return writer->data + writer->dataUsed;
}

// Complete a write previously begun by prepareWrite.
// endptr should point one byte past the last byte written
// to the buffer returned from prepareWrite.
static void completeWrite(struct net_writer *writer, void *endptr) {

    //fprintf(stderr, "completeWrite\n");

    writer->dataUsed = endptr - writer->data;

    if (writer->dataUsed >= DumpFLARM.net_output_flush_size) {
        flushWrites(writer);
    }
}

//
//=========================================================================
//
// Write raw output in Beast Binary format with Timestamp to TCP clients
//
static void modesSendBeastOutput(struct modesMessage *mm) {

    //fprintf(stderr, "Preparing message\n");

    int msgLen = mm->msgbits / 8;
    char *p = prepareWrite(&DumpFLARM.beast_out, 2 + 2 * (7 + msgLen));
    char ch;
    int j;
    int sig;
    unsigned char *msg = (DumpFLARM.net_verbatim ? mm->verbatim : mm->msg);


    if (!p) {
    //fprintf(stderr, "p is true\n");
    return;
    }


    *p++ = 0x1a;

    *p++ = 0x38; //Special FLARM code



    /* timestamp, big-endian */
    *p++ = (ch = (mm->timestampMsg >> 56));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg >> 48));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg >> 40));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg >> 32));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg >> 24));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg >> 16));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg >> 8));
    if (0x1A == ch) {*p++ = ch; }
    *p++ = (ch = (mm->timestampMsg));
    if (0x1A == ch) {*p++ = ch; }

    //*p++ = 0xaa;

    sig = round(sqrt(mm->signalLevel) * 255);
    if (mm->signalLevel > 0 && sig < 1)
        sig = 1;
    if (sig > 255)
        sig = 255;
    *p++ = ch = (char)sig;
    if (0x1A == ch) {*p++ = ch; }

    //*p++=0xbb;

    for (j = 0; j < msgLen; j++) {
        *p++ = (ch = msg[j]);
        if (0x1A == ch) {*p++ = ch; }
    }

    //*p++=0xcc;




    completeWrite(&DumpFLARM.beast_out, p);
}

static void send_beast_heartbeat(struct net_service *service)
{
    static char heartbeat_message[] = { 0x1a, '1', 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    char *data;

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, sizeof(heartbeat_message));
    if (!data)
        return;

    memcpy(data, heartbeat_message, sizeof(heartbeat_message));
    completeWrite(service->writer, data + sizeof(heartbeat_message));
}


//=========================================================================
//
void modesQueueOutput(struct modesMessage *mm) {
//    int is_mlat = (mm->source == SOURCE_MLAT);
//
//    if (!is_mlat && mm->correctedbits < 2) {
//        // Don't ever forward 2-bit-corrected messages via SBS output.
//        // Don't ever forward mlat messages via SBS output.
//        modesSendSBSOutput(mm, a);
//    }
//
//    if (!is_mlat && (DumpFLARM.net_verbatim || mm->correctedbits < 2)) {
        // Forward 2-bit-corrected messages via raw output only if --net-verbatim is set
        // Don't ever forward mlat messages via raw output.
//        modesSendRawOutput(mm);
//    }

//    if ((!is_mlat || DumpFLARM.forward_mlat) && (DumpFLARM.net_verbatim || mm->correctedbits < 2)) {
//        // Forward 2-bit-corrected messages via beast output only if --net-verbatim is set
//        // Forward mlat messages via beast output only if --forward-mlat is set
        modesSendBeastOutput(mm);
   }
//
//    if (!is_mlat) {
//        writeFATSVEvent(mm, a);
//    }
//}


//
//=========================================================================
//



//
//=========================================================================
//
// This function polls the clients using read() in order to receive new
// messages from the net.
//
// The message is supposed to be separated from the next message by the
// separator 'sep', which is a null-terminated C string.
//
// Every full message received is decoded and passed to the higher layers
// calling the function's 'handler'.
//
// The handler returns 0 on success, or 1 to signal this function we should
// close the connection with the client in case of non-recoverable errors.
//
static void modesReadFromClient(struct client *c) {
    int left;
    int nread;
    int fullmsg;
    int bContinue = 1;
    char *s, *e, *p;

    while(bContinue) {

        fullmsg = 0;
        left = MODES_CLIENT_BUF_SIZE - c->buflen;
        // If our buffer is full discard it, this is some badly formatted shit
        if (left <= 0) {
            c->buflen = 0;
            left = MODES_CLIENT_BUF_SIZE;
            // If there is garbage, read more to discard it ASAP
        }
#ifndef _WIN32
        nread = read(c->fd, c->buf+c->buflen, left);
#else
        nread = recv(c->fd, c->buf+c->buflen, left, 0);
        if (nread < 0) {errno = WSAGetLastError();}
#endif

        // If we didn't get all the data we asked for, then return once we've processed what we did get.
        if (nread != left) {
            bContinue = 0;
        }

        if (nread == 0) { // End of file
            modesCloseClient(c);
            return;
        }

#ifndef _WIN32
        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) // No data available (not really an error)
#else
        if (nread < 0 && errno == EWOULDBLOCK) // No data available (not really an error)
#endif
        {
            return;
        }

        if (nread < 0) { // Other errors
            modesCloseClient(c);
            return;
        }

        c->buflen += nread;

        // Always null-term so we are free to use strstr() (it won't affect binary case)
        c->buf[c->buflen] = '\0';

        e = s = c->buf;                                // Start with the start of buffer, first message

        if (c->service->read_sep == NULL) {
            // This is the Beast Binary scanning case.
            // If there is a complete message still in the buffer, there must be the separator 'sep'
            // in the buffer, note that we full-scan the buffer at every read for simplicity.

            left = c->buflen;                                  // Length of valid search for memchr()
            while (left > 1 && ((s = memchr(e, (char) 0x1a, left)) != NULL)) { // The first byte of buffer 'should' be 0x1a
                s++;                                           // skip the 0x1a
                if        (*s == '1') {
                    e = s + MODEAC_MSG_BYTES      + 8;         // point past remainder of message
                } else if (*s == '2') {
                    e = s + MODES_SHORT_MSG_BYTES + 8;
                } else if (*s == '3') {
                    e = s + MODES_LONG_MSG_BYTES  + 8;
                } else {
                    e = s;                                     // Not a valid beast message, skip
                    left = &(c->buf[c->buflen]) - e;
                    continue;
                }
                // we need to be careful of double escape characters in the message body
                for (p = s; p < e; p++) {
                    if (0x1A == *p) {
                        p++; e++;
                        if (e > &(c->buf[c->buflen])) {
                            break;
                        }
                    }
                }
                left = &(c->buf[c->buflen]) - e;
                if (left < 0) {                                // Incomplete message in buffer
                    e = s - 1;                                 // point back at last found 0x1a.
                    break;
                }
                // Have a 0x1a followed by 1, 2 or 3 - pass message less 0x1a to handler.
                if (c->service->read_handler(c, s)) {
                    modesCloseClient(c);
                    return;
                }
                fullmsg = 1;
            }
            s = e;     // For the buffer remainder below

        } else {
            //
            // This is the ASCII scanning case, AVR RAW or HTTP at present
            // If there is a complete message still in the buffer, there must be the separator 'sep'
            // in the buffer, note that we full-scan the buffer at every read for simplicity.
            //
            while ((e = strstr(s, c->service->read_sep)) != NULL) { // end of first message if found
                *e = '\0';                         // The handler expects null terminated strings
                if (c->service->read_handler(c, s)) {               // Pass message to handler.
                    modesCloseClient(c);           // Handler returns 1 on error to signal we .
                    return;                        // should close the client connection
                }
                s = e + strlen(c->service->read_sep);               // Move to start of next message
                fullmsg = 1;
            }
        }

        if (fullmsg) {                             // We processed something - so
            c->buflen = &(c->buf[c->buflen]) - s;  //     Update the unprocessed buffer length
            memmove(c->buf, s, c->buflen);         //     Move what's remaining to the start of the buffer
        } else {                                   // If no message was decoded process the next client
            return;
        }
    }
}

#define TSV_MAX_PACKET_SIZE 275

//static void writeFATSVEventMessage(struct modesMessage *mm, const char *datafield, unsigned char *data, size_t len)
//{
//    char *p = prepareWrite(&DumpFLARM.fatsv_out, TSV_MAX_PACKET_SIZE);
//    if (!p)
//        return;
//
//    char *end = p + TSV_MAX_PACKET_SIZE;
//#       define bufsize(_p,_e) ((_p) >= (_e) ? (size_t)0 : (size_t)((_e) - (_p)))
//
//    p += snprintf(p, bufsize(p, end), "clock\t%" PRIu64, mstime() / 1000);
//
//    if (mm->addr & MODES_NON_ICAO_ADDRESS) {
//        p += snprintf(p, bufsize(p, end), "\totherid\t%06X", mm->addr & 0xFFFFFF);
//    } else {
//        p += snprintf(p, bufsize(p, end), "\thexid\t%06X", mm->addr);
//    }
//
//    if (mm->addrtype != ADDR_ADSB_ICAO) {
//        p += snprintf(p, bufsize(p, end), "\taddrtype\t%s", addrtype_short_string(mm->addrtype));
//    }
//
//    p += snprintf(p, bufsize(p, end), "\t%s\t", datafield);
//    for (size_t i = 0; i < len; ++i) {
//        p += snprintf(p, bufsize(p, end), "%02X", data[i]);
//    }
//
//    p += snprintf(p, bufsize(p, end), "\n");
//
//    if (p <= end)
//        completeWrite(&DumpFLARM.fatsv_out, p);
//    else
//        fprintf(stderr, "fatsv: output too large (max %d, overran by %d)\n", TSV_MAX_PACKET_SIZE, (int) (p - end));
//#       undef bufsize
//}
//
//static void writeFATSVEvent(struct modesMessage *mm, struct aircraft *a)
//{
//    // Write event records for a couple of message types.
//
//    if (!DumpFLARM.fatsv_out.service || !DumpFLARM.fatsv_out.service->connections) {
//        return; // not enabled or no active connections
//    }
//
//    if (a->messages < 2)  // basic filter for bad decodes
//        return;
//
//    switch (mm->msgtype) {
//    case 20:
//    case 21:
//        if (mm->correctedbits > 0)
//            break; // only messages we trust a little more
//
//        // DF 20/21: Comm-B: emit if they've changed since we last sent them
//        //
//        // BDS 1,0: data link capability report
//        // BDS 3,0: ACAS RA report
//        if (mm->MB[0] == 0x10 && memcmp(mm->MB, a->fatsv_emitted_bds_10, 7) != 0) {
//            memcpy(a->fatsv_emitted_bds_10, mm->MB, 7);
//            writeFATSVEventMessage(mm, "datalink_caps", mm->MB, 7);
//        }
//
//        else if (mm->MB[0] == 0x30 && memcmp(mm->MB, a->fatsv_emitted_bds_30, 7) != 0) {
//            memcpy(a->fatsv_emitted_bds_30, mm->MB, 7);
//            writeFATSVEventMessage(mm, "commb_acas_ra", mm->MB, 7);
//        }
//
//        break;
//
//    case 17:
//    case 18:
//        // DF 17/18: extended squitter
//        if (mm->metype == 28 && mm->mesub == 2 && memcmp(mm->ME, &a->fatsv_emitted_es_acas_ra, 7) != 0) {
//            // type 28 subtype 2: ACAS RA report
//            // first byte has the type/subtype, remaining bytes match the BDS 3,0 format
//            memcpy(a->fatsv_emitted_es_acas_ra, mm->ME, 7);
//            writeFATSVEventMessage(mm, "es_acas_ra", mm->ME, 7);
//        } else if (mm->metype == 31 && (mm->mesub == 0 || mm->mesub == 1) && memcmp(mm->ME, a->fatsv_emitted_es_status, 7) != 0) {
//            // aircraft operational status
//            memcpy(a->fatsv_emitted_es_status, mm->ME, 7);
//            writeFATSVEventMessage(mm, "es_op_status", mm->ME, 7);
//        } else if (mm->metype == 29 && (mm->mesub == 0 || mm->mesub == 1) && memcmp(mm->ME, a->fatsv_emitted_es_target, 7) != 0) {
//            // target state and status
//            memcpy(a->fatsv_emitted_es_target, mm->ME, 7);
//            writeFATSVEventMessage(mm, "es_target", mm->ME, 7);
//        }
//        break;
//    }
//}

typedef enum {
    TISB_IDENT = 1,
    TISB_SQUAWK = 2,
    TISB_ALTITUDE = 4,
    TISB_ALTITUDE_GNSS = 8,
    TISB_SPEED = 16,
    TISB_SPEED_IAS = 32,
    TISB_SPEED_TAS = 64,
    TISB_POSITION = 128,
    TISB_HEADING = 256,
    TISB_HEADING_MAGNETIC = 512,
    TISB_AIRGROUND = 1024,
    TISB_CATEGORY = 2048
} tisb_flags;

static inline unsigned unsigned_difference(unsigned v1, unsigned v2)
{
    return (v1 > v2) ? (v1 - v2) : (v2 - v1);
}

static inline unsigned heading_difference(unsigned h1, unsigned h2)
{
    unsigned d = unsigned_difference(h1, h2);
    return (d < 180) ? d : (360 - d);
}

//static void writeFATSV()
//{
//    struct aircraft *a;
//    uint64_t now;
//    static uint64_t next_update;
//
//    if (!DumpFLARM.fatsv_out.service || !DumpFLARM.fatsv_out.service->connections) {
//        return; // not enabled or no active connections
//    }
//
//    now = mstime();
//    if (now < next_update) {
//        return;
//    }
//
//    // scan once a second at most
//    next_update = now + 1000;
//
//    for (a = DumpFLARM.aircrafts; a; a = a->next) {
//        int altValid = 0;
//        int altGNSSValid = 0;
//        int positionValid = 0;
//        int speedValid = 0;
//        int speedIASValid = 0;
//        int speedTASValid = 0;
//        int headingValid = 0;
//        int headingMagValid = 0;
//        int airgroundValid = 0;
//        int categoryValid = 0;
//
//        uint64_t minAge;
//
//        int useful = 0;
//        int changed = 0;
//        tisb_flags tisb = 0;
//
//        char *p, *end;
//
//        if (a->messages < 2)  // basic filter for bad decodes
//            continue;
//
//        // don't emit if it hasn't updated since last time
//        if (a->seen < a->fatsv_last_emitted) {
//            continue;
//        }
//
//        altValid = trackDataValidEx(&a->altitude_valid, now, 15000, SOURCE_MODE_S); // for non-ADS-B transponders, DF0/4/16/20 are the only sources of altitude data
//        altGNSSValid = trackDataValidEx(&a->altitude_gnss_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        airgroundValid = trackDataValidEx(&a->airground_valid, now, 15000, SOURCE_MODE_S_CHECKED); // for non-ADS-B transponders, only trust DF11 CA field
//        positionValid = trackDataValidEx(&a->position_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        headingValid = trackDataValidEx(&a->heading_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        headingMagValid = trackDataValidEx(&a->heading_magnetic_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        speedValid = trackDataValidEx(&a->speed_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        speedIASValid = trackDataValidEx(&a->speed_ias_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        speedTASValid = trackDataValidEx(&a->speed_tas_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//        categoryValid = trackDataValidEx(&a->category_valid, now, 15000, SOURCE_MODE_S_CHECKED);
//
//        // If we are definitely on the ground, suppress any unreliable altitude info.
//        // When on the ground, ADS-B transponders don't emit an ADS-B message that includes
//        // altitude, so a corrupted Mode S altitude response from some other in-the-air AC
//        // might be taken as the "best available altitude" and produce e.g. "airGround G+ alt 31000".
//        if (airgroundValid && a->airground == AG_GROUND && a->altitude_valid.source < SOURCE_MODE_S_CHECKED)
//            altValid = 0;
//
//        // if it hasn't changed altitude, heading, or speed much,
//        // don't update so often
//        changed = 0;
//        if (altValid && abs(a->altitude - a->fatsv_emitted_altitude) >= 50) {
//            changed = 1;
//        }
//        if (altGNSSValid && abs(a->altitude_gnss - a->fatsv_emitted_altitude_gnss) >= 50) {
//            changed = 1;
//        }
//        if (headingValid && heading_difference(a->heading, a->fatsv_emitted_heading) >= 2) {
//            changed = 1;
//        }
//        if (headingMagValid && heading_difference(a->heading_magnetic, a->fatsv_emitted_heading_magnetic) >= 2) {
//            changed = 1;
//        }
//        if (speedValid && unsigned_difference(a->speed, a->fatsv_emitted_speed) >= 25) {
//            changed = 1;
//        }
//        if (speedIASValid && unsigned_difference(a->speed_ias, a->fatsv_emitted_speed_ias) >= 25) {
//            changed = 1;
//        }
//        if (speedTASValid && unsigned_difference(a->speed_tas, a->fatsv_emitted_speed_tas) >= 25) {
//            changed = 1;
//        }
//
//        if (airgroundValid && ((a->airground == AG_AIRBORNE && a->fatsv_emitted_airground == AG_GROUND) ||
//                               (a->airground == AG_GROUND && a->fatsv_emitted_airground == AG_AIRBORNE))) {
//            // Air-ground transition, handle it immediately.
//            minAge = 0;
//        } else if (!positionValid) {
//            // don't send mode S very often
//            minAge = 30000;
//        } else if ((airgroundValid && a->airground == AG_GROUND) ||
//                   (altValid && a->altitude < 500 && (!speedValid || a->speed < 200)) ||
//                   (speedValid && a->speed < 100 && (!altValid || a->altitude < 1000))) {
//            // we are probably on the ground, increase the update rate
//            minAge = 1000;
//        } else if (!altValid || a->altitude < 10000) {
//            // Below 10000 feet, emit up to every 5s when changing, 10s otherwise
//            minAge = (changed ? 5000 : 10000);
//        } else {
//            // Above 10000 feet, emit up to every 10s when changing, 30s otherwise
//            minAge = (changed ? 10000 : 30000);
//        }
//
//        if ((now - a->fatsv_last_emitted) < minAge)
//            continue;
//
//        p = prepareWrite(&DumpFLARM.fatsv_out, TSV_MAX_PACKET_SIZE);
//        if (!p)
//            return;
//
//        end = p + TSV_MAX_PACKET_SIZE;
//#       define bufsize(_p,_e) ((_p) >= (_e) ? (size_t)0 : (size_t)((_e) - (_p)))
//
//        p += snprintf(p, bufsize(p, end), "clock\t%" PRIu64, (uint64_t)(a->seen / 1000));
//
//        if (a->addr & MODES_NON_ICAO_ADDRESS) {
//            p += snprintf(p, bufsize(p, end), "\totherid\t%06X", a->addr & 0xFFFFFF);
//        } else {
//            p += snprintf(p, bufsize(p, end), "\thexid\t%06X", a->addr);
//        }
//
//        if (a->addrtype != ADDR_ADSB_ICAO) {
//            p += snprintf(p, bufsize(p, end), "\taddrtype\t%s", addrtype_short_string(a->addrtype));
//        }
//
//        if (trackDataValidEx(&a->callsign_valid, now, 15000, SOURCE_MODE_S_CHECKED) && strcmp(a->callsign, "        ") != 0 && a->callsign_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tident\t%s", a->callsign);
//            switch (a->callsign_valid.source) {
//            case SOURCE_MODE_S:
//                p += snprintf(p, bufsize(p,end), "\tiSource\tmodes");
//                break;
//            case SOURCE_ADSB:
//                p += snprintf(p, bufsize(p,end), "\tiSource\tadsb");
//                break;
//            case SOURCE_TISB:
//                p += snprintf(p, bufsize(p,end), "\tiSource\ttisb");
//                break;
//            default:
//                p += snprintf(p, bufsize(p,end), "\tiSource\tunknown");
//                break;
//            }
//
//            useful = 1;
//            tisb |= (a->callsign_valid.source == SOURCE_TISB) ? TISB_IDENT : 0;
//        }
//
//        if (trackDataValidEx(&a->squawk_valid, now, 15000, SOURCE_MODE_S) && a->squawk_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tsquawk\t%04x", a->squawk);
//            useful = 1;
//            tisb |= (a->squawk_valid.source == SOURCE_TISB) ? TISB_SQUAWK : 0;
//        }
//
//        // only emit alt, speed, latlon, track if they have been received since the last time
//        // and are not stale
//
//        if (altValid && a->altitude_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\talt\t%d", a->altitude);
//            a->fatsv_emitted_altitude = a->altitude;
//            useful = 1;
//            tisb |= (a->altitude_valid.source == SOURCE_TISB) ? TISB_ALTITUDE : 0;
//        }
//
//        if (altGNSSValid && a->altitude_gnss_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\talt_gnss\t%d", a->altitude_gnss);
//            a->fatsv_emitted_altitude_gnss = a->altitude_gnss;
//            useful = 1;
//            tisb |= (a->altitude_gnss_valid.source == SOURCE_TISB) ? TISB_ALTITUDE_GNSS : 0;
//        }
//
//        if (speedValid && a->speed_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tspeed\t%d", a->speed);
//            a->fatsv_emitted_speed = a->speed;
//            useful = 1;
//            tisb |= (a->speed_valid.source == SOURCE_TISB) ? TISB_SPEED : 0;
//        }
//
//        if (speedIASValid && a->speed_ias_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tspeed_ias\t%d", a->speed_ias);
//            a->fatsv_emitted_speed_ias = a->speed_ias;
//            useful = 1;
//            tisb |= (a->speed_ias_valid.source == SOURCE_TISB) ? TISB_SPEED_IAS : 0;
//        }
//
//        if (speedTASValid && a->speed_tas_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tspeed_tas\t%d", a->speed_tas);
//            a->fatsv_emitted_speed_tas = a->speed_tas;
//            useful = 1;
//            tisb |= (a->speed_tas_valid.source == SOURCE_TISB) ? TISB_SPEED_TAS : 0;
//        }
//
//        if (positionValid && a->position_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tlat\t%.5f\tlon\t%.5f", a->lat, a->lon);
//            useful = 1;
//            tisb |= (a->position_valid.source == SOURCE_TISB) ? TISB_POSITION : 0;
//        }
//
//        if (headingValid && a->heading_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\theading\t%d", a->heading);
//            a->fatsv_emitted_heading = a->heading;
//            useful = 1;
//            tisb |= (a->heading_valid.source == SOURCE_TISB) ? TISB_HEADING : 0;
//        }
//
//        if (headingMagValid && a->heading_magnetic_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\theading_magnetic\t%d", a->heading);
//            a->fatsv_emitted_heading_magnetic = a->heading_magnetic;
//            useful = 1;
//            tisb |= (a->heading_magnetic_valid.source == SOURCE_TISB) ? TISB_HEADING_MAGNETIC : 0;
//        }
//
//        if (airgroundValid && (a->airground == AG_GROUND || a->airground == AG_AIRBORNE) && a->airground_valid.updated > a->fatsv_last_emitted) {
//            p += snprintf(p, bufsize(p,end), "\tairGround\t%s", a->airground == AG_GROUND ? "G+" : "A+");
//            a->fatsv_emitted_airground = a->airground;
//            useful = 1;
//            tisb |= (a->airground_valid.source == SOURCE_TISB) ? TISB_AIRGROUND : 0;
//        }
//
//        if (categoryValid && (a->category & 0xF0) != 0xA0 && a->category_valid.updated > a->fatsv_last_emitted) {
//            // interesting category, not a regular aircraft
//            p += snprintf(p, bufsize(p,end), "\tcategory\t%02X", a->category);
//            useful = 1;
//            tisb |= (a->category_valid.source == SOURCE_TISB) ? TISB_CATEGORY : 0;
//        }
//
//        // if we didn't get anything interesting, bail out.
//        // We don't need to do anything special to unwind prepareWrite().
//        if (!useful) {
//            continue;
//        }
//
//        if (tisb != 0) {
//            p += snprintf(p, bufsize(p,end), "\ttisb\t%d", (int)tisb);
//        }
//
//        p += snprintf(p, bufsize(p,end), "\n");
//
//        if (p <= end)
//            completeWrite(&DumpFLARM.fatsv_out, p);
//        else
//            fprintf(stderr, "fatsv: output too large (max %d, overran by %d)\n", TSV_MAX_PACKET_SIZE, (int) (p - end));
//#       undef bufsize
//
//        a->fatsv_last_emitted = now;
//    }
//}

//
// Perform periodic network work
//
void modesNetPeriodicWork(void) {
    struct client *c, **prev;
    struct net_service *s;
    uint64_t now = mstime();
    int need_flush = 0;

    // Accept new connections
    modesAcceptClients();

    // Read from clients
    for (c = DumpFLARM.clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->service->read_handler)
            modesReadFromClient(c);
    }

    // Generate FATSV output
    //writeFATSV();

    // If we have generated no messages for a while, send
    // a heartbeat
    if (DumpFLARM.net_heartbeat_interval) {
        for (s = DumpFLARM.services; s; s = s->next) {
            if (s->writer &&
                s->connections &&
                s->writer->send_heartbeat &&
                (s->writer->lastWrite + DumpFLARM.net_heartbeat_interval) <= now) {
                s->writer->send_heartbeat(s);
            }
        }
    }

    // If we have data that has been waiting to be written for a while,
    // write it now.
    for (s = DumpFLARM.services; s; s = s->next) {
        if (s->writer &&
            s->writer->dataUsed &&
            (need_flush || (s->writer->lastWrite + DumpFLARM.net_output_flush_interval) <= now)) {
            flushWrites(s->writer);
        }
    }

    // Unlink and free closed clients
    for (prev = &DumpFLARM.clients, c = *prev; c; c = *prev) {
        if (c->fd == -1) {
            // Recently closed, prune from list
            *prev = c->next;
            free(c);
        } else {
            prev = &c->next;
        }
    }
}

//
// =============================== Network IO ===========================
//
