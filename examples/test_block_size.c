
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "isotp.h"

#define TEST_LENGTH 1024U
#define QUEUE_SIZE 32U
#define MAX_CF_TIMESTAMPS 200U

#define SIMULATION_STEP_US 100U
#define MAX_TRANSFER_LOOPS 20000U

static uint32_t simulated_time_us = 0U;

typedef struct
{
    IsoTpCanFrame frame;
    IsoTpContext *destination;
} QueuedFrame;

typedef struct
{
    QueuedFrame frames[QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} SimulatedCanBus;

typedef struct
{
    uint8_t data[TEST_LENGTH];
    uint16_t length;
    bool received;
} ReceiveResult;

typedef struct
{
    uint32_t timestamps[MAX_CF_TIMESTAMPS];
    uint16_t count;
} CfTiming;

typedef struct
{
    uint16_t cts_count;
} FlowControlStats;

static FlowControlStats forward_fc_stats = {0};
static FlowControlStats reverse_fc_stats = {0};

typedef struct
{
    SimulatedCanBus *bus;
    IsoTpContext *peer;
    uint32_t tx_id;
    uint32_t *simulated_time;
    ReceiveResult *receive_result;
} SimulatedCan;

static ReceiveResult sender_received = {0};
static ReceiveResult receiver_received = {0};

static CfTiming forward_timing = {0};
static CfTiming reverse_timing = {0};


/* Return simulated time in microseconds. */
static uint32_t get_time_us(void *user)
{
    SimulatedCan *endpoint = (SimulatedCan *)user;

    if (endpoint == NULL ||
        endpoint->simulated_time == NULL)
    {
        return 0U;
    }

    return *(endpoint->simulated_time);
}


/* Simulated CAN transmit callback. */
static bool can_send(void *user, const IsoTpCanFrame *frame)
{
    SimulatedCan *endpoint = (SimulatedCan *)user;
    SimulatedCanBus *bus;
    CfTiming *timing = NULL;
    uint8_t index;

    if (endpoint == NULL ||
        frame == NULL ||
        endpoint->bus == NULL ||
        endpoint->simulated_time == NULL ||
        endpoint->peer == NULL)
    {
        return false;
    }

    if (frame->id != endpoint->tx_id)
    {
        return false;
    }

    bus = endpoint->bus;

    if (bus->count >= QUEUE_SIZE)
    {
        return false;
    }
    /* Count Flow Control frames (PCI type 3, CTS status 0). */
if (frame->dlc >= 3U &&
    (frame->data[0] >> 4) == 3U &&
    (frame->data[0] & 0x0FU) == 0U)
{
    if (endpoint->tx_id == 0x7E8U)
{
    forward_fc_stats.cts_count++;
}
else if (endpoint->tx_id == 0x7E0U)
{
    reverse_fc_stats.cts_count++;
}
}
    /* Record Consecutive Frame transmission timestamps. */
    if (frame->dlc == 8U &&
        (frame->data[0] >> 4) == 2U)
    {
        if (endpoint->tx_id == 0x7E0U)
        {
            timing = &forward_timing;
        }
        else if (endpoint->tx_id == 0x7E8U)
        {
            timing = &reverse_timing;
        }

        if (timing != NULL &&
            timing->count < MAX_CF_TIMESTAMPS)
        {
            timing->timestamps[timing->count] =
                *(endpoint->simulated_time);

            timing->count++;
        }
    }

    index = bus->tail;

    bus->frames[index].frame = *frame;
    bus->frames[index].destination = endpoint->peer;

    bus->tail = (uint8_t)((bus->tail + 1U) % QUEUE_SIZE);
    bus->count++;

    return true;
}


/* Deliver one queued CAN frame. */
static void deliver_one_frame(SimulatedCanBus *bus)
{
    QueuedFrame *queued;

    if (bus == NULL || bus->count == 0U)
    {
        return;
    }

    queued = &bus->frames[bus->head];

    isotp_on_can_frame(
        queued->destination,
        &queued->frame
    );

    bus->head = (uint8_t)((bus->head + 1U) % QUEUE_SIZE);
    bus->count--;
}


/* Receive completed ISO-TP message. */
static void on_message(
    void *user,
    const uint8_t *data,
    uint16_t length)
{
    SimulatedCan *endpoint = (SimulatedCan *)user;
    ReceiveResult *result;
    uint16_t i;

    if (endpoint == NULL ||
        endpoint->receive_result == NULL ||
        data == NULL ||
        length > TEST_LENGTH)
    {
        return;
    }

    result = endpoint->receive_result;

    for (i = 0U; i < length; i++)
    {
        result->data[i] = data[i];
    }

    result->length = length;
    result->received = true;
}


/* Reset simulated CAN queue. */
static void reset_bus(SimulatedCanBus *bus)
{
    bus->head = 0U;
    bus->tail = 0U;
    bus->count = 0U;
}


/* Reset receive result. */
static void reset_result(ReceiveResult *result)
{
    result->length = 0U;
    result->received = false;
}


/* Reset timing measurements. */
static void reset_timing(CfTiming *timing)
{
    timing->count = 0U;
}


/* Verify complete received payload. */
static bool verify_payload(
    const ReceiveResult *result,
    const uint8_t *expected)
{
    uint16_t i;

    if (!result->received)
    {
        printf("FAIL: Message was not received.\n");
        return false;
    }

    if (result->length != TEST_LENGTH)
    {
        printf("FAIL: Incorrect received length: %u\n",
               (unsigned int)result->length);
        return false;
    }

    for (i = 0U; i < TEST_LENGTH; i++)
    {
        if (result->data[i] != expected[i])
        {
            printf("FAIL: Data mismatch at byte %u.\n",
                   (unsigned int)i);
            return false;
        }
    }

    printf("PASS: All %u received bytes verified!\n",
           (unsigned int)TEST_LENGTH);

    return true;
}


/* Verify minimum STmin between consecutive CFs. */
static bool verify_stmin(
    const CfTiming *timing,
    uint32_t minimum_interval_us,
    const char *direction)
{
    uint16_t i;

    printf("%s CF timestamps: %u\n",
           direction,
           (unsigned int)timing->count);

    if (timing->count < 2U)
    {
        printf("FAIL: Not enough CF timestamps.\n");
        return false;
    }

    for (i = 1U; i < timing->count; i++)
    {
        uint32_t interval_us =
            timing->timestamps[i] -
            timing->timestamps[i - 1U];

        if (interval_us < minimum_interval_us)
        {
            printf("FAIL: %s STmin violated between CF %u and CF %u: %lu us\n",
                   direction,
                   (unsigned int)i,
                   (unsigned int)(i + 1U),
                   (unsigned long)interval_us);

            return false;
        }
    }

    printf("PASS: %s CF intervals meet %lu us STmin!\n",
           direction,
           (unsigned long)minimum_interval_us);

    return true;
}


/* Run one complete 1024-byte transfer. */
static bool run_transfer(
    IsoTpContext *tx,
    IsoTpContext *rx,
    SimulatedCanBus *bus,
    ReceiveResult *result,
    const uint8_t *test_data)
{
    uint32_t loop_count;

    if (!isotp_send(tx, test_data, TEST_LENGTH))
    {
        printf("FAIL: Could not start transmission.\n");
        return false;
    }

    printf("1024-byte transmission started.\n");

    for (loop_count = 0U;
         loop_count < MAX_TRANSFER_LOOPS;
         loop_count++)
    {
        simulated_time_us += SIMULATION_STEP_US;

        isotp_poll(tx);
        isotp_poll(rx);

        while (bus->count > 0U)
        {
            deliver_one_frame(bus);
        }

        if (tx->tx_state == ISOTP_TX_IDLE &&
            rx->rx_state == ISOTP_RX_IDLE &&
            result->received)
        {
            break;
        }
    }

    if (loop_count >= MAX_TRANSFER_LOOPS)
    {
        printf("FAIL: Transfer timed out.\n");
        return false;
    }

    return verify_payload(result, test_data);
}


int main(void)
{
    IsoTpContext sender;
    IsoTpContext receiver;

    IsoTpConfig sender_config = {0};
    IsoTpConfig receiver_config = {0};

    SimulatedCanBus bus = {0};

    SimulatedCan sender_can = {0};
    SimulatedCan receiver_can = {0};

    uint8_t test_data[TEST_LENGTH];
    uint16_t i;

    printf("\n--- TWO-CONTEXT ISO-TP TEST ---\n");

    /* Prepare test payload. */
    for (i = 0U; i < TEST_LENGTH; i++)
    {
        test_data[i] = (uint8_t)(i & 0xFFU);
    }

    /* Configure sender endpoint. */
    sender_can.bus = &bus;
    sender_can.peer = &receiver;
    sender_can.tx_id = 0x7E0U;
    sender_can.simulated_time = &simulated_time_us;
    sender_can.receive_result = &sender_received;

    /* Configure receiver endpoint. */
    receiver_can.bus = &bus;
    receiver_can.peer = &sender;
    receiver_can.tx_id = 0x7E8U;
    receiver_can.simulated_time = &simulated_time_us;
    receiver_can.receive_result = &receiver_received;

    /* Configure sender ISO-TP. */
    sender_config.tx_id = 0x7E0U;
    sender_config.rx_id = 0x7E8U;
    sender_config.wft_max = 3U;
    sender_config.block_size = 0U;
    sender_config.rx_block_size = 0U;
    sender_config.st_min = 0U;
    sender_config.n_bs_timeout_ms = 1000U;
    sender_config.n_cr_timeout_ms = 1000U;
    sender_config.send_frame = can_send;
    sender_config.get_time_us = get_time_us;
    sender_config.receive_message = on_message;
    sender_config.user = &sender_can;

    /* Configure receiver ISO-TP. */
    receiver_config.tx_id = 0x7E8U;
    receiver_config.rx_id = 0x7E0U;
    receiver_config.wft_max = 3U;
    receiver_config.block_size = 0U;
    receiver_config.rx_block_size = 1U;
    receiver_config.st_min = 0xF1U; 
    receiver_config.n_bs_timeout_ms = 1000U;
    receiver_config.n_cr_timeout_ms = 1000U;
    receiver_config.send_frame = can_send;
    receiver_config.get_time_us = get_time_us;
    receiver_config.receive_message = on_message;
    receiver_config.user = &receiver_can;

    /* Initialize both ISO-TP contexts. */
    if (!isotp_init(&sender, &sender_config))
    {
        printf("FAIL: Sender initialization failed.\n");
        return 1;
    }

    if (!isotp_init(&receiver, &receiver_config))
    {
        printf("FAIL: Receiver initialization failed.\n");
        return 1;
    }

    printf("Sender and receiver initialized.\n");

    /* -------------------------------------------------- */
    /* FORWARD TRANSFER: 0x7E0 -> 0x7E8                  */
    /* -------------------------------------------------- */

    printf("\n--- FORWARD 1024-BYTE TRANSFER ---\n");

    if (!run_transfer(
            &sender,
            &receiver,
            &bus,
            &receiver_received,
            test_data))
    {
        return 1;
    }

    if (!verify_stmin(
        &forward_timing,
        100U,
        "Forward"))
    {
        return 1;
    }

    printf("PASS: Forward 1024-byte transfer!\n");

    /* -------------------------------------------------- */
    /* REVERSE TRANSFER: 0x7E8 -> 0x7E0                  */
    /* -------------------------------------------------- */

    printf("\n--- REVERSE 1024-BYTE TRANSFER ---\n");

    reset_bus(&bus);
    reset_result(&sender_received);
    reset_timing(&reverse_timing);
    printf("Forward CTS Flow Control frames: %u\n",
       (unsigned int)forward_fc_stats.cts_count);
       printf("Reverse CTS Flow Control frames: %u\n",
       (unsigned int)reverse_fc_stats.cts_count);
       if (forward_fc_stats.cts_count != forward_timing.count)
{
    printf("FAIL: Forward Block Size 1 verification!\n");
    return 1;
}

printf("PASS: Forward Block Size 1 verified!\n");
if (reverse_fc_stats.cts_count != reverse_timing.count)
{
    printf("FAIL: Reverse Block Size 1 verification!\n");
    return 1;
}

printf("PASS: Reverse Block Size 1 verified!\n");
    /*
     * Original sender becomes receiver.
     * It now advertises BS=8 and STmin=5 ms.
     */
    sender.config.rx_block_size = 1U;
    sender.config.st_min = 0xF1U;
    if (!run_transfer(
            &receiver,
            &sender,
            &bus,
            &sender_received,
            test_data))
    {
        return 1;
    }

    if (!verify_stmin(
        &reverse_timing,
        100U,
        "Reverse"))
    {
        return 1;
    }

    printf("PASS: Reverse 1024-byte transfer!\n");

    printf("\nALL TWO-CONTEXT TESTS COMPLETED!\n");

    return 0;
}
