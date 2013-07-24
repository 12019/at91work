/* Licence: GPL
   (c) 2010 by Harald Welte <hwelte@hmw-consulting.de>
   (c) 2013 by Tom Schouten <tom@zwizwa.be>
*/

#include <stdint.h>
#include <string.h>
#include "errno.h"
#include <utility/trace.h>

/* LOW-LEVEL non-blocking I/O */

struct iso7816_port; // Abstract. implementation = platform-specific
struct iso7816_port *iso7816_port_init(int port_nb); // port_nb is board-specific
int iso7816_port_tx(struct iso7816_port *p, uint8_t c);
int iso7816_port_rx(struct iso7816_port *p, uint8_t *c);

void iso7816_port_get_rst_vcc(struct iso7816_port *p, int *rst, int *vcc);





/* BYTE-LEVEL PROTOCOL */

enum iso7816_ins {
    INS_DEACTIVATE_FILE        = 0x04,
    INS_ERASE_BINARY           = 0x0E,
    INS_TERMINAL_PROFILE       = 0x10,
    INS_FETCH                  = 0x12,
    INS_TERMINAL_RESPONSE      = 0x14,
    INS_VERIFY                 = 0x20,
    INS_CHANGE_PIN             = 0x24,
    INS_DISABLE_PIN            = 0x26,
    INS_ENABLE_PIN             = 0x28,
    INS_UNBLOCK_PIN            = 0x2C,
    INS_INCREASE               = 0x32,
    INS_ACTIVATE_FILE          = 0x44,
    INS_MANAGE_CHANNEL         = 0x70,
    INS_MANAGE_SECURE_CHANNEL  = 0x73,
    INS_TRANSACT_DATA       L  = 0x75,
    INS_EXTERNAL_AUTHOENTICATE = 0x82,
    INS_GET_CHALLENGE          = 0x84,
    INS_INTERNAL_AUTHENTICATE  = 0x88, // also 0x89 ??
    INS_SEARCH_RECORD          = 0xA2,
    INS_SELECT_FILE            = 0xA4,
    INS_TERMINAL_CAPABILITY    = 0xAA,
    INS_READ_BINARY            = 0xB0,
    INS_READ_RECORD            = 0xB2,
    INS_GET_RESPONSE           = 0xC0,   /* Transmission-oriented APDU */
    INS_ENVELOPE               = 0xC2,
    INS_RETRIEVE_DATA          = 0xCB,
    INS_GET_DATA               = 0xCA,
    INS_WRITE_BINARY           = 0xD0,
    INS_WRITE_RECORD           = 0xD2,
    INS_UPDATE_BINARY          = 0xD6,
    INS_PUT_DATA               = 0xDA,
    INS_SET_DATA               = 0xDB,
    INS_UPDATE_DATA            = 0xDC,
    INS_APPEND_RECORD          = 0xE2,
    INS_STATUS                 = 0xF2,
};

enum iso7816_state {
    S_INIT = 0, // wait for RST low
    S_RESET,    // wait for RST high and start sending ATR
    S_ATR,      // ATR is out, start waiting for PTS (FIXME: hardcoded to 3 bytes)

    S_PTS,      // PTS is in, start sending PTS ack
    S_PTS_ACK,  // PTS ack is out, start listening for TPDU header.

    S_TPDU,
    S_TPDU_PROT,
    S_TPDU_REQ,
    S_TPDU_SW,
    S_TPDU_RESP,

    S_HALT,     // scaffolding: break loop

    /* RX/TX: io_next contains next state after I/O is done */
    S_RX,
    S_TX,

};

struct tpdu {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t p3;
    uint8_t data[];
} __attribute__((__packed__));

struct sw {
    uint8_t sw1;
    uint8_t sw2;
} __attribute__((__packed__));


struct iso7816_slave {
    struct iso7816_port *port;
    enum iso7816_state state;
    enum iso7816_state io_next; // next state after I/O is done
    uint8_t *io_ptr;        // current read(RX) or write(TX) index
    uint8_t *io_endx;       // current(TX) or expected(RX) size of message
    union {
        struct tpdu tpdu;
        uint8_t buf[8 + 256];   // receive/send buffer
    } msg;
    struct sw sw; // response code
};


