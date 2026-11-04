
#include <stdio.h>
#include "isotp.h"

static uint32_t simulated_time_ms = 0;
static uint8_t received_data[ISOTP_MAX_PAYLOAD];
static uint16_t received_length = 0;
static bool message_received = false;
static bool simulate_can_send_failure = false;

static void capture_message(
    void *user,
    const uint8_t *data,
    uint16_t length)
{
    uint16_t i;
    (void)user;

    if (length > ISOTP_MAX_PAYLOAD)
        return;

    for (i = 0; i < length; i++)
        received_data[i] = data[i];

    received_length = length;
    message_received = true;
}

static void on_message(
    void *user,
    const uint8_t *data,
    uint16_t length)
{
    uint16_t i;
    (void)user;

    printf("\nReceived ISO-TP message:\n");
    printf("Length: %u bytes\n", (unsigned int)length);
    printf("Data: ");

    for (i = 0; i < length; i++)
        printf("%02X ", data[i]);

    printf("\n");
}

static bool can_send(void *user, const IsoTpCanFrame *frame)
{
    uint8_t i;
    (void)user;

    if (simulate_can_send_failure)
    {
        printf("\nSimulated CAN transmission failure!\n");
        return false;
    }

    printf("\nTransmitted CAN frame:\n");
    printf("CAN ID: 0x%03X\n", (unsigned int)frame->id);
    printf("DLC: %u\n", (unsigned int)frame->dlc);
    printf("Data: ");

    for (i = 0; i < frame->dlc; i++)
        printf("%02X ", frame->data[i]);

    printf("\n");
    return true;
}

static uint32_t get_time_us(void *user)
{
    (void)user;
    return simulated_time_ms * 1000U;
}

