/******************************************************************************
 * netif.h
 * 
 * Unified network-device I/O interface for Xen guest OSes.
 * 
 * Copyright (c) 2003-2004, Keir Fraser
 */

#ifndef __SHARED_NETIF_H__
#define __SHARED_NETIF_H__

typedef unsigned long netif_addr_t;

typedef struct {
    netif_addr_t addr;   /* Machine address of packet.  */
    u16          id;     /* Echoed in response message. */
    u16          size;   /* Packet size in bytes.       */
} netif_tx_request_t;

typedef struct {
    u16          id;
    s8           status;
} netif_tx_response_t;

typedef struct {
    u16          id;     /* Echoed in response message.        */
} netif_rx_request_t;

typedef struct {
    netif_addr_t addr;   /* Machine address of packet.              */
    u16          id;
    s16          status; /* -ve: BLKIF_RSP_* ; +ve: Rx'ed pkt size. */
} netif_rx_response_t;

/*
 * We use a special capitalised type name because it is _essential_ that all 
 * arithmetic on indexes is done on an integer type of the correct size.
 */
typedef unsigned int NETIF_RING_IDX;

/*
 * Ring indexes are 'free running'. That is, they are not stored modulo the
 * size of the ring buffer. The following macros convert a free-running counter
 * into a value that can directly index a ring-buffer array.
 */
#define MASK_NETIF_RX_IDX(_i) ((_i)&(NETIF_RX_RING_SIZE-1))
#define MASK_NETIF_TX_IDX(_i) ((_i)&(NETIF_TX_RING_SIZE-1))

#define NETIF_TX_RING_SIZE 256
#define NETIF_RX_RING_SIZE 256

/* This structure must fit in a memory page. */
typedef struct {
    union {
        netif_tx_request_t  req;
        netif_tx_response_t resp;
    } ring[NETIF_TX_RING_SIZE];
    /*
     * Frontend places packets into ring at tx_req_prod.
     * Frontend receives event when tx_resp_prod passes tx_event.
     */
    NETIF_RING_IDX req_prod, resp_prod, event;
} netif_tx_interface_t;

/* This structure must fit in a memory page. */
typedef struct {
    union {
        netif_rx_request_t  req;
        netif_rx_response_t resp;
    } ring[NETIF_RX_RING_SIZE];
    /*
     * Frontend places empty buffers into ring at rx_req_prod.
     * Frontend receives event when rx_resp_prod passes rx_event.
     */
    NETIF_RING_IDX req_prod, resp_prod, event;
} netif_rx_interface_t;

/* Descriptor status values */
#define NETIF_RSP_DROPPED         -2
#define NETIF_RSP_ERROR           -1
#define NETIF_RSP_OKAY             0

#endif
