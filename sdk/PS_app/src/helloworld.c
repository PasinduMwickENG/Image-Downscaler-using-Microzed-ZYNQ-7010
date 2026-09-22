/******************************************************************************
*
* Image downscale server - lwIP raw TCP edition.
*
* Protocol (over a TCP connection to the board on SERVER_PORT):
*   Host -> board : u32 width, u32 height, width*height*4 bytes of RGBA pixel data
*   Board -> host : u32 new_width, u32 new_height, new_width*new_height*4 bytes
* Multiple frames may be sent back-to-back on the same connection.
*
******************************************************************************/

#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "platform_config.h"
#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/ip_addr.h"
#include "netif/xadapter.h"

// ================== CONFIG ==================
#define DMA_DEV_ID              XPAR_AXIDMA_0_DEVICE_ID

// AXI4-Lite control interface of the image_downscale IP (runtime WIDTH /
// HEIGHT registers). Update this after wiring the S_AXI interface into the
// block design and assigning its address in the Vivado Address Editor.
#define IMAGE_DOWNSCALE_BASE_ADDR   0x43C00000
#define IMAGE_DOWNSCALE_REG_WIDTH   (IMAGE_DOWNSCALE_BASE_ADDR + 0x0)
#define IMAGE_DOWNSCALE_REG_HEIGHT  (IMAGE_DOWNSCALE_BASE_ADDR + 0x4)

#define SRC_ADDR            0x10000000
// The destination buffer is anchored at the top of DDR and grows down based
// on the output size, so any image size works as long as it fits in DDR.
#define DDR_TOP_ADDR        0x40000000
// Max bytes for a single image (256 MB). Must leave headroom below
// DDR_TOP_ADDR for the (smaller) output buffer as well.
#define MAX_IMG_BYTES       (256u * 1024u * 1024u)

#define PIXEL_SIZE          4

// The AXI DMA buffer-length register is XPAR_AXI_DMA_0_SG_LENGTH_WIDTH bits,
// so one SimpleTransfer is limited to (1<<23)-1 bytes. MM2S can be chunked
// freely; S2MM may NOT complete mid-stream (extra beats flag DMAIntErr and
// halt the channel), so a whole output must fit in one transfer.
#define DMA_CHUNK           0x400000u   // 4 MB per MM2S transfer
#define DMA_MAX_XFER        0x7FFFFFu   // 8 MB-1: max single DMA transfer

// TCP server settings. The board uses a static IP - configure the host PC's
// Ethernet adapter to the same subnet (e.g. 192.168.1.1).
#define SERVER_PORT         5001
#define BOARD_IP0           192
#define BOARD_IP1           168
#define BOARD_IP2           1
#define BOARD_IP3           10
#define BOARD_NETMASK0      255
#define BOARD_NETMASK1      255
#define BOARD_NETMASK2      255
#define BOARD_NETMASK3      0
#define BOARD_GW0           192
#define BOARD_GW1           168
#define BOARD_GW2           1
#define BOARD_GW3           1
// ============================================

XAxiDma AxiDma;
struct netif server_netif;

u8 *src_buf = (u8*)SRC_ADDR;
u8 *dst_buf = NULL;

// lwIP timer flags, set by the platform timer ISR in platform.c
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;

// -------------------- Connection state --------------------
static struct tcp_pcb *client_pcb = NULL;

enum { RX_HEADER, RX_IMAGE } rx_stage = RX_HEADER;
static u8  rx_hdr[8];
static u32 rx_have = 0;          // bytes received in current stage
static u32 rx_width = 0, rx_height = 0;
static u32 rx_total = 0;         // expected image payload bytes
static volatile int frame_ready = 0;

static u8  tx_hdr[8];
static u32 tx_off = 0;           // bytes sent of (8-byte header + payload)
static u32 tx_size = 0;          // payload bytes to send
static int tx_active = 0;
static u32 conn_idle = 0;        // ~2s poll ticks since last RX activity

// -------------------- TX helper --------------------
// Enqueue as much of the response as the TCP send buffer can take; the
// tcp_sent callback keeps draining it as ACKs arrive.
static void flush_send(void)
{
    u32 total = 8 + tx_size;
    while (tx_active && client_pcb && tx_off < total) {
        u32 space = tcp_sndbuf(client_pcb);
        if (space == 0)
            return;
        u32 chunk = total - tx_off;
        if (chunk > space)
            chunk = space;

        const u8 *src;
        if (tx_off < 8) {
            src = tx_hdr + tx_off;
            if (chunk > 8 - tx_off)
                chunk = 8 - tx_off;
        } else {
            src = dst_buf + (tx_off - 8);
        }

        err_t e = tcp_write(client_pcb, src, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK)
            return;
        tx_off += chunk;
        tcp_output(client_pcb);
    }
    if (tx_active && tx_off >= total)
        tx_active = 0;   // response fully queued
}

static err_t sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    flush_send();
    return ERR_OK;
}

