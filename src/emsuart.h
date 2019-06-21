/*
 * emsuart.h
 * 
 * Header file for emsuart.cpp
 * 
 * Paul Derbyshire - https://github.com/proddy/EMS-ESP
 */
#pragma once

#include <Arduino.h>

#define EMSUART_UART 0      // UART 0
#define EMSUART_CONFIG 0x1C // 8N1 (8 bits, no stop bits, 1 parity)
#define EMSUART_BAUD 9600   // uart baud rate for the EMS circuit

#define EMS_MAXBUFFERS 10    // buffers for circular filling to avoid collisions
#define EMS_MAXBUFFERSIZE 32 // max size of the buffer. packets are max 32 bytes to support EMS 1.0

#define EMSUART_BIT_TIME 104 // bit time @9600 baud

#define EMSUART_recvTaskPrio 1
#define EMSUART_recvTaskQueueLen 64

typedef struct {
    uint8_t length;
    uint8_t buffer[EMS_MAXBUFFERSIZE];
} _EMSRxBuf;

void ICACHE_FLASH_ATTR emsuart_init();
void ICACHE_FLASH_ATTR emsuart_stop();
void ICACHE_FLASH_ATTR emsuart_start();
uint32_t ICACHE_FLASH_ATTR emsuart_tx_buffer(uint8_t * buf, uint8_t len);
void ICACHE_FLASH_ATTR emsuart_tx_poll();
