
#ifndef ISOTP_H
#define ISOTP_H

#include <stdint.h>
#include <stdbool.h>

#define ISOTP_MAX_PAYLOAD 1024U
#define ISOTP_CAN_DLEN     8U

typedef struct
{
    uint32_t id;
    uint8_t data[ISOTP_CAN_DLEN];
    uint8_t dlc;
} IsoTpCanFrame;

/* Callback functions */
typedef bool (*IsoTpSendFrame)(
    void *user,
    const IsoTpCanFrame *frame
);

typedef uint32_t (*IsoTpGetTimeUs)(void *user);

typedef void (*IsoTpReceiveMessage)(
    void *user,
    const uint8_t *data,
    uint16_t length
);

/* ISO-TP configuration */
typedef struct
{
    uint32_t tx_id;
    uint32_t rx_id;

    uint8_t wft_max;       /* Maximum FC WAIT frames accepted */
    uint8_t block_size;    /* Reserved: local TX block limit */
    uint8_t rx_block_size; /* CF frames accepted before sending FC */
    uint8_t st_min;        /* Receiver-advertised STmin */

    uint16_t n_bs_timeout_ms;
    uint16_t n_cr_timeout_ms;

    IsoTpSendFrame send_frame;
    IsoTpGetTimeUs get_time_us;
    IsoTpReceiveMessage receive_message;

    void *user;

} IsoTpConfig;

/* Transmission states */
typedef enum
{
    ISOTP_TX_IDLE,
    ISOTP_TX_WAIT_FC,
    ISOTP_TX_SENDING_CF
} IsoTpTxState;

/* Reception states */
typedef enum
{
    ISOTP_RX_IDLE,
    ISOTP_RX_RECEIVING
} IsoTpRxState;

/* Independent ISO-TP instance */
typedef struct
{
    IsoTpConfig config;

    /* Transmission data */
    uint8_t tx_buffer[ISOTP_MAX_PAYLOAD];
    uint16_t tx_length;
    uint16_t tx_offset;
    uint8_t tx_sequence_number;
    uint8_t tx_block_size;
    uint8_t tx_block_count;
    uint8_t tx_wait_count;
    uint8_t tx_st_min;
    uint32_t tx_next_time;
    uint32_t tx_deadline;
    IsoTpTxState tx_state;

    /* Reception data */
    uint8_t rx_buffer[ISOTP_MAX_PAYLOAD];
    uint16_t rx_length;
    uint16_t rx_offset;
    uint8_t rx_sequence_number;
    uint8_t rx_block_size;
    uint8_t rx_block_count;
    uint32_t rx_deadline;
    IsoTpRxState rx_state;

    bool initialized;

} IsoTpContext;

/* Public API */
bool isotp_init(
    IsoTpContext *context,
    const IsoTpConfig *config
);

bool isotp_send(
    IsoTpContext *context,
    const uint8_t *data,
    uint16_t length
);

void isotp_on_can_frame(
    IsoTpContext *context,
    const IsoTpCanFrame *frame
);

void isotp_poll(IsoTpContext *context);

#endif
