/*
 * emsuart.cpp
 *
 * The low level UART code for ESP8266 to read and write to the EMS bus via uart
 * Paul Derbyshire - https://github.com/proddy/EMS-ESP
 */

#include "emsuart.h"
#include "ems.h"
#include <Arduino.h>
#include <user_interface.h>

_EMSRxBuf * pEMSRxBuf;
_EMSRxBuf * paEMSRxBuf[EMS_MAXBUFFERS];
uint8_t     emsRxBufIdx = 0;

os_event_t recvTaskQueue[EMSUART_recvTaskQueueLen]; // our Rx queue

//
// Main interrupt handler
// Important: do not use ICACHE_FLASH_ATTR !
//
static void emsuart_rx_intr_handler(void * para) {
    static uint8_t length;
    static uint8_t uart_buffer[EMS_MAXBUFFERSIZE];

    // is a new buffer? if so init the thing for a new telegram
    if (EMS_Sys_Status.emsRxStatus == EMS_RX_STATUS_IDLE) {
        EMS_Sys_Status.emsRxStatus = EMS_RX_STATUS_BUSY; // status set to busy
        length                     = 0;
    }

    // fill IRQ buffer, by emptying Rx FIFO
    if (USIS(EMSUART_UART) & ((1 << UIFF) | (1 << UITO) | (1 << UIBD))) {
        while ((USS(EMSUART_UART) >> USRXC) & 0xFF) {
            uart_buffer[length++] = USF(EMSUART_UART);
        }

        // clear Rx FIFO full and Rx FIFO timeout interrupts
        USIC(EMSUART_UART) = (1 << UIFF) | (1 << UITO);
    }

    // BREAK detection = End of EMS data block
    if (USIS(EMSUART_UART) & ((1 << UIBD))) {
        ETS_UART_INTR_DISABLE(); // disable all interrupts and clear them

        USIC(EMSUART_UART) = (1 << UIBD); // INT clear the BREAK detect interrupt

        pEMSRxBuf->length = length;
        os_memcpy((void *)pEMSRxBuf->buffer, (void *)&uart_buffer, length); // copy data into transfer buffer, including the BRK 0x00 at the end
        EMS_Sys_Status.emsRxStatus = EMS_RX_STATUS_IDLE;                    // set the status flag stating BRK has been received and we can start a new package

        system_os_post(EMSUART_recvTaskPrio, 0, 0); // call emsuart_recvTask() at next opportunity

        ETS_UART_INTR_ENABLE(); // re-enable UART interrupts
    }
}

/*
 * system task triggered on BRK interrupt
 * incoming received messages are always asynchronous
 * The full buffer is sent to the ems_parseTelegram() function in ems.cpp.
 */
static void ICACHE_FLASH_ATTR emsuart_recvTask(os_event_t * events) {
    _EMSRxBuf * pCurrent = pEMSRxBuf;
    uint8_t     length   = pCurrent->length; // number of bytes including the BRK at the end

    // validate and transmit the EMS buffer, excluding the BRK
    if (length == 2) {
        // it's a poll or status code, single byte
        ems_parseTelegram((uint8_t *)pCurrent->buffer, 1);
    } else if ((length > 4) && (pCurrent->buffer[length - 2] != 0x00)) {
        // ignore double BRK at the end, possibly from the Tx loopback
        // also telegrams with no data value
        ems_parseTelegram((uint8_t *)pCurrent->buffer, length - 1); // transmit EMS buffer, excluding the BRK
    }

    memset(pCurrent->buffer, 0x00, EMS_MAXBUFFERSIZE); // wipe memory just to be safe

    pEMSRxBuf = paEMSRxBuf[++emsRxBufIdx % EMS_MAXBUFFERS]; // next free EMS Receive buffer
}

/*
 * toggle UART_CONF0 with given bitmask
 */
static inline void ICACHE_FLASH_ATTR emsuart_toggle_conf0(uint32_t mask) {
    USC0(EMSUART_UART) |= (mask);   // set bits
    USC0(EMSUART_UART) &= ~(mask);  // clear bits
}

/*
 * init UART0 driver
 */
void ICACHE_FLASH_ATTR emsuart_init() {
    ETS_UART_INTR_DISABLE();
    ETS_UART_INTR_ATTACH(NULL, NULL);

    // allocate and preset EMS Receive buffers
    for (int i = 0; i < EMS_MAXBUFFERS; i++) {
        _EMSRxBuf * p = (_EMSRxBuf *)malloc(sizeof(_EMSRxBuf));
        paEMSRxBuf[i] = p;
    }
    pEMSRxBuf = paEMSRxBuf[0]; // preset EMS Rx Buffer

    // pin settings
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0RXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD);

    // set 9600, 8 bits, no parity check, 1 stop bit
    USD(EMSUART_UART)  = (UART_CLK_FREQ / EMSUART_BAUD);
    USC0(EMSUART_UART) = EMSUART_CONFIG; // 8N1
    emsuart_toggle_conf0(((1 << UCRXRST) | (1 << UCTXRST)));    //flush FIFOs

    // conf1 params
    // UCTOE = RX TimeOut enable (default is 1)
    // UCTOT = RX TimeOut Threshold (7 bit) = want this when no more data after 2 characters (default is 2)
    // UCFFT = RX FIFO Full Threshold (7 bit) = want this to be 31 for 32 bytes of buffer (default was 127)
    // see https://www.espressif.com/sites/default/files/documentation/esp8266-technical_reference_en.pdf
    USC1(EMSUART_UART) = 0;                                                                   // reset config first
    USC1(EMSUART_UART) = (EMS_MAX_TELEGRAM_LENGTH << UCFFT) | (0x02 << UCTOT) | (1 << UCTOE); // enable interupts

    // set interrupts for triggers
    USIC(EMSUART_UART) = 0xFFFF; // clear all interupts
    USIE(EMSUART_UART) = 0;      // disable all interrupts

    // enable rx break, fifo full and timeout.
    // but not frame error UIFR (because they are too frequent) or overflow UIOF because our buffer is only max 32 bytes
    USIE(EMSUART_UART) = (1 << UIBD) | (1 << UIFF) | (1 << UITO);

    // set up interrupt callbacks for Rx
    system_os_task(emsuart_recvTask, EMSUART_recvTaskPrio, recvTaskQueue, EMSUART_recvTaskQueueLen);

    // disable esp debug which will go to Tx and mess up the line - see https://github.com/espruino/Espruino/issues/655
    system_set_os_print(0);

    // swap Rx and Tx pins to use GPIO13 (D7) and GPIO15 (D8) respectively
    system_uart_swap();

    ETS_UART_INTR_ATTACH(emsuart_rx_intr_handler, NULL);
    ETS_UART_INTR_ENABLE();
}