int main(void)
{
    IsoTpContext context;
    IsoTpConfig config = {0};
    IsoTpCanFrame incoming_frame;

    uint8_t tx_data[] = {0x22, 0xF1, 0x90};

    uint8_t long_data[20] = {
        0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14
    };

    /* Configure ISO-TP */
    config.tx_id = 0x7E0;
    config.rx_id = 0x7E8;
    config.wft_max = 3;
    config.block_size = 0;
    config.rx_block_size = 1;
    config.st_min = 0;
    config.n_bs_timeout_ms = 1000;
    config.n_cr_timeout_ms = 1000;
    config.send_frame = can_send;
    config.get_time_us = get_time_us;
    config.receive_message = on_message;
    config.user = NULL;

    if (!isotp_init(&context, &config))
    {
        printf("ISO-TP initialization failed!\n");
        return 1;
    }

    printf("ISO-TP initialized successfully!\n");

    /* SINGLE FRAME TRANSMISSION */
    printf("\n--- SINGLE FRAME TRANSMISSION TEST ---\n");

    if (isotp_send(&context, tx_data, sizeof(tx_data)))
        printf("Transmission successful!\n");
    else
        printf("Transmission failed!\n");

    /* SINGLE FRAME RECEPTION */
    printf("\n--- SINGLE FRAME RECEPTION TEST ---\n");

    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    incoming_frame.data[0] = 0x03;
    incoming_frame.data[1] = 0x62;
    incoming_frame.data[2] = 0xF1;
    incoming_frame.data[3] = 0x90;

    isotp_on_can_frame(&context, &incoming_frame);

    /* FIRST FRAME TRANSMISSION */
    printf("\n--- FIRST FRAME TRANSMISSION TEST ---\n");

    printf("Long data size = %u bytes\n",
           (unsigned int)sizeof(long_data));

    context.tx_state = ISOTP_TX_IDLE;

    if (isotp_send(&context, long_data, sizeof(long_data)))
        printf("First Frame transmission successful!\n");
    else
        printf("First Frame transmission failed!\n");

    /* FLOW CONTROL TESTS */
    printf("\n--- FLOW CONTROL TESTS ---\n");

    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    /* CTS */
    printf("\nTesting CTS...\n");

    incoming_frame.data[0] = 0x30;
    incoming_frame.data[1] = 4;
    incoming_frame.data[2] = 10;

    isotp_on_can_frame(&context, &incoming_frame);

    printf("TX State: %d\n", context.tx_state);
    printf("Block Size: %u\n", context.tx_block_size);
    printf("STmin: %u ms\n", context.tx_st_min);

    /* WAIT */
    printf("\nTesting WAIT...\n");

    context.tx_state = ISOTP_TX_IDLE;

    isotp_send(&context, long_data, sizeof(long_data));

    incoming_frame.data[0] = 0x31;
    incoming_frame.data[1] = 0;
    incoming_frame.data[2] = 0;

    isotp_on_can_frame(&context, &incoming_frame);

    printf("TX State after WAIT: %d\n", context.tx_state);

        /* OVERFLOW */
    printf("\nTesting OVERFLOW...\n");

    incoming_frame.data[0] = 0x32;
    incoming_frame.data[1] = 0;
    incoming_frame.data[2] = 0;

    isotp_on_can_frame(&context, &incoming_frame);

    printf("TX State after OVERFLOW: %d\n", context.tx_state);

    if (context.tx_state != ISOTP_TX_IDLE)
    {
        printf("FAIL: OVERFLOW did not reset TX state!\n");
        return 1;
    }

    printf("PASS: OVERFLOW reset TX state!\n");
    /* CONSECUTIVE FRAME TRANSMISSION: BS = 1 */
    printf("\n--- BLOCK SIZE = 1 TX TEST ---\n");

    context.tx_state = ISOTP_TX_IDLE;
    simulated_time_ms = 0;

    if (isotp_send(&context, long_data, sizeof(long_data)))
    {
        IsoTpCanFrame fc = {0};

        fc.id = config.rx_id;
        fc.dlc = 8;

        printf("\nSending first CTS...\n");

        fc.data[0] = 0x30;
        fc.data[1] = 1;
        fc.data[2] = 10;

        isotp_on_can_frame(&context, &fc);

        simulated_time_ms = 0;
        isotp_poll(&context);

        simulated_time_ms = 10;
        isotp_poll(&context);

        printf("TX State after first block: %d\n",
               context.tx_state);

        printf("\nSending second CTS...\n");

        fc.data[0] = 0x30;
        fc.data[1] = 1;
        fc.data[2] = 10;

        isotp_on_can_frame(&context, &fc);

        simulated_time_ms = 20;
        isotp_poll(&context);

        printf("TX State after second block: %d\n",
               context.tx_state);
    }
    else
    {
        printf("Could not start Block Size TX test.\n");
    }

    /* FIRST FRAME RECEPTION */
    printf("\n--- FIRST FRAME RECEPTION TEST ---\n");

    simulated_time_ms = 0;

    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    incoming_frame.data[0] = 0x10;
    incoming_frame.data[1] = 0x14;
    incoming_frame.data[2] = 1;
    incoming_frame.data[3] = 2;
    incoming_frame.data[4] = 3;
    incoming_frame.data[5] = 4;
    incoming_frame.data[6] = 5;
    incoming_frame.data[7] = 6;

    isotp_on_can_frame(&context, &incoming_frame);

    printf("RX State: %d\n", context.rx_state);
    printf("RX Length: %u\n", context.rx_length);
    printf("RX Offset: %u\n", context.rx_offset);

    /* CONSECUTIVE FRAME RECEPTION */
    printf("\n--- CONSECUTIVE FRAME RECEPTION TEST ---\n");

    {
        IsoTpCanFrame cf1 = {0};

        cf1.id = 0x7E8;
        cf1.dlc = 8;

        cf1.data[0] = 0x21;
        cf1.data[1] = 0x07;
        cf1.data[2] = 0x08;
        cf1.data[3] = 0x09;
        cf1.data[4] = 0x0A;
        cf1.data[5] = 0x0B;
        cf1.data[6] = 0x0C;
        cf1.data[7] = 0x0D;

        printf("\nReceiving first Consecutive Frame...\n");
        isotp_on_can_frame(&context, &cf1);

        {
            IsoTpCanFrame cf2 = {0};

            cf2.id = 0x7E8;
            cf2.dlc = 8;

            cf2.data[0] = 0x22;
            cf2.data[1] = 0x0E;
            cf2.data[2] = 0x0F;
            cf2.data[3] = 0x10;
            cf2.data[4] = 0x11;
            cf2.data[5] = 0x12;
            cf2.data[6] = 0x13;
            cf2.data[7] = 0x14;

            printf("\nReceiving second Consecutive Frame...\n");
            isotp_on_can_frame(&context, &cf2);
        }
    }

    printf("\nRX State after complete message: %d\n",
           context.rx_state);

    if (context.rx_state == ISOTP_RX_IDLE)
        printf("PASS: Consecutive Frame reception completed!\n");
    else
        printf("FAIL: Consecutive Frame reception incomplete!\n");

    /* RECEIVER BLOCK SIZE = 1 TEST */
    printf("\n--- RECEIVER BLOCK SIZE = 1 TEST ---\n");

    simulated_time_ms = 0;

    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    incoming_frame.data[0] = 0x10;
    incoming_frame.data[1] = 0x14;
    incoming_frame.data[2] = 1;
    incoming_frame.data[3] = 2;
    incoming_frame.data[4] = 3;
    incoming_frame.data[5] = 4;
    incoming_frame.data[6] = 5;
    incoming_frame.data[7] = 6;

    printf("\nReceiving First Frame...\n");
    isotp_on_can_frame(&context, &incoming_frame);

    printf("RX State: %d\n", context.rx_state);

    {
        IsoTpCanFrame rx_cf1 = {0};

        rx_cf1.id = 0x7E8;
        rx_cf1.dlc = 8;
        rx_cf1.data[0] = 0x21;
        rx_cf1.data[1] = 7;
        rx_cf1.data[2] = 8;
        rx_cf1.data[3] = 9;
        rx_cf1.data[4] = 10;
        rx_cf1.data[5] = 11;
        rx_cf1.data[6] = 12;
        rx_cf1.data[7] = 13;

        printf("\nReceiving first CF...\n");
        isotp_on_can_frame(&context, &rx_cf1);

        printf("RX Offset after first CF: %u\n", context.rx_offset);
    }

    {
        IsoTpCanFrame rx_cf2 = {0};

        rx_cf2.id = 0x7E8;
        rx_cf2.dlc = 8;
        rx_cf2.data[0] = 0x22;
        rx_cf2.data[1] = 14;
        rx_cf2.data[2] = 15;
        rx_cf2.data[3] = 16;
        rx_cf2.data[4] = 17;
        rx_cf2.data[5] = 18;
        rx_cf2.data[6] = 19;
        rx_cf2.data[7] = 20;

        printf("\nReceiving final CF...\n");
        isotp_on_can_frame(&context, &rx_cf2);
    }

    printf("RX State after final CF: %d\n", context.rx_state);

    if (context.rx_state == ISOTP_RX_IDLE)
        printf("PASS: Receiver completed message with Block Size = 1!\n");
    else
        printf("FAIL: Receiver did not complete message!\n");

    /* MALFORMED CAN DLC TEST */
   /* MALFORMED CAN DLC TEST */
printf("\n--- MALFORMED CAN DLC TEST ---\n");

/* Make sure the receiver starts in IDLE state */
context.rx_state = ISOTP_RX_IDLE;
context.rx_length = 0;
context.rx_offset = 0;

incoming_frame = (IsoTpCanFrame){0};
incoming_frame.id = 0x7E8;
incoming_frame.dlc = 9;  /* Invalid: classic CAN supports max DLC 8 */

incoming_frame.data[0] = 0x01;
incoming_frame.data[1] = 0xAA;

printf("Sending frame with invalid DLC = 9...\n");
isotp_on_can_frame(&context, &incoming_frame);

printf("RX State after invalid DLC: %d\n", context.rx_state);
printf("RX Length after invalid DLC: %u\n", context.rx_length);

if (context.rx_state == ISOTP_RX_IDLE &&
    context.rx_length == 0 &&
    context.rx_offset == 0)
{
    printf("PASS: Frame with invalid DLC was rejected!\n");
}
else
{
    printf("FAIL: Invalid DLC frame was not handled correctly!\n");
}
    /* MALFORMED SINGLE FRAME TEST */
   /* MALFORMED SINGLE FRAME TEST */
printf("\n--- MALFORMED SINGLE FRAME TEST ---\n");

/* Make sure the receiver starts in IDLE state */
context.rx_state = ISOTP_RX_IDLE;
context.rx_length = 0;
context.rx_offset = 0;

incoming_frame = (IsoTpCanFrame){0};
incoming_frame.id = 0x7E8;
incoming_frame.dlc = 4;

/* Declared payload length = 7, but only 3 payload bytes are present */
incoming_frame.data[0] = 0x07;
incoming_frame.data[1] = 0x62;
incoming_frame.data[2] = 0xF1;
incoming_frame.data[3] = 0x90;

printf("Sending malformed Single Frame...\n");
isotp_on_can_frame(&context, &incoming_frame);

printf("RX State after malformed SF: %d\n", context.rx_state);
printf("RX Length after malformed SF: %u\n", context.rx_length);

if (context.rx_state == ISOTP_RX_IDLE &&
    context.rx_length == 0 &&
    context.rx_offset == 0)
{
    printf("PASS: Malformed Single Frame was rejected!\n");
}
else
{
    printf("FAIL: Malformed Single Frame was not handled correctly!\n");
}

    /* MALFORMED FIRST FRAME LENGTH TEST */
    /* MALFORMED FIRST FRAME LENGTH TEST */
printf("\n--- MALFORMED FIRST FRAME LENGTH TEST ---\n");

/* Make sure the receiver starts in IDLE state */
context.rx_state = ISOTP_RX_IDLE;
context.rx_length = 0;
context.rx_offset = 0;

incoming_frame = (IsoTpCanFrame){0};
incoming_frame.id = 0x7E8;
incoming_frame.dlc = 8;

/* First Frame PCI: length = 1500 bytes */
incoming_frame.data[0] = 0x15;
incoming_frame.data[1] = 0xDC;

printf("Sending First Frame claiming 1500 bytes...\n");
isotp_on_can_frame(&context, &incoming_frame);

printf("RX State after malformed First Frame: %d\n",
       context.rx_state);
printf("RX Length after malformed First Frame: %u\n",
       context.rx_length);

if (context.rx_state == ISOTP_RX_IDLE &&
    context.rx_length == 0 &&
    context.rx_offset == 0)
{
    printf("PASS: Oversized First Frame was rejected!\n");
}
else
{
    printf("FAIL: Oversized First Frame was not handled correctly!\n");
}

    /* WRONG CF SEQUENCE NUMBER TEST */
    printf("\n--- WRONG CF SEQUENCE NUMBER TEST ---\n");

    simulated_time_ms =2000;
    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    incoming_frame.data[0] = 0x10;
    incoming_frame.data[1] = 0x14;
    incoming_frame.data[2] = 1;
    incoming_frame.data[3] = 2;
    incoming_frame.data[4] = 3;
    incoming_frame.data[5] = 4;
    incoming_frame.data[6] = 5;
    incoming_frame.data[7] = 6;

    isotp_on_can_frame(&context, &incoming_frame);

    printf("RX State before wrong CF: %d\n", context.rx_state);

    {
        IsoTpCanFrame wrong_cf = {0};

        wrong_cf.id = 0x7E8;
        wrong_cf.dlc = 8;

        wrong_cf.data[0] = 0x22;
        wrong_cf.data[1] = 7;
        wrong_cf.data[2] = 8;
        wrong_cf.data[3] = 9;
        wrong_cf.data[4] = 10;
        wrong_cf.data[5] = 11;
        wrong_cf.data[6] = 12;
        wrong_cf.data[7] = 13;

        printf("\nSending incorrect CF sequence number 2...\n");
        isotp_on_can_frame(&context, &wrong_cf);
    }

    printf("RX State after wrong CF: %d\n", context.rx_state);
    printf("RX Length after wrong CF: %u\n", context.rx_length);
    printf("RX Offset after wrong CF: %u\n", context.rx_offset);

    if (context.rx_state == ISOTP_RX_IDLE &&
        context.rx_length == 0 &&
        context.rx_offset == 0)
    {
        printf("PASS: Incorrect CF sequence was rejected!\n");
    }
    else
    {
        printf("FAIL: Incorrect CF sequence was not rejected!\n");
        return 1;
    }

    /* N_Cr RECEIVER TIMEOUT TEST */
    printf("\n--- N_Cr TIMEOUT TEST ---\n");

    simulated_time_ms = 0;

    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    incoming_frame.data[0] = 0x10;
    incoming_frame.data[1] = 0x14;
    incoming_frame.data[2] = 1;
    incoming_frame.data[3] = 2;
    incoming_frame.data[4] = 3;
    incoming_frame.data[5] = 4;
    incoming_frame.data[6] = 5;
    incoming_frame.data[7] = 6;

    printf("Sending First Frame...\n");
    isotp_on_can_frame(&context, &incoming_frame);

    printf("RX State before timeout: %d\n", context.rx_state);
    printf("RX Length before timeout: %u\n", context.rx_length);

    simulated_time_ms = 1100;

    printf("\nWaiting for N_Cr timeout...\n");
    isotp_poll(&context);

    printf("RX State after timeout: %d\n", context.rx_state);
    printf("RX Length after timeout: %u\n", context.rx_length);
    printf("RX Offset after timeout: %u\n", context.rx_offset);

    if (context.rx_state == ISOTP_RX_IDLE &&
        context.rx_length == 0 &&
        context.rx_offset == 0)
    {
        printf("PASS: N_Cr timeout worked!\n");
    }
    else
    {
        printf("FAIL: N_Cr timeout did not work!\n");
        return 1;
    }

    /* MAXIMUM PAYLOAD LENGTH FIRST FRAME TEST */
    printf("\n--- MAXIMUM PAYLOAD LENGTH TEST ---\n");

    incoming_frame = (IsoTpCanFrame){0};
    incoming_frame.id = 0x7E8;
    incoming_frame.dlc = 8;

    /* 1024 decimal = 0x0400 */
    incoming_frame.data[0] = 0x14;
    incoming_frame.data[1] = 0x00;

    incoming_frame.data[2] = 0x01;
    incoming_frame.data[3] = 0x02;
    incoming_frame.data[4] = 0x03;
    incoming_frame.data[5] = 0x04;
    incoming_frame.data[6] = 0x05;
    incoming_frame.data[7] = 0x06;

    printf("Sending First Frame claiming 1024 bytes...\n");
    isotp_on_can_frame(&context, &incoming_frame);

    printf("RX State: %d\n", context.rx_state);
    printf("RX Length: %u\n", context.rx_length);
    printf("RX Offset: %u\n", context.rx_offset);

    if (context.rx_state == ISOTP_RX_RECEIVING &&
        context.rx_length == 1024 &&
        context.rx_offset == 6)
    {
        printf("PASS: Maximum payload First Frame accepted!\n");
    }
    else
    {
        printf("FAIL: Maximum payload First Frame handling!\n");
        return 1;
    }

    /* COMPLETE 1024-BYTE ISO-TP RECEPTION TEST */
    printf("\n--- COMPLETE 1024-BYTE TRANSFER TEST ---\n");

    {
        uint8_t test_data[ISOTP_MAX_PAYLOAD];
        uint16_t i;
        uint16_t offset;
        uint8_t sequence_number;
        IsoTpCanFrame frame = {0};
        bool data_matches = true;

        /* Prepare known test data: 0x00, 0x01, ... */
        for (i = 0; i < ISOTP_MAX_PAYLOAD; i++)
            test_data[i] = (uint8_t)i;

        received_length = 0;
        message_received = false;

        /* Use capture callback for this test. */
        context.config.receive_message = capture_message;

        /* Reset receiver state before starting. */
        context.rx_state = ISOTP_RX_IDLE;
        context.rx_length = 0;
        context.rx_offset = 0;

        context.config.rx_block_size = 0;

        simulated_time_ms = 0;

        /* Send First Frame: total length = 1024 (0x0400). */
        frame.id = config.rx_id;
        frame.dlc = 8;

        frame.data[0] = 0x14;
        frame.data[1] = 0x00;

        for (i = 0; i < 6; i++)
            frame.data[i + 2] = test_data[i];

        printf("Sending First Frame...\n");
        isotp_on_can_frame(&context, &frame);

        if (context.rx_state != ISOTP_RX_RECEIVING ||
            context.rx_length != ISOTP_MAX_PAYLOAD)
        {
            printf("FAIL: First Frame was not accepted!\n");
            return 1;
        }

        /* Send remaining bytes in Consecutive Frames. */
        offset = 6;
        sequence_number = 1;

        while (offset < ISOTP_MAX_PAYLOAD)
        {
            uint8_t j;

            frame = (IsoTpCanFrame){0};
            frame.id = config.rx_id;
            frame.dlc = 8;

            frame.data[0] =
                (uint8_t)(0x20U | sequence_number);

            for (j = 0; j < 7; j++)
            {
                if (offset < ISOTP_MAX_PAYLOAD)
                    frame.data[j + 1] = test_data[offset++];
                else
                    frame.data[j + 1] = 0;
            }

            isotp_on_can_frame(&context, &frame);

            sequence_number =
                (uint8_t)((sequence_number + 1U) & 0x0FU);
        }

        /* Verify the complete received message. */
        if (!message_received)
        {
            printf("FAIL: Receive callback was not called!\n");
            return 1;
        }

        if (received_length != ISOTP_MAX_PAYLOAD)
        {
            printf("FAIL: Incorrect received length: %u\n",
                   (unsigned int)received_length);
            return 1;
        }

        for (i = 0; i < ISOTP_MAX_PAYLOAD; i++)
        {
            if (received_data[i] != test_data[i])
            {
                printf("FAIL: Data mismatch at byte %u\n",
                       (unsigned int)i);
                data_matches = false;
                break;
            }
        }

        if (data_matches &&
            context.rx_state == ISOTP_RX_IDLE)
        {
            printf("PASS: All 1024 bytes received correctly!\n");
            printf("PASS: Consecutive Frame sequence numbers wrapped correctly!\n");
        }
        else
        {
            printf("FAIL: Complete transfer verification failed!\n");
            return 1;
        }

        /* Restore original callback and block size. */
        context.config.receive_message = on_message;
        context.config.rx_block_size = 1;
    }

    /* N_Bs TIMEOUT TEST */
    printf("\n--- N_Bs TIMEOUT TEST ---\n");

    if (context.tx_state != ISOTP_TX_IDLE)
    {
        printf("ERROR: Previous transmission is not idle.\n");
        return 1;
    }

    simulated_time_ms = 0;

    if (isotp_send(&context, long_data, sizeof(long_data)))
    {
        printf("First Frame sent. Waiting for Flow Control...\n");
        printf("TX State before timeout: %d\n", context.tx_state);

        simulated_time_ms = 1100;
        isotp_poll(&context);

        printf("TX State after timeout: %d\n", context.tx_state);

        if (context.tx_state == ISOTP_TX_IDLE)
            printf("PASS: N_Bs timeout worked!\n");
        else
        {
            printf("FAIL: N_Bs timeout did not work!\n");
            return 1;
        }
    }
    else
    {
        printf("FAIL: Could not start timeout test.\n");
        return 1;
    }

    /* FLOW CONTROL WAIT LIMIT TEST */
    printf("\n--- FLOW CONTROL WAIT LIMIT TEST ---\n");

    {
        IsoTpCanFrame wait_frame = {0};
        uint8_t i;

        simulated_time_ms = 0;

        wait_frame.id = config.rx_id;
        wait_frame.dlc = 8;
        wait_frame.data[0] = 0x31; /* FC WAIT */

        /* Start a multi-frame transmission. */
        if (!isotp_send(&context, long_data, sizeof(long_data)))
        {
            printf("FAIL: Could not start WAIT limit test.\n");
            return 1;
        }

        printf("Transmission started.\n");
        printf("Configured wft_max: %u\n",
               (unsigned int)config.wft_max);

        /* Send three permitted WAIT frames. */
        for (i = 1U; i <= 3U; i++)
        {
            isotp_on_can_frame(&context, &wait_frame);

            printf("WAIT %u: TX state = %d, WAIT count = %u\n",
                   (unsigned int)i,
                   context.tx_state,
                   (unsigned int)context.tx_wait_count);

            if (context.tx_state != ISOTP_TX_WAIT_FC)
            {
                printf("FAIL: Sender aborted too early!\n");
                return 1;
            }
        }

        /* Send the fourth WAIT frame. */
        printf("Sending fourth WAIT frame...\n");
        isotp_on_can_frame(&context, &wait_frame);

        printf("TX state after fourth WAIT: %d\n",
               context.tx_state);

        if (context.tx_state == ISOTP_TX_IDLE &&
            context.tx_wait_count == 0U)
        {
            printf("PASS: WAIT frame limit enforced!\n");
        }
        else
        {
            printf("FAIL: WAIT frame limit not enforced!\n");
            return 1;
        }
    }
        /* MAXIMUM FLOW CONTROL WAIT LIMIT TEST */
    printf("\n--- MAXIMUM WFT_MAX = 255 TEST ---\n");

    {
        IsoTpConfig max_wait_config = config;
        IsoTpCanFrame wait_frame = {0};
        uint16_t i;

        /* Configure and initialize a context with wft_max = 255. */
        max_wait_config.wft_max = 255U;

        if (!isotp_init(&context, &max_wait_config))
        {
            printf("FAIL: Could not initialize maximum WAIT test!\n");
            return 1;
        }

        simulated_time_ms = 0;

        wait_frame.id = config.rx_id;
        wait_frame.dlc = 8;
        wait_frame.data[0] = 0x31; /* FC WAIT */

        if (!isotp_send(&context, long_data, sizeof(long_data)))
        {
            printf("FAIL: Could not start maximum WAIT test!\n");
            return 1;
        }

        printf("Configured wft_max: %u\n",
               (unsigned int)context.config.wft_max);

        /* Send all 255 permitted WAIT frames. */
        for (i = 1U; i <= 255U; i++)
        {
            isotp_on_can_frame(&context, &wait_frame);

            if (context.tx_state != ISOTP_TX_WAIT_FC ||
                context.tx_wait_count != (uint8_t)i)
            {
                printf("FAIL: Incorrect state at WAIT %u!\n",
                       (unsigned int)i);
                return 1;
            }
        }

        printf("PASS: All 255 WAIT frames were accepted!\n");
        printf("TX State after 255 WAIT frames: %d\n",
               context.tx_state);
        printf("WAIT count after 255 frames: %u\n",
               (unsigned int)context.tx_wait_count);

        /* The 256th WAIT frame must abort the transmission. */
        printf("Sending WAIT frame number 256...\n");

        isotp_on_can_frame(&context, &wait_frame);

        printf("TX State after 256 WAIT frames: %d\n",
               context.tx_state);
        printf("WAIT count after abort: %u\n",
               (unsigned int)context.tx_wait_count);

        if (context.tx_state == ISOTP_TX_IDLE &&
            context.tx_wait_count == 0U)
        {
            printf("PASS: 256th WAIT frame correctly aborted transmission!\n");
        }
        else
        {
            printf("FAIL: Maximum WAIT limit was not enforced!\n");
            return 1;
        }

        /* Restore the original configuration (wft_max = 3). */
        if (!isotp_init(&context, &config))
        {
            printf("FAIL: Could not restore original ISO-TP configuration!\n");
            return 1;
        }

        printf("Original configuration restored: wft_max = %u\n",
               (unsigned int)context.config.wft_max);
    }

    /* INITIAL FIRST FRAME SEND FAILURE TEST */
    printf("\n--- INITIAL FIRST FRAME SEND FAILURE TEST ---\n");

    simulated_time_ms = 0;
    simulate_can_send_failure = true;

    if (!isotp_send(&context, long_data, sizeof(long_data)))
    {
        printf("PASS: isotp_send() reported transmission failure!\n");
    }
    else
    {
        printf("FAIL: isotp_send() reported success unexpectedly!\n");
        return 1;
    }

    simulate_can_send_failure = false;

    printf("TX State after send failure: %d\n", context.tx_state);

    if (context.tx_state == ISOTP_TX_IDLE)
    {
        printf("PASS: TX state returned to IDLE!\n");
    }
    else
    {
        printf("FAIL: TX state did not return to IDLE!\n");
        return 1;
    }

    /* CONSECUTIVE FRAME SEND FAILURE TEST */
    printf("\n--- CONSECUTIVE FRAME SEND FAILURE TEST ---\n");

    simulated_time_ms =2000;

    if (!isotp_send(&context, long_data, sizeof(long_data)))
    {
        printf("FAIL: Could not start transmission!\n");
        return 1;
    }

    {
        IsoTpCanFrame fc_frame = {0};

        fc_frame.id = config.rx_id;
        fc_frame.dlc = 8;
        fc_frame.data[0] = 0x30; /* CTS */
        fc_frame.data[1] = 0x00; /* Block Size = unlimited */
        fc_frame.data[2] = 0x00; /* STmin = 0 ms */

        isotp_on_can_frame(&context, &fc_frame);
    }

    printf("TX State after CTS: %d\n", context.tx_state);

    simulate_can_send_failure = true;
    isotp_poll(&context);
    simulate_can_send_failure = false;

    printf("TX State after CF send failure: %d\n", context.tx_state);

    if (context.tx_state == ISOTP_TX_IDLE)
    {
        printf("PASS: TX returned to IDLE after CF send failure!\n");
    }
    else
    {
        printf("FAIL: TX did not return to IDLE!\n");
        return 1;
    }

    /* RECEIVER FLOW CONTROL SEND FAILURE TEST */
    printf("\n--- RECEIVER FLOW CONTROL SEND FAILURE TEST ---\n");

    simulated_time_ms = 0;

    {
        IsoTpCanFrame first_frame = {0};

        first_frame.id = config.rx_id;
        first_frame.dlc = 8;
        first_frame.data[0] = 0x10;
        first_frame.data[1] = 0x14;

        first_frame.data[2] = 0x01;
        first_frame.data[3] = 0x02;
        first_frame.data[4] = 0x03;
        first_frame.data[5] = 0x04;
        first_frame.data[6] = 0x05;
        first_frame.data[7] = 0x06;

        simulate_can_send_failure = true;
        isotp_on_can_frame(&context, &first_frame);
        simulate_can_send_failure = false;
    }

    printf("RX State after FC send failure: %d\n", context.rx_state);
    printf("RX Length after FC send failure: %u\n",
           (unsigned int)context.rx_length);

    if (context.rx_state == ISOTP_RX_IDLE &&
        context.rx_length == 0)
    {
        printf("PASS: Receiver returned to IDLE after FC send failure!\n");
    }
    else
    {
        printf("FAIL: Receiver did not reset correctly!\n");
        return 1;
    }

    printf("\n--- ALL TESTS COMPLETED ---\n");

    return 0;
}
