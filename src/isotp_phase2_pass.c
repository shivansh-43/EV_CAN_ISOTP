
#include "isotp.h"
#include <stddef.h>

/* Validate supported ISO-TP STmin encodings. */
static bool isotp_is_valid_st_min(uint8_t st_min)
{
    return ((st_min <= 0x7FU) ||
            ((st_min >= 0xF1U) && (st_min <= 0xF9U)));
}

/* Convert STmin to microseconds. */
static uint32_t isotp_st_min_to_us(uint8_t st_min)
{
    if (st_min <= 0x7FU)
    {
        return (uint32_t)st_min * 1000U;
    }

    return (uint32_t)(st_min - 0xF0U) * 100U;
}

/* Get current time in microseconds. */
static uint32_t isotp_now(IsoTpContext *context)
{
    return context->config.get_time_us(context->config.user);
}

/* Convert milliseconds to microseconds. */
static uint32_t isotp_timeout_to_us(uint16_t timeout_ms)
{
    return (uint32_t)timeout_ms * 1000U;
}

/* Reset transmission state. */
static void isotp_reset_tx(IsoTpContext *context)
{
    context->tx_state = ISOTP_TX_IDLE;
    context->tx_length = 0U;
    context->tx_offset = 0U;
    context->tx_block_count = 0U;
    context->tx_wait_count = 0U;
}

/* Reset reception state. */
static void isotp_reset_rx(IsoTpContext *context)
{
    context->rx_state = ISOTP_RX_IDLE;
    context->rx_length = 0U;
    context->rx_offset = 0U;
    context->rx_block_count = 0U;
}

/* Send one CAN frame. */
static bool isotp_send_frame(
    IsoTpContext *context,
    const uint8_t *data,
    uint8_t dlc)
{
    IsoTpCanFrame frame;
    uint8_t i;

    if ((dlc == 0U) || (dlc > ISOTP_CAN_DLEN))
    {
        return false;
    }

    frame.id = context->config.tx_id;
    frame.dlc = dlc;

    for (i = 0U; i < ISOTP_CAN_DLEN; i++)
    {
        frame.data[i] = data[i];
    }

    return context->config.send_frame(
        context->config.user,
        &frame);
}

/* Send Flow Control. */
static bool isotp_send_flow_control(
    IsoTpContext *context,
    uint8_t flow_status)
{
    uint8_t data[ISOTP_CAN_DLEN] = {0U};

    data[0] = (uint8_t)(0x30U | (flow_status & 0x0FU));
    data[1] = context->config.rx_block_size;
    data[2] = context->config.st_min;

    return isotp_send_frame(context, data, ISOTP_CAN_DLEN);
}

/* Initialize ISO-TP context. */
bool isotp_init(
    IsoTpContext *context,
    const IsoTpConfig *config)
{
    if ((context == NULL) || (config == NULL))
    {
        return false;
    }

    if ((config->send_frame == NULL) ||
        (config->get_time_us == NULL) ||
        (config->receive_message == NULL))
    {
        return false;
    }

    if ((config->tx_id > 0x7FFU) ||
        (config->rx_id > 0x7FFU))
    {
        return false;
    }

    if (!isotp_is_valid_st_min(config->st_min))
    {
        return false;
    }

    if ((config->n_bs_timeout_ms == 0U) ||
        (config->n_cr_timeout_ms == 0U))
    {
        return false;
    }

    *context = (IsoTpContext){0};

    context->config = *config;
    context->tx_state = ISOTP_TX_IDLE;
    context->rx_state = ISOTP_RX_IDLE;
    context->initialized = true;

    return true;
}