/*
 * stop UART0 driver
 * This is called prior to an OTA upload and also before a save to SPIFFS to prevent conflicts
 */
void ICACHE_FLASH_ATTR emsuart_stop() {
    ETS_UART_INTR_DISABLE();
}

/*
 * re-start UART0 driver
 */
void ICACHE_FLASH_ATTR emsuart_start() {
    ETS_UART_INTR_ENABLE();
}

/*
 * set loopback mode and clear Tx/Rx FIFO
 */
static inline void ICACHE_FLASH_ATTR emsuart_loopback(bool enable) {
    if (enable)
        USC0(EMSUART_UART) |= (1 << UCLBE); // enable loopback
    else
        USC0(EMSUART_UART) &= ~(1 << UCLBE); // disable loopback
}

/*
 * Send to Tx, ending with a <BRK>
 */
uint32_t ICACHE_FLASH_ATTR emsuart_tx_buffer(uint8_t * buf, uint8_t len) {
    uint16_t wdc = EMS_TX_TELEGRAM_TO_COUNT;   // watchdog loop count
    uint32_t result = EMS_TX_TELEGRAM_SUCCESS;

    if (len > 0) {
    /*
     * based on code from https://github.com/proddy/EMS-ESP/issues/103 by @susisstrolch
     * we emit the whole telegram, with Rx interrupt disabled, collecting busmaster response in FIFO.
     * after sending the last char we poll the Rx status until either
     * - size(Rx FIFO) == size(Tx-Telegram)
     * - <BRK> is detected
     * At end of receive we re-enable Rx-INT and send a Tx-BRK in loopback mode.
     * 
     * ° For monitoring BRK we must use UART_INT_RAW
     * 
     */
    ETS_UART_INTR_DISABLE();    // disable all UART interrupt
    USIC(EMSUART_UART) = 0xFF;  // clear all UART interrupts
    emsuart_toggle_conf0(((1 << UCRXRST) | (1 << UCTXRST)));    //flush FIFOs

    // throw out the telegram...
    for (uint8_t i = 0; i < len;) {
        wdc = EMS_TX_TELEGRAM_TO_COUNT;

        volatile uint8_t _usrxc = (USS(EMSUART_UART) >> USRXC) & 0xFF;
        USF(EMSUART_UART) = buf[i++]; // send each Tx byte

        // wait for echo from busmaster, BRK or watchdog timeout
        while (((USS(EMSUART_UART) >> USRXC) & 0xFF) == _usrxc) {
            delayMicroseconds(EMSUART_BIT_TIME); // burn CPU cycles...
            if (--wdc == 0) {
                result = i << 8 | EMS_TX_TELEGRAM_TIMEOUT;
                break;
            }
            if (USIR(EMSUART_UART) & (1 << UIBD)) {
                USIC(EMSUART_UART) = (1 << UIBD); // clear BRK detect IRQ
                result = wdc << 16 | i << 8 | EMS_TX_TELEGRAM_BRK_RCV;
                break;
            }
        }
        if (result != EMS_TX_TELEGRAM_SUCCESS)
            break;
    }

    // we got the whole telegram in the Rx buffer
    // on Rx-BRK (bus collision) or wdc timeout we simply enable Rx and leave.
    // otherwise we send the final Tx-BRK in the loopback and re=enable Rx-INT.
    // worst case, we'll see an additional Rx-BRK...
    if (result == EMS_TX_TELEGRAM_SUCCESS) {
        // neither bus collision nor timeout - send terminating BRK signal
        emsuart_loopback(true);
        USC0(EMSUART_UART) |= (1 << UCBRK); // set <BRK>

        // wait until BRK detected...
        while (!(USIR(EMSUART_UART) & (1 << UIBD))) {
            delayMicroseconds(EMSUART_BIT_TIME);
        }

        USC0(EMSUART_UART) &= ~(1 << UCBRK); // clear <BRK>
        emsuart_loopback(false);          // disable loopback mode
//        USIC(EMSUART_UART) = (1 << UIBD); // clear BRK detect IRQ
    }
    ETS_UART_INTR_ENABLE(); // receive anything from FIFO...
    }
    return result;
}

/*
 * Send the Poll (our own ID) to Tx as a single byte and end with a <BRK>
 */
void ICACHE_FLASH_ATTR emsuart_tx_poll() {
    static uint8_t buf[1];
    if (EMS_Sys_Status.emsReverse) {
        buf[0] = {EMS_ID_ME | 0x80};
    } else {
        buf[0] = {EMS_ID_ME};
    }
    emsuart_tx_buffer(buf, 1);
}