static void reset_conn_state(void)
{
    rx_stage = RX_HEADER;
    rx_have = 0;
    frame_ready = 0;
    tx_active = 0;
    tx_off = 0;
}

static void conn_err_callback(void *arg, err_t err)
{
    // The pcb is already deallocated by lwIP - just drop our reference,
    // and only if it is the pcb we are actually tracking.
    if (arg == client_pcb) {
        client_pcb = NULL;
        reset_conn_state();
    }
}

// -------------------- RX state machine --------------------
static err_t recv_callback(void *arg, struct tcp_pcb *tpcb,
                           struct pbuf *p, err_t err)
{
    if (tpcb != client_pcb) {
        // Event from a connection we already dropped - ignore it so a
        // zombie pcb can't corrupt the active connection's state.
        if (p != NULL)
            pbuf_free(p);
        return ERR_OK;
    }
    if (p == NULL) {
        // Remote closed the connection.
        client_pcb = NULL;
        reset_conn_state();
        if (tcp_close(tpcb) != ERR_OK)
            tcp_abort(tpcb);
        return ERR_OK;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        u8 *data = (u8*)q->payload;
        u32 len = q->len;
        while (len) {
            if (rx_stage == RX_HEADER) {
                u32 n = 8 - rx_have;
                if (n > len) n = len;
                memcpy(rx_hdr + rx_have, data, n);
                rx_have += n; data += n; len -= n;
                if (rx_have == 8) {
                    rx_width  = rx_hdr[0] | (rx_hdr[1] << 8) |
                                ((u32)rx_hdr[2] << 16) | ((u32)rx_hdr[3] << 24);
                    rx_height = rx_hdr[4] | (rx_hdr[5] << 8) |
                                ((u32)rx_hdr[6] << 16) | ((u32)rx_hdr[7] << 24);
                    u64 sz = (u64)rx_width * rx_height * PIXEL_SIZE;
                    if (rx_width == 0 || rx_height == 0 ||
                        (rx_width & 1) || (rx_height & 1) ||
                        sz > MAX_IMG_BYTES) {
                        xil_printf("Bad header %ux%u, dropping\r\n",
                                   rx_width, rx_height);
                        rx_have = 0;   // stay in RX_HEADER, resync next bytes
                    } else {
                        rx_total = (u32)sz;
                        rx_stage = RX_IMAGE;
                        rx_have = 0;
                    }
                }
            } else { // RX_IMAGE
                u32 n = rx_total - rx_have;
                if (n > len) n = len;
                memcpy(src_buf + rx_have, data, n);
                rx_have += n; data += n; len -= n;
                if (rx_have == rx_total) {
                    frame_ready = 1;
                    rx_stage = RX_HEADER;
                    rx_have = 0;
                }
            }
        }
    }

    conn_idle = 0;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// Called by lwIP every ~2s while the connection exists. If the peer has
// gone silent for a long time (e.g. host vanished without closing), abort
// so the single-client gate can't wedge forever.
static err_t poll_callback(void *arg, struct tcp_pcb *tpcb)
{
    if (++conn_idle > 60) {
        if (tpcb == client_pcb)
            client_pcb = NULL;
        reset_conn_state();
        tcp_abort(tpcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (client_pcb != NULL) {
        // Only one client at a time - the shared buffers aren't reentrant.
        // tcp_abort (not tcp_close) frees the pcb immediately even if a
        // graceful close can't proceed; leaked pcbs eventually exhaust the
        // pool and new SYNs get silently dropped.
        tcp_abort(newpcb);
        return ERR_OK;
    }
    client_pcb = newpcb;
    conn_idle = 0;
    reset_conn_state();
    tcp_arg(newpcb, newpcb);
    tcp_recv(newpcb, recv_callback);
    tcp_sent(newpcb, sent_callback);
    tcp_err(newpcb, conn_err_callback);
    tcp_poll(newpcb, poll_callback, 4);   // 4 x 500ms slow-timer tick
    return ERR_OK;
}

static int start_tcp_server(void)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL)
        return -1;
    err_t err = tcp_bind(pcb, IP_ANY_TYPE, SERVER_PORT);
    if (err != ERR_OK)
        return -2;
    pcb = tcp_listen(pcb);
    if (pcb == NULL)
        return -3;
    tcp_accept(pcb, accept_callback);
    return 0;
}

// -------------------- Frame processing --------------------
static void process_frame(void)
{
    /* Recover both DMA channels: if a previous frame errored (DMAIntErr) the
     * channel stays halted until reset, and nothing else resets it. */
    XAxiDma_Reset(&AxiDma);
    {
        u32 t = 1000000;
        while (!XAxiDma_ResetIsDone(&AxiDma) && --t);
    }

    // Configure the IP for this frame's size (latched at frame start in PL).
    Xil_Out32(IMAGE_DOWNSCALE_REG_WIDTH, rx_width);
    Xil_Out32(IMAGE_DOWNSCALE_REG_HEIGHT, rx_height);

    u32 new_width  = rx_width  / 2;
    u32 new_height = rx_height / 2;
    tx_size = new_width * new_height * PIXEL_SIZE;
    dst_buf = (u8*)(DDR_TOP_ADDR - tx_size);

    Xil_DCacheFlushRange((UINTPTR)src_buf, rx_total);

    /* S2MM must be armed for the whole frame output in ONE transfer: if an
     * S2MM transfer completes while the stream still has data, the channel
     * flags DMAIntErr and halts. The 23-bit length field therefore limits
     * output to DMA_MAX_XFER (8MB-1) - i.e. ~32MB of input. MM2S can be
     * chunked freely; stream gaps between chunks are harmless. */
    if (tx_size > DMA_MAX_XFER) {
        xil_printf("Frame output too large for DMA (%u > %u)\r\n",
                   tx_size, (u32)DMA_MAX_XFER);
        return;
    }
    if (XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)dst_buf, tx_size,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        xil_printf("S2MM start failed\r\n");
        return;
    }

