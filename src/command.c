#include "command_internal.h"

#include <stddef.h>

static const unsigned char PING_COMMAND[] = "PING";
static const unsigned char ECHO_COMMAND[] = "ECHO";
static const unsigned char PONG_REPLY[] = "PONG";
static const unsigned char UNKNOWN_COMMAND_REPLY[] =
    "ERR unknown command";
static const unsigned char PING_WRONG_ARITY_REPLY[] =
    "ERR wrong number of arguments for 'ping' command";
static const unsigned char ECHO_WRONG_ARITY_REPLY[] =
    "ERR wrong number of arguments for 'echo' command";

static unsigned char ascii_fold(unsigned char byte)
{
    if (byte >= (unsigned char) 'A' &&
        byte <= (unsigned char) 'Z') {
        return (unsigned char) (
            byte + ((unsigned char) 'a' - (unsigned char) 'A'));
    }

    return byte;
}

static int command_name_equals(
    const unsigned char *name,
    size_t name_length,
    const unsigned char *expected,
    size_t expected_length)
{
    size_t index;

    if (name_length != expected_length) {
        return 0;
    }

    for (index = 0U; index < name_length; index++) {
        if (ascii_fold(name[index]) != ascii_fold(expected[index])) {
            return 0;
        }
    }

    return 1;
}

static int valid_command_frame(
    const struct minikv_resp2_value *request)
{
    size_t index;

    if (request->type != MINIKV_RESP2_ARRAY ||
        request->as.array.length == 0U ||
        request->as.array.elements == NULL) {
        return 0;
    }

    for (index = 0U; index < request->as.array.length; index++) {
        const struct minikv_resp2_value *element =
            request->as.array.elements[index];

        if (element == NULL ||
            element->type != MINIKV_RESP2_BULK_STRING ||
            (element->as.bytes.length > 0U &&
             element->as.bytes.data == NULL)) {
            return 0;
        }
    }

    return request->as.array.elements[0]->as.bytes.length > 0U;
}

static void set_static_reply(
    struct minikv_command_reply *reply,
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length)
{
    reply->type = type;
    reply->payload = payload;
    reply->payload_length = payload_length;
}

static void set_bulk_reply(
    struct minikv_command_reply *reply,
    const struct minikv_resp2_value *argument)
{
    reply->type = MINIKV_RESP2_ENCODE_BULK_STRING;
    reply->payload = argument->as.bytes.data;
    reply->payload_length = argument->as.bytes.length;
}

static enum minikv_command_dispatch_result dispatch_ping(
    const struct minikv_resp2_value *request,
    struct minikv_command_reply *out_reply)
{
    if (request->as.array.length == 1U) {
        set_static_reply(
            out_reply,
            MINIKV_RESP2_ENCODE_SIMPLE_STRING,
            PONG_REPLY,
            sizeof(PONG_REPLY) - 1U);
    } else if (request->as.array.length == 2U) {
        set_bulk_reply(out_reply, request->as.array.elements[1]);
    } else {
        set_static_reply(
            out_reply,
            MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
            PING_WRONG_ARITY_REPLY,
            sizeof(PING_WRONG_ARITY_REPLY) - 1U);
    }

    return MINIKV_COMMAND_DISPATCH_REPLY;
}

static enum minikv_command_dispatch_result dispatch_echo(
    const struct minikv_resp2_value *request,
    struct minikv_command_reply *out_reply)
{
    if (request->as.array.length == 2U) {
        set_bulk_reply(out_reply, request->as.array.elements[1]);
    } else {
        set_static_reply(
            out_reply,
            MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
            ECHO_WRONG_ARITY_REPLY,
            sizeof(ECHO_WRONG_ARITY_REPLY) - 1U);
    }

    return MINIKV_COMMAND_DISPATCH_REPLY;
}

enum minikv_command_dispatch_result minikv_command_dispatch(
    const struct minikv_resp2_value *request,
    struct minikv_command_reply *out_reply)
{
    const struct minikv_resp2_bytes *command_name;

    if (out_reply != NULL) {
        out_reply->type = MINIKV_RESP2_ENCODE_SIMPLE_STRING;
        out_reply->payload = NULL;
        out_reply->payload_length = 0U;
    }

    if (request == NULL || out_reply == NULL) {
        return MINIKV_COMMAND_DISPATCH_INTERNAL_ERROR;
    }

    if (!valid_command_frame(request)) {
        return MINIKV_COMMAND_DISPATCH_PROTOCOL_ERROR;
    }

    command_name = &request->as.array.elements[0]->as.bytes;

    if (command_name_equals(
            command_name->data,
            command_name->length,
            PING_COMMAND,
            sizeof(PING_COMMAND) - 1U)) {
        return dispatch_ping(request, out_reply);
    }

    if (command_name_equals(
            command_name->data,
            command_name->length,
            ECHO_COMMAND,
            sizeof(ECHO_COMMAND) - 1U)) {
        return dispatch_echo(request, out_reply);
    }

    set_static_reply(
        out_reply,
        MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
        UNKNOWN_COMMAND_REPLY,
        sizeof(UNKNOWN_COMMAND_REPLY) - 1U);
    return MINIKV_COMMAND_DISPATCH_REPLY;
}
