#include "command_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct test_case {
    const char *name;
    int (*run)(void);
};

static int fail(const char *function, int line, const char *expression)
{
    fprintf(stderr,
            "FAILED: %s at line %d: %s\n",
            function,
            line,
            expression);
    return -1;
}

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            status = fail(__func__, __LINE__, #condition);                \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

static int parse_request(
    const unsigned char *wire,
    size_t wire_length,
    struct minikv_resp2_value **out_value)
{
    struct minikv_resp2_parser *parser = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    int status = -1;

    *out_value = NULL;

    if (minikv_resp2_parser_create(&parser, NULL) < 0) {
        goto cleanup;
    }

    result = minikv_resp2_parser_feed(
        parser,
        wire,
        wire_length,
        &consumed,
        out_value);

    if (result != MINIKV_RESP2_VALUE_READY ||
        consumed != wire_length ||
        *out_value == NULL) {
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_parser_destroy(parser);

    if (status != 0) {
        minikv_resp2_value_destroy(*out_value);
        *out_value = NULL;
    }

    return status;
}

static int encode_reply(
    const struct minikv_command_reply *reply,
    unsigned char *destination,
    size_t capacity,
    size_t *out_written)
{
    enum minikv_resp2_encode_error error;

    return minikv_resp2_encode(
        reply->type,
        reply->payload,
        reply->payload_length,
        destination,
        capacity,
        out_written,
        &error);
}

static int dispatch_wire(
    const unsigned char *wire,
    size_t wire_length,
    enum minikv_command_dispatch_result expected_result,
    const unsigned char *expected_response,
    size_t expected_response_length)
{
    struct minikv_resp2_value *value = NULL;
    struct minikv_command_reply reply;
    enum minikv_command_dispatch_result result;
    unsigned char encoded[256];
    size_t written = 0U;
    int status = -1;

    if (parse_request(wire, wire_length, &value) < 0) {
        goto cleanup;
    }

    result = minikv_command_dispatch(value, &reply);

    if (result != expected_result) {
        goto cleanup;
    }

    if (result != MINIKV_COMMAND_DISPATCH_REPLY) {
        if (reply.type != MINIKV_RESP2_ENCODE_SIMPLE_STRING ||
            reply.payload != NULL ||
            reply.payload_length != 0U) {
            goto cleanup;
        }

        status = 0;
        goto cleanup;
    }

    if (expected_response == NULL ||
        encode_reply(
            &reply,
            encoded,
            sizeof(encoded),
            &written) < 0 ||
        written != expected_response_length ||
        memcmp(encoded, expected_response, written) != 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    return status;
}

#define DISPATCH_TEXT(wire, result, response)                             \
    dispatch_wire(                                                        \
        (const unsigned char *) (wire),                                   \
        sizeof(wire) - 1U,                                                \
        (result),                                                         \
        (const unsigned char *) (response),                               \
        sizeof(response) - 1U)

static int test_ping_and_echo(void)
{
    static const unsigned char binary_ping_request[] = {
        '*', '2', '\r', '\n',
        '$', '4', '\r', '\n', 'P', 'I', 'N', 'G', '\r', '\n',
        '$', '5', '\r', '\n', 0x00, '\r', '\n', 0xff, 'x', '\r', '\n'
    };
    static const unsigned char binary_echo_request[] = {
        '*', '2', '\r', '\n',
        '$', '4', '\r', '\n', 'E', 'C', 'H', 'O', '\r', '\n',
        '$', '5', '\r', '\n', 0x00, '\r', '\n', 0xff, 'x', '\r', '\n'
    };
    static const unsigned char binary_response[] = {
        '$', '5', '\r', '\n', 0x00, '\r', '\n', 0xff, 'x', '\r', '\n'
    };
    int status = -1;

    CHECK(DISPATCH_TEXT(
              "*1\r\n$4\r\nPING\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "+PONG\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*1\r\n$4\r\nping\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "+PONG\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*1\r\n$4\r\nPiNg\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "+PONG\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*2\r\n$4\r\nPING\r\n$0\r\n\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "$0\r\n\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "$5\r\nhello\r\n") == 0);
    CHECK(dispatch_wire(
              binary_ping_request,
              sizeof(binary_ping_request),
              MINIKV_COMMAND_DISPATCH_REPLY,
              binary_response,
              sizeof(binary_response)) == 0);
    CHECK(DISPATCH_TEXT(
              "*3\r\n$4\r\nPING\r\n$1\r\na\r\n$1\r\nb\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "-ERR wrong number of arguments for 'ping' command\r\n") == 0);

    CHECK(DISPATCH_TEXT(
              "*1\r\n$4\r\nECHO\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "-ERR wrong number of arguments for 'echo' command\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*2\r\n$4\r\necho\r\n$1\r\nx\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "$1\r\nx\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*2\r\n$4\r\nEcHo\r\n$1\r\nx\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "$1\r\nx\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*2\r\n$4\r\nECHO\r\n$0\r\n\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "$0\r\n\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "$5\r\nhello\r\n") == 0);
    CHECK(dispatch_wire(
              binary_echo_request,
              sizeof(binary_echo_request),
              MINIKV_COMMAND_DISPATCH_REPLY,
              binary_response,
              sizeof(binary_response)) == 0);
    CHECK(DISPATCH_TEXT(
              "*3\r\n$4\r\nECHO\r\n$1\r\na\r\n$1\r\nb\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "-ERR wrong number of arguments for 'echo' command\r\n") == 0);

    status = 0;

cleanup:
    return status;
}

static int test_unknown_commands(void)
{
    static const unsigned char non_ascii[] = {
        '*', '1', '\r', '\n',
        '$', '2', '\r', '\n', 0xc3, 0xa9, '\r', '\n'
    };
    static const unsigned char embedded_nul[] = {
        '*', '1', '\r', '\n',
        '$', '5', '\r', '\n', 'P', 'I', 'N', 'G', 0x00, '\r', '\n'
    };
    static const unsigned char unknown[] = "-ERR unknown command\r\n";
    int status = -1;

    CHECK(DISPATCH_TEXT(
              "*1\r\n$3\r\nGET\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "-ERR unknown command\r\n") == 0);
    CHECK(dispatch_wire(
              non_ascii,
              sizeof(non_ascii),
              MINIKV_COMMAND_DISPATCH_REPLY,
              unknown,
              sizeof(unknown) - 1U) == 0);
    CHECK(dispatch_wire(
              embedded_nul,
              sizeof(embedded_nul),
              MINIKV_COMMAND_DISPATCH_REPLY,
              unknown,
              sizeof(unknown) - 1U) == 0);
    CHECK(DISPATCH_TEXT(
              "*1\r\n$5\r\nPINGX\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "-ERR unknown command\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*1\r\n$3\r\nPIN\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "-ERR unknown command\r\n") == 0);
    CHECK(DISPATCH_TEXT(
              "*1\r\n$4\r\nPING\r\n",
              MINIKV_COMMAND_DISPATCH_REPLY,
              "+PONG\r\n") == 0);

    status = 0;

cleanup:
    return status;
}

static int test_protocol_shapes(void)
{
    static const char *const invalid[] = {
        "+PING\r\n",
        "-PING\r\n",
        ":1\r\n",
        "$4\r\nPING\r\n",
        "*-1\r\n",
        "*0\r\n",
        "*1\r\n$0\r\n\r\n",
        "*1\r\n+PING\r\n",
        "*2\r\n$4\r\nPING\r\n+arg\r\n",
        "*2\r\n$4\r\nPING\r\n:1\r\n",
        "*2\r\n$4\r\nPING\r\n$-1\r\n",
        "*2\r\n$4\r\nPING\r\n*0\r\n",
        "*2\r\n$4\r\nPING\r\n*1\r\n$1\r\nx\r\n",
        "*2\r\n$3\r\nGET\r\n+arg\r\n"
    };
    size_t index;
    int status = -1;

    for (index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]);
         index++) {
        CHECK(dispatch_wire(
                  (const unsigned char *) invalid[index],
                  strlen(invalid[index]),
                  MINIKV_COMMAND_DISPATCH_PROTOCOL_ERROR,
                  NULL,
                  0U) == 0);
    }

    status = 0;

cleanup:
    return status;
}

static int test_internal_error_and_reset(void)
{
    struct minikv_command_reply reply;
    int status = -1;

    reply.type = MINIKV_RESP2_ENCODE_BULK_STRING;
    reply.payload = (const unsigned char *) (uintptr_t) 1U;
    reply.payload_length = SIZE_MAX;

    CHECK(minikv_command_dispatch(NULL, &reply) ==
          MINIKV_COMMAND_DISPATCH_INTERNAL_ERROR);
    CHECK(reply.type == MINIKV_RESP2_ENCODE_SIMPLE_STRING);
    CHECK(reply.payload == NULL);
    CHECK(reply.payload_length == 0U);
    CHECK(minikv_command_dispatch(NULL, NULL) ==
          MINIKV_COMMAND_DISPATCH_INTERNAL_ERROR);

    status = 0;

cleanup:
    return status;
}

static int test_borrowed_payload_and_request_unchanged(void)
{
    static const unsigned char echo_wire[] =
        "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    static const unsigned char ping_wire[] =
        "*2\r\n$4\r\nPING\r\n$5\r\nworld\r\n";
    struct minikv_resp2_value *value = NULL;
    struct minikv_resp2_value *command;
    struct minikv_resp2_value *argument;
    struct minikv_command_reply reply;
    unsigned char encoded[16];
    const unsigned char *command_data;
    const unsigned char *argument_data;
    size_t command_length;
    size_t argument_length;
    size_t written = 0U;
    int status = -1;

    CHECK(parse_request(
              echo_wire,
              sizeof(echo_wire) - 1U,
              &value) == 0);
    CHECK(value->type == MINIKV_RESP2_ARRAY);
    CHECK(value->as.array.length == 2U);

    command = value->as.array.elements[0];
    argument = value->as.array.elements[1];
    command_data = command->as.bytes.data;
    command_length = command->as.bytes.length;
    argument_data = argument->as.bytes.data;
    argument_length = argument->as.bytes.length;

    CHECK(minikv_command_dispatch(value, &reply) ==
          MINIKV_COMMAND_DISPATCH_REPLY);
    CHECK(reply.type == MINIKV_RESP2_ENCODE_BULK_STRING);
    CHECK(reply.payload == argument_data);
    CHECK(reply.payload_length == argument_length);
    CHECK(value->type == MINIKV_RESP2_ARRAY);
    CHECK(value->as.array.length == 2U);
    CHECK(command->type == MINIKV_RESP2_BULK_STRING);
    CHECK(command->as.bytes.data == command_data);
    CHECK(command->as.bytes.length == command_length);
    CHECK(memcmp(command_data, "ECHO", 4U) == 0);
    CHECK(argument->type == MINIKV_RESP2_BULK_STRING);
    CHECK(argument->as.bytes.data == argument_data);
    CHECK(argument->as.bytes.length == argument_length);
    CHECK(memcmp(argument_data, "hello", 5U) == 0);
    CHECK(encode_reply(
              &reply,
              encoded,
              sizeof(encoded),
              &written) == 0);
    CHECK(written == sizeof("$5\r\nhello\r\n") - 1U);
    CHECK(memcmp(
              encoded,
              "$5\r\nhello\r\n",
              written) == 0);

    minikv_resp2_value_destroy(value);
    value = NULL;

    CHECK(parse_request(
              ping_wire,
              sizeof(ping_wire) - 1U,
              &value) == 0);
    CHECK(value->type == MINIKV_RESP2_ARRAY);
    CHECK(value->as.array.length == 2U);
    argument = value->as.array.elements[1];
    argument_data = argument->as.bytes.data;
    argument_length = argument->as.bytes.length;
    CHECK(minikv_command_dispatch(value, &reply) ==
          MINIKV_COMMAND_DISPATCH_REPLY);
    CHECK(reply.type == MINIKV_RESP2_ENCODE_BULK_STRING);
    CHECK(reply.payload == argument_data);
    CHECK(reply.payload_length == argument_length);
    CHECK(encode_reply(
              &reply,
              encoded,
              sizeof(encoded),
              &written) == 0);
    CHECK(written == sizeof("$5\r\nworld\r\n") - 1U);
    CHECK(memcmp(encoded, "$5\r\nworld\r\n", written) == 0);

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    return status;
}

static int test_pipeline(void)
{
    static const unsigned char wire[] =
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n"
        "*1\r\n$3\r\nGET\r\n"
        "*2\r\n$4\r\nPING\r\n$5\r\nworld\r\n";
    static const unsigned char expected[] =
        "+PONG\r\n"
        "$5\r\nhello\r\n"
        "-ERR unknown command\r\n"
        "$5\r\nworld\r\n";
    struct minikv_resp2_parser *parser = NULL;
    unsigned char responses[128];
    size_t input_offset = 0U;
    size_t response_offset = 0U;
    size_t request_count = 0U;
    int status = -1;

    CHECK(minikv_resp2_parser_create(&parser, NULL) == 0);

    while (input_offset < sizeof(wire) - 1U) {
        struct minikv_resp2_value *value = NULL;
        struct minikv_command_reply reply;
        enum minikv_resp2_parse_result parse_result;
        size_t consumed = 0U;
        size_t written = 0U;

        parse_result = minikv_resp2_parser_feed(
            parser,
            wire + input_offset,
            sizeof(wire) - 1U - input_offset,
            &consumed,
            &value);
        CHECK(parse_result == MINIKV_RESP2_VALUE_READY);
        CHECK(value != NULL);
        CHECK(consumed > 0U);
        CHECK(consumed <= sizeof(wire) - 1U - input_offset);
        input_offset += consumed;

        CHECK(minikv_command_dispatch(value, &reply) ==
              MINIKV_COMMAND_DISPATCH_REPLY);
        CHECK(encode_reply(
                  &reply,
                  responses + response_offset,
                  sizeof(responses) - response_offset,
                  &written) == 0);
        response_offset += written;
        request_count++;
        minikv_resp2_value_destroy(value);
    }

    CHECK(input_offset == sizeof(wire) - 1U);
    CHECK(request_count == 4U);
    CHECK(response_offset == sizeof(expected) - 1U);
    CHECK(memcmp(responses, expected, response_offset) == 0);

    status = 0;

cleanup:
    minikv_resp2_parser_destroy(parser);
    return status;
}

int main(void)
{
    static const struct test_case cases[] = {
        {"PING and ECHO", test_ping_and_echo},
        {"unknown commands", test_unknown_commands},
        {"protocol shapes", test_protocol_shapes},
        {"internal error and safe reset", test_internal_error_and_reset},
        {"borrowed payload and request unchanged",
         test_borrowed_payload_and_request_unchanged},
        {"pipeline parser and dispatch", test_pipeline}
    };
    size_t index;
    int failures = 0;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (cases[index].run() != 0) {
            fprintf(stderr, "FAILED: %s\n", cases[index].name);
            failures++;
        } else {
            printf("PASSED: %s\n", cases[index].name);
        }
    }

    return failures == 0 ? 0 : 1;
}