    u32 mm2s_off = 0;
    while (mm2s_off < rx_total) {
        u32 c = rx_total - mm2s_off;
        if (c > DMA_CHUNK) c = DMA_CHUNK;
        if (XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)(src_buf + mm2s_off), c,
                                   XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
            xil_printf("MM2S start failed\r\n");
            return;
        }
        mm2s_off += c;

        u32 timeout = 100000000;
        while (XAxiDma_Busy(&AxiDma, XAXIDMA_DMA_TO_DEVICE))
            if (--timeout == 0) {
                xil_printf("MM2S timeout\r\n");
                return;
            }
    }

    {
        u32 timeout = 100000000;
        while (XAxiDma_Busy(&AxiDma, XAXIDMA_DEVICE_TO_DMA))
            if (--timeout == 0) {
                xil_printf("S2MM timeout\r\n");
                return;
            }
    }

    Xil_DCacheInvalidateRange((UINTPTR)dst_buf, tx_size);

    // Build response header
    tx_hdr[0] = new_width & 0xFF;
    tx_hdr[1] = (new_width >> 8) & 0xFF;
    tx_hdr[2] = (new_width >> 16) & 0xFF;
    tx_hdr[3] = (new_width >> 24) & 0xFF;
    tx_hdr[4] = new_height & 0xFF;
    tx_hdr[5] = (new_height >> 8) & 0xFF;
    tx_hdr[6] = (new_height >> 16) & 0xFF;
    tx_hdr[7] = (new_height >> 24) & 0xFF;

    tx_off = 0;
    tx_active = 1;
    flush_send();
}

// -------------------- Main --------------------
int main()
{
    init_platform();

    // ---------- Initialize DMA ----------
    XAxiDma_Config *dma_cfg = XAxiDma_LookupConfig(DMA_DEV_ID);
    if (dma_cfg == NULL)
        return -1;
    int status = XAxiDma_CfgInitialize(&AxiDma, dma_cfg);
    if (status != XST_SUCCESS)
        return -1;
    if (XAxiDma_HasSg(&AxiDma))
        return -1;
    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

    // ---------- Initialize Ethernet / lwIP ----------
    ip_addr_t ipaddr, netmask, gw;
    unsigned char mac[] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

    IP4_ADDR(&ipaddr,  BOARD_IP0, BOARD_IP1, BOARD_IP2, BOARD_IP3);
    IP4_ADDR(&netmask, BOARD_NETMASK0, BOARD_NETMASK1, BOARD_NETMASK2, BOARD_NETMASK3);
    IP4_ADDR(&gw,      BOARD_GW0, BOARD_GW1, BOARD_GW2, BOARD_GW3);

    lwip_init();

    platform_setup_timer();
    platform_setup_interrupts();
    platform_enable_interrupts();

    if (!xemac_add(&server_netif, &ipaddr, &netmask, &gw,
                   mac, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding network interface\r\n");
        return -1;
    }
    netif_set_default(&server_netif);
    netif_set_up(&server_netif);

    if (start_tcp_server() != 0) {
        xil_printf("Failed to start TCP server\r\n");
        return -1;
    }

    xil_printf("Image downscale server listening on %d.%d.%d.%d:%d\r\n",
               BOARD_IP0, BOARD_IP1, BOARD_IP2, BOARD_IP3, SERVER_PORT);

    while (1) {
        if (TcpFastTmrFlag) {
            tcp_fasttmr();
            TcpFastTmrFlag = 0;
        }
        if (TcpSlowTmrFlag) {
            tcp_slowtmr();
            TcpSlowTmrFlag = 0;
        }

        xemacif_input(&server_netif);

        if (frame_ready && !tx_active) {
            frame_ready = 0;
            process_frame();
        }

        // Keep draining the response if there's still data queued.
        if (tx_active)
            flush_send();
    }

    cleanup_platform();
    return 0;
}