/* Start ISO-TP transmission. */
bool isotp_send(
    IsoTpContext *context,
    const uint8_t *data,
    uint16_t length)
{
    uint8_t frame_data[ISOTP_CAN_DLEN] = {0U};
    uint16_t i;
    uint32_t now;

    if ((context == NULL) ||
        (data == NULL) ||
        (!context->initialized) ||
        (length == 0U) ||
        (length > ISOTP_MAX_PAYLOAD))
    {
        return false;
    }

    if (context->tx_state != ISOTP_TX_IDLE)
    {
        return false;
    }

    /* Single Frame. */
    if (length <= 7U)
    {
        frame_data[0] = (uint8_t)length;

        for (i = 0U; i < length; i++)
        {
            frame_data[i + 1U] = data[i];
        }

        return isotp_send_frame(context, frame_data, 8U);
    }

    /* First Frame. */
    now = isotp_now(context);

    context->tx_length = length;
    context->tx_offset = 6U;
    context->tx_sequence_number = 1U;
    context->tx_block_count = 0U;
    context->tx_wait_count = 0U;

    /*
     * IMPORTANT:
     * Initialize the CF timing deadline for every new transfer.
     * This prevents an old transfer's deadline from affecting
     * a newly started transmission.
     */
    context->tx_next_time = now;

    for (i = 0U; i < length; i++)
    {
        context->tx_buffer[i] = data[i];
    }

    frame_data[0] =
        (uint8_t)(0x10U | ((length >> 8U) & 0x0FU));

    frame_data[1] = (uint8_t)(length & 0xFFU);

    for (i = 0U; i < 6U; i++)
    {
        frame_data[i + 2U] = data[i];
    }

    context->tx_state = ISOTP_TX_WAIT_FC;

    context->tx_deadline =
        now + isotp_timeout_to_us(context->config.n_bs_timeout_ms);

    if (!isotp_send_frame(context, frame_data, 8U))
    {
        isotp_reset_tx(context);
        return false;
    }

    return true;
}

/* Process received CAN frame. */
void isotp_on_can_frame(
    IsoTpContext *context,
    const IsoTpCanFrame *frame)
{
    uint8_t pci_type;

    if ((context == NULL) ||
        (frame == NULL) ||
        (!context->initialized) ||
        (frame->id != context->config.rx_id) ||
        (frame->dlc > ISOTP_CAN_DLEN) ||
        (frame->dlc == 0U))
    {
        return;
    }

    pci_type = (uint8_t)(frame->data[0] >> 4U);

    /* SINGLE FRAME. */
    if (pci_type == 0U)
    {
        uint8_t length =
            (uint8_t)(frame->data[0] & 0x0FU);

        if ((length == 0U) ||
            (length > 7U) ||
            (frame->dlc < (uint8_t)(length + 1U)))
        {
            return;
        }

        context->config.receive_message(
            context->config.user,
            &frame->data[1],
            length);
    }

    /* FIRST FRAME. */
    else if (pci_type == 1U)
    {
        uint16_t length;
        uint8_t i;

        if (frame->dlc != 8U)
        {
            return;
        }

        length = (uint16_t)(
            ((uint16_t)(frame->data[0] & 0x0FU) << 8U) |
            frame->data[1]);

        if ((length <= 7U) ||
            (length > ISOTP_MAX_PAYLOAD) ||
            (context->rx_state != ISOTP_RX_IDLE))
        {
            return;
        }

        context->rx_length = length;
        context->rx_offset = 6U;
        context->rx_sequence_number = 1U;
        context->rx_block_count = 0U;

        for (i = 0U; i < 6U; i++)
        {
            context->rx_buffer[i] = frame->data[i + 2U];
        }

        context->rx_state = ISOTP_RX_RECEIVING;

        context->rx_deadline =
            isotp_now(context) +
            isotp_timeout_to_us(context->config.n_cr_timeout_ms);

        if (!isotp_send_flow_control(context, 0U))
        {
            isotp_reset_rx(context);
        }
    }

    /* CONSECUTIVE FRAME. */
    else if (pci_type == 2U)
    {
        uint8_t sequence_number;
        uint16_t remaining;
        uint8_t copy_length;
        uint8_t i;

        if ((context->rx_state != ISOTP_RX_RECEIVING) ||
            (frame->dlc < 2U))
        {
            return;
        }

        sequence_number =
            (uint8_t)(frame->data[0] & 0x0FU);

        if (sequence_number != context->rx_sequence_number)
        {
            isotp_reset_rx(context);
            return;
        }

        remaining =
            (uint16_t)(context->rx_length - context->rx_offset);

        copy_length =
            (remaining > 7U) ? 7U : (uint8_t)remaining;

        if (frame->dlc < (uint8_t)(copy_length + 1U))
        {
            isotp_reset_rx(context);
            return;
        }

        for (i = 0U; i < copy_length; i++)
        {
            context->rx_buffer[context->rx_offset + i] =
                frame->data[i + 1U];
        }

        context->rx_offset =
            (uint16_t)(context->rx_offset + copy_length);

        context->rx_sequence_number =
            (uint8_t)((context->rx_sequence_number + 1U) & 0x0FU);

        context->rx_deadline =
            isotp_now(context) +
            isotp_timeout_to_us(context->config.n_cr_timeout_ms);

        context->rx_block_count++;

        if (context->rx_offset >= context->rx_length)
        {
            uint16_t completed_length = context->rx_length;

            context->rx_state = ISOTP_RX_IDLE;
            context->rx_length = 0U;
            context->rx_offset = 0U;
            context->rx_block_count = 0U;

            context->config.receive_message(
                context->config.user,
                context->rx_buffer,
                completed_length);
        }
        else if ((context->config.rx_block_size != 0U) &&
                 (context->rx_block_count >=
                  context->config.rx_block_size))
        {
            context->rx_block_count = 0U;

            if (!isotp_send_flow_control(context, 0U))
            {
                isotp_reset_rx(context);
            }
        }
    }

    /* FLOW CONTROL. */
    else if (pci_type == 3U)
    {
        uint8_t flow_status;

        if ((frame->dlc != 8U) ||
            (context->tx_state != ISOTP_TX_WAIT_FC))
        {
            return;
        }

        flow_status =
            (uint8_t)(frame->data[0] & 0x0FU);

        /* CONTINUE TO SEND. */
        if (flow_status == 0U)
        {
            if (!isotp_is_valid_st_min(frame->data[2]))
            {
                isotp_reset_tx(context);
                return;
            }

            context->tx_block_size = frame->data[1];
            context->tx_block_count = 0U;
            context->tx_wait_count = 0U;
            context->tx_st_min = frame->data[2];

            {
                uint32_t now = isotp_now(context);

                if ((int32_t)(now - context->tx_next_time) >= 0)
                {
                    context->tx_next_time = now;
                }
            }

            context->tx_state = ISOTP_TX_SENDING_CF;
        }

        /* WAIT. */
        else if (flow_status == 1U)
        {
            if (context->tx_wait_count < 0xFFU)
            {
                context->tx_wait_count++;
            }

            if (context->tx_wait_count > context->config.wft_max)
            {
                isotp_reset_tx(context);
                return;
            }

            context->tx_deadline =
                isotp_now(context) +
                isotp_timeout_to_us(context->config.n_bs_timeout_ms);
        }

        /* OVERFLOW or reserved Flow Status. */
        else
        {
            isotp_reset_tx(context);
        }
    }
}