/* Start S_RX/S_TX transfer and continue at io_next */
static void next_io_start(struct iso7816_slave *s,
                          enum iso7816_state transfer_state,
                          enum iso7816_state io_next,
                          uint8_t *buf, int size) {
    if (!size) {
        s->state = io_next;
    }
    else {
        s->state = transfer_state;
        s->io_next = io_next;
        s->io_ptr = buf ? buf : s->msg.buf;
        s->io_endx = s->io_ptr + size;
    }
}
static void next_receive(struct iso7816_slave *s,
                         enum iso7816_state io_next,
                         uint8_t *buf,
                         int size) {
    next_io_start(s, S_RX, io_next, buf, size);
}
static void next_send(struct iso7816_slave *s,
                      enum iso7816_state io_next,
                      const uint8_t *buf,
                      int size) {
    next_io_start(s, S_TX, io_next, (uint8_t*)buf, size);
}

/* Continue or transfer and jump to io_next when done. */
static void next_io(struct iso7816_slave *s, int rv) {
    switch (rv) {
    case -EAGAIN:
        break;
    case ENOERR:
        s->io_ptr++;
        if (s->io_ptr >= s->io_endx) {
            s->state = s->io_next;
        }
        break;
    default:
        TRACE_DEBUG("i/o error %d\n\r", rv);
        s->state = S_RESET;
        break;
    }
}

static void next_receive_tpdu_header(struct iso7816_slave *s) {
    memset(s->msg.buf, 0x55, sizeof(s->msg.buf));  // simplify debugging
    next_receive(s, S_TPDU, s->msg.buf, sizeof(s->msg.tpdu));
}


static void print_apdu(struct iso7816_slave *s) {
    printf("APDU:");
    int i, len = sizeof(s->msg.tpdu) + s->msg.tpdu.p3;
    for (i = 0; i < len; i++) printf(" %02X", s->msg.buf[i]);
    printf(" %02X %02X\n\r", s->sw.sw1, s->sw.sw2);
}
static void apdu_request(struct iso7816_slave *s) {
    switch(s->msg.tpdu.ins) {
    case INS_SELECT_FILE:
        TRACE_DEBUG("SELECT_FILE %02x%02x\n\r",
                    s->msg.tpdu.data[0],
                    s->msg.tpdu.data[1]);
        s->sw.sw1 = 0x9F;
        s->sw.sw2 = 0x1A;
        break;
    case INS_GET_RESPONSE:
        TRACE_DEBUG("GET_RESPONSE %d\n\r", s->msg.tpdu.p3);
        memset(s->msg.tpdu.data, 0x55, s->msg.tpdu.p3);
        s->sw.sw1 = 0x90;
        s->sw.sw2 = 0x00;
        break;
    default:
        TRACE_DEBUG("bad APDU\n");
        break;
    }

    print_apdu(s);
}



/* Non-blocking state machine for ISO7816, slave side. */


