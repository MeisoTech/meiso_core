#include "meiso_four_kind_lab_protocol_private.h"

/* Transcript ordering owns the fixed host/edge state-machine progression. */

#include <string.h>

void meiso_lab_order_init(MeisoLabOrder *order)
{
    if (order != NULL) {
        memset(order, 0, sizeof(*order));
    }
}

static bool action_identity_matches(const MeisoLabOrder *order,
                                    MeisoActionId id, MeisoOwnerScope executor)
{
    return meiso_action_id_equal(order->action_id, id) &&
           meiso_owner_scope_equal(order->action_executor, executor);
}

MeisoLabOrderStatus meiso_lab_order_advance(MeisoLabOrder *order,
                                            MeisoLabSender sender,
                                            const MeisoLabMessage *message)
{
    size_t ignored_size;
    bool matches = false;

    if (order == NULL || message == NULL ||
        (sender != MEISO_LAB_SENDER_HOST && sender != MEISO_LAB_SENDER_EDGE)) {
        return MEISO_LAB_ORDER_INVALID_ARGUMENT;
    }
    if (order->terminal != 0U) {
        return MEISO_LAB_ORDER_ALREADY_TERMINAL;
    }
    if (!meiso_four_kind_lab_protocol_private_message_size_and_valid(
            message, &ignored_size)) {
        order->terminal = 2U;
        return MEISO_LAB_ORDER_UNEXPECTED;
    }
    switch (order->step) {
    case 0U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
                  message->type == MEISO_LAB_MESSAGE_REGISTRY;
        break;
    case 1U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
                  message->type == MEISO_LAB_MESSAGE_REGISTRY_ACCEPT;
        break;
    case 2U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
                  message->type == MEISO_LAB_MESSAGE_STATE_ACTUAL;
        if (matches) {
            order->state_owner = message->body.state_actual.owner;
            order->state_version = message->body.state_actual.version;
            order->window_id = message->body.state_actual.value.window_id;
        }
        break;
    case 3U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
                  message->type == MEISO_LAB_MESSAGE_STATE_ACCEPT &&
                  meiso_owner_scope_equal(order->state_owner,
                                          message->body.state_accept.owner) &&
                  order->state_version == message->body.state_accept.version;
        break;
    case 4U:
        matches =
            sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_STREAM_METRICS &&
            meiso_owner_scope_equal(order->state_owner,
                                    message->body.stream_metrics.producer) &&
            order->window_id == message->body.stream_metrics.value.window_id;
        break;
    case 5U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
                  message->type == MEISO_LAB_MESSAGE_ACTION_RESET &&
                  meiso_owner_scope_equal(order->state_owner,
                                          message->body.action_reset.executor);
        if (matches) {
            order->action_id = message->body.action_reset.id;
            order->action_executor = message->body.action_reset.executor;
        }
        break;
    case 6U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
                  message->type == MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT &&
                  action_identity_matches(
                      order, message->body.action_delivery_accept.id,
                      message->body.action_delivery_accept.executor);
        break;
    case 7U:
        matches =
            sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_ACTION_RESULT &&
            message->body.action_result.state == MEISO_ACTION_STATE_ACCEPTED &&
            action_identity_matches(order, message->body.action_result.id,
                                    message->body.action_result.executor);
        break;
    case 8U:
        matches =
            sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_ACTION_RESULT &&
            message->body.action_result.state == MEISO_ACTION_STATE_SUCCEEDED &&
            message->body.action_result.new_window_id != order->window_id &&
            action_identity_matches(order, message->body.action_result.id,
                                    message->body.action_result.executor);
        break;
    case 9U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
                  message->type == MEISO_LAB_MESSAGE_BLOB_MANIFEST;
        break;
    case 10U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
                  message->type == MEISO_LAB_MESSAGE_BLOB_ACCEPT;
        break;
    case 11U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
                  message->type == MEISO_LAB_MESSAGE_CLOSE &&
                  message->body.close.status == MEISO_LAB_CLOSE_PASS;
        break;
    default:
        break;
    }

    if (!matches) {
        order->terminal = 2U;
        return MEISO_LAB_ORDER_UNEXPECTED;
    }
    order->step = (uint8_t)(order->step + 1U);
    if (order->step == 12U) {
        order->terminal = 1U;
        return MEISO_LAB_ORDER_COMPLETE;
    }
    return MEISO_LAB_ORDER_ADVANCED;
}