/* Periodic ISO-TP processing. */
void isotp_poll(IsoTpContext *context)
{
    uint32_t now;

    if ((context == NULL) || (!context->initialized))
    {
        return;
    }

    now = isotp_now(context);

    /* N_Cr receiver timeout. */
    if ((context->rx_state == ISOTP_RX_RECEIVING) &&
        ((int32_t)(now - context->rx_deadline) >= 0))
    {
        isotp_reset_rx(context);
    }

    /* N_Bs sender timeout. */
    if ((context->tx_state == ISOTP_TX_WAIT_FC) &&
        ((int32_t)(now - context->tx_deadline) >= 0))
    {
        isotp_reset_tx(context);
    }

    /* TRANSMIT CONSECUTIVE FRAME. */
    if (context->tx_state == ISOTP_TX_SENDING_CF)
    {
        if ((int32_t)(now - context->tx_next_time) < 0)
        {
            return;
        }

        if (context->tx_offset >= context->tx_length)
        {
            isotp_reset_tx(context);
            return;
        }

        {
            uint8_t frame_data[ISOTP_CAN_DLEN] = {0U};
            uint16_t remaining;
            uint8_t copy_length;
            uint8_t i;

            remaining =
                (uint16_t)(context->tx_length - context->tx_offset);

            copy_length =
                (remaining > 7U) ? 7U : (uint8_t)remaining;

            frame_data[0] =
                (uint8_t)(0x20U |
                (context->tx_sequence_number & 0x0FU));

            for (i = 0U; i < copy_length; i++)
            {
                frame_data[i + 1U] =
                    context->tx_buffer[context->tx_offset + i];
            }

            if (!isotp_send_frame(context, frame_data, 8U))
            {
                isotp_reset_tx(context);
                return;
            }

            context->tx_offset =
                (uint16_t)(context->tx_offset + copy_length);

            context->tx_sequence_number =
                (uint8_t)((context->tx_sequence_number + 1U) & 0x0FU);

            context->tx_block_count++;

            if (context->tx_offset >= context->tx_length)
            {
                isotp_reset_tx(context);
                return;
            }

            if ((context->tx_block_size != 0U) &&
                (context->tx_block_count >= context->tx_block_size))
            {
                context->tx_state = ISOTP_TX_WAIT_FC;

                context->tx_deadline =
                    now +
                    isotp_timeout_to_us(context->config.n_bs_timeout_ms);

                return;
            }

            context->tx_next_time =
                now + isotp_st_min_to_us(context->tx_st_min);
        }
    }
}