int iso7816_slave_tick(struct iso7816_slave *s) {

    int rst, vcc;
    iso7816_port_get_rst_vcc(s->port, &rst, &vcc);

    /* Monitor RST and VCC */
    if (!(rst && vcc)) {
        if (s->state != S_RESET) {
            TRACE_DEBUG("%d -> S_RESET\n\r", s->state)
            s->state = S_RESET;
        }
    }


    switch(s->state) {
        /* PROTOCOL:
           These states are "high level" states executed in sequence. */

        /**** STARTUP ****/
    case S_INIT:
        /* Don't talk to iso7816 master in unknown state. */
        break;
    case S_RESET: {
        /* Boot sync: wait for RST release. */
        if (rst && vcc) {
            TRACE_DEBUG("S_RESET -> S_ATR\n\r");
            static const uint8_t atr[] =
                {0x3B, 0x98, 0x96, 0x00, 0x93, 0x94,
                 0x03, 0x08, 0x05, 0x03, 0x03, 0x03};
            next_send(s, S_ATR, atr, sizeof(atr));
        }
        break;
    }
    case S_ATR:
        /* ATR is out, wait for Protocol Type Selection (PTS) */
        TRACE_DEBUG("S_ATR\n\r");
        next_receive(s, S_PTS, NULL, 3);  // FIXME: hardcoded to 3 bytes from BTU phone
        break;
    case S_PTS:
        /* PTS is in, send PTS ack */
        TRACE_DEBUG("S_PTS %02x %02x %02x\n\r",
                    s->msg.buf[0], s->msg.buf[1], s->msg.buf[2]);
        next_send(s, S_PTS_ACK, NULL, 3);
        break;
    case S_PTS_ACK:
        /* PTS ack is out, Start waiting for TPDU header */
        TRACE_DEBUG("S_PTS_ACK\n\r");
        next_receive_tpdu_header(s);
        break;


        /**** MAIN LOOP ****/

    case S_TPDU:
        /* TPDU header is in, send protocol byte. */
        TRACE_DEBUG("S_TPDU\n\r");
        next_send(s, S_TPDU_PROT, &s->msg.tpdu.ins, 1);
        break;
    case S_TPDU_PROT:
        /* Protocol byte is out.  Based on INS, we either read payload
           from master and then perform high level APDU request, or we
           perform request now and send the resulting bytes to
           master. */
        TRACE_DEBUG("S_TPDU_PROT\n\r");
        switch(s->msg.tpdu.ins) {
        case INS_SELECT_FILE:
            /* Read data from master, then delegate request. */
            next_receive(s, S_TPDU_REQ, &s->msg.tpdu.data[0], s->msg.tpdu.p3);
            break;
        case INS_GET_RESPONSE:
            /* Delegate request, then send data to master. */
            apdu_request(s);
            next_send(s, S_TPDU_SW, &s->msg.tpdu.data[0], s->msg.tpdu.p3);
            break;
        default:
            TRACE_DEBUG("INS %02X not supported\n\r", s->msg.tpdu.ins);
            TRACE_DEBUG("TPDU: %02X %02X %02X %02X %02X\n\r",
                        s->msg.buf[0], s->msg.buf[1], s->msg.buf[2],
                        s->msg.buf[3], s->msg.buf[4]);
            s->state = S_INIT;
            break;
        }
        break;
    case S_TPDU_REQ:
        TRACE_DEBUG("S_TPDU_REQ\n\r");
        apdu_request(s);
        // fallthrough
    case S_TPDU_SW:
        /* TPDU data is transferred.  Send SW1:SW2. */
        TRACE_DEBUG("S_TPDU_SW\n\r");
        next_send(s, S_TPDU_RESP, &s->sw.sw1, sizeof(s->sw));
        break;
    case S_TPDU_RESP:
        /* Response sent, wait for next TPDU */
        TRACE_DEBUG("S_TPDU_RESP\n\r");
        next_receive_tpdu_header(s);
        break;


        /**** LOW LEVEL ****/

        /* Message transfer */
    case S_RX: next_io(s, iso7816_port_rx(s->port,  s->io_ptr));    break;
    case S_TX: next_io(s, iso7816_port_tx(s->port,  s->io_ptr[0])); break;

    default:
        TRACE_DEBUG("unknown state %d\n\r", s->state);
        s->state = S_INIT;
        break;
        /* Scaffolding: end loop */
    case S_HALT:
        return ENOERR;
    }
    return -EAGAIN; // state != S_HALT
}

// FIXME: scaffolding: run state machine
int iso7816_slave_run(struct iso7816_slave *s) {
    int rv;
    while (-EAGAIN == (rv = iso7816_slave_tick(s)));
    return rv;
}


void iso7816_slave_mainloop(struct iso7816_slave *s) {
    s->state = S_INIT;
    iso7816_slave_run(s);
}



/* BOARD-SPECIFIC: SIMTRACE */

static struct iso7816_slave phone;

struct iso7816_slave *iso7816_slave_init(void) {
    struct iso7816_slave *p = &phone;
    p->port = iso7816_port_init(1);
    return p;
}

