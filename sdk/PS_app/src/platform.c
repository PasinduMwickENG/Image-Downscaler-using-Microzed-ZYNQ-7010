/******************************************************************************
*
* Copyright (C) 2010 - 2015 Xilinx, Inc.  All rights reserved.
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
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

#include "xparameters.h"
#include "xil_cache.h"
#include "xil_exception.h"

#include "platform_config.h"

#ifdef PLATFORM_ZYNQ
#include "xscutimer.h"
#include "xscugic.h"
#include "xscugic_hw.h"
#include "xil_printf.h"
#endif

/*
 * Uncomment one of the following two lines, depending on the target,
 * if ps7/psu init source files are added in the source directory for
 * compiling example outside of SDK.
 */
/*#include "ps7_init.h"*/
/*#include "psu_init.h"*/

#ifdef STDOUT_IS_16550
 #include "xuartns550_l.h"

 #define UART_BAUD 9600
#endif

void
enable_caches()
{
#ifdef __PPC__
    Xil_ICacheEnableRegion(CACHEABLE_REGION_MASK);
    Xil_DCacheEnableRegion(CACHEABLE_REGION_MASK);
#elif __MICROBLAZE__
#ifdef XPAR_MICROBLAZE_USE_ICACHE
    Xil_ICacheEnable();
#endif
#ifdef XPAR_MICROBLAZE_USE_DCACHE
    Xil_DCacheEnable();
#endif
#endif
}

void
disable_caches()
{
#ifdef __MICROBLAZE__
#ifdef XPAR_MICROBLAZE_USE_DCACHE
    Xil_DCacheDisable();
#endif
#ifdef XPAR_MICROBLAZE_USE_ICACHE
    Xil_ICacheDisable();
#endif
#endif
}

void
init_uart()
{
#ifdef STDOUT_IS_16550
    XUartNs550_SetBaud(STDOUT_BASEADDR, XPAR_XUARTNS550_CLOCK_HZ, UART_BAUD);
    XUartNs550_SetLineControlReg(STDOUT_BASEADDR, XUN_LCR_8_DATA_BITS);
#endif
    /* Bootrom/BSP configures PS7/PSU UART to 115200 bps */
}

void
init_platform()
{
    /*
     * If you want to run this example outside of SDK,
     * uncomment one of the following two lines and also #include "ps7_init.h"
     * or #include "ps7_init.h" at the top, depending on the target.
     * Make sure that the ps7/psu_init.c and ps7/psu_init.h files are included
     * along with this example source files for compilation.
     */
    /* ps7_init();*/
    /* psu_init();*/
    enable_caches();
    init_uart();
}

void
cleanup_platform()
{
    disable_caches();
}

#ifdef PLATFORM_ZYNQ
/*
 * Standard Xilinx lwIP (raw mode) platform glue: a periodic SCU timer drives
 * the TCP timers (tcp_fasttmr / tcp_slowtmr) via flags, and the GIC is set up
 * so both the timer and (optionally) the EMAC can interrupt.
 */

static XScuTimer TimerInstance;

volatile int TcpFastTmrFlag = 0;
volatile int TcpSlowTmrFlag = 0;

static void timer_callback(XScuTimer * TimerInstance)
{
    /* Call tcp_fasttmr every ~250ms, tcp_slowtmr every ~500ms. Exact timing
     * is not critical. */
    static int odd = 1;
    TcpFastTmrFlag = 1;
    odd = !odd;
    if (odd) {
        TcpSlowTmrFlag = 1;
    }
    XScuTimer_ClearInterruptStatus(TimerInstance);
}

void platform_setup_timer(void)
{
    XScuTimer_Config *config = XScuTimer_LookupConfig(TIMER_DEVICE_ID);
    XScuTimer_CfgInitialize(&TimerInstance, config, config->BaseAddr);
    XScuTimer_SelfTest(&TimerInstance);
    XScuTimer_EnableAutoReload(&TimerInstance);
    /* ~250 ms period: timer clock is CPU_CLK/2, loaded value = cycles/4 */
    XScuTimer_LoadTimer(&TimerInstance, XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ / 8);
}

void platform_setup_interrupts(void)
{
    Xil_ExceptionInit();

    XScuGic_DeviceInitialize(INTC_DEVICE_ID);

    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
            (Xil_ExceptionHandler)XScuGic_DeviceInterruptHandler,
            (void *)INTC_DEVICE_ID);

    XScuGic_RegisterHandler(XPAR_SCUGIC_0_CPU_BASEADDR, TIMER_IRPT_INTR,
            (Xil_ExceptionHandler)timer_callback,
            (void *)&TimerInstance);

    XScuGic_EnableIntr(XPAR_SCUGIC_0_DIST_BASEADDR, TIMER_IRPT_INTR);
}

void platform_enable_interrupts(void)
{
    XScuTimer_EnableInterrupt(&TimerInstance);
    XScuTimer_Start(&TimerInstance);
    Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);
}
#endif
