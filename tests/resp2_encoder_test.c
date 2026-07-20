#include "resp2_internal.h"

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

static int expect_size(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    size_t expected)
{
    enum minikv_resp2_encode_error error =
        MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT;
    size_t size = SIZE_MAX;

    if (minikv_resp2_encoded_size(
            type,
            payload,
            payload_length,
            &size,
            &error) != 0 ||
        size != expected ||
        error != MINIKV_RESP2_ENCODE_ERROR_NONE) {
        return -1;
    }

    return 0;
}

static int expect_encoding(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    const unsigned char *expected,
    size_t expected_length)
{
    unsigned char destination[256];
    enum minikv_resp2_encode_error error =
        MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT;
    size_t required = 0U;
    size_t written = 0U;

    if (expected_length + 2U > sizeof(destination) ||
        minikv_resp2_encoded_size(
            type,
            payload,
            payload_length,
            &required,
            &error) != 0 ||
        required != expected_length ||
        error != MINIKV_RESP2_ENCODE_ERROR_NONE) {
        return -1;
    }

    memset(destination, 0xa5, sizeof(destination));

    if (minikv_resp2_encode(
            type,
            payload,
            payload_length,
            destination + 1U,
            expected_length,
            &written,
            &error) != 0 ||
        written != expected_length ||
        error != MINIKV_RESP2_ENCODE_ERROR_NONE ||
        memcmp(destination + 1U, expected, expected_length) != 0 ||
        destination[0] != 0xa5 ||
        destination[expected_length + 1U] != 0xa5) {
        return -1;
    }

    return 0;
}

static int test_sizes_and_text_output(void)
{
    static const unsigned char pong[] = "PONG";
    static const unsigned char unknown[] = "ERR unknown command";
    static const unsigned char one[] = "x";
    static const unsigned char nine[] = "123456789";
    static const unsigned char ten[] = "0123456789";
    static const unsigned char ninety_nine[99] = {0};
    static const unsigned char one_hundred[100] = {0};
    static const unsigned char empty_simple[] = "+\r\n";
    static const unsigned char pong_response[] = "+PONG\r\n";
    static const unsigned char empty_error[] = "-\r\n";
    static const unsigned char unknown_response[] =
        "-ERR unknown command\r\n";
    static const unsigned char empty_bulk[] = "$0\r\n\r\n";
    static const unsigned char one_bulk[] = "$1\r\nx\r\n";
    static const unsigned char hello_bulk[] = "$5\r\nhello\r\n";
    static const unsigned char ten_bulk[] = "$10\r\n0123456789\r\n";
    int status = -1;

    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              3U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              pong,
              sizeof(pong) - 1U,
              7U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
              NULL,
              0U,
              3U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
              unknown,
              sizeof(unknown) - 1U,
              sizeof(unknown_response) - 1U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              NULL,
              0U,
              6U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              one,
              sizeof(one) - 1U,
              7U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              nine,
              sizeof(nine) - 1U,
              15U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              ten,
              sizeof(ten) - 1U,
              17U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              ninety_nine,
              sizeof(ninety_nine),
              106U) == 0);
    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              one_hundred,
              sizeof(one_hundred),
              108U) == 0);

    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              empty_simple,
              sizeof(empty_simple) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              pong,
              sizeof(pong) - 1U,
              pong_response,
              sizeof(pong_response) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
              NULL,
              0U,
              empty_error,
              sizeof(empty_error) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
              unknown,
              sizeof(unknown) - 1U,
              unknown_response,
              sizeof(unknown_response) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              NULL,
              0U,
              empty_bulk,
              sizeof(empty_bulk) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              one,
              sizeof(one) - 1U,
              one_bulk,
              sizeof(one_bulk) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              (const unsigned char *) "hello",
              5U,
              hello_bulk,
              sizeof(hello_bulk) - 1U) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              ten,
              sizeof(ten) - 1U,
              ten_bulk,
              sizeof(ten_bulk) - 1U) == 0);

    status = 0;

cleanup:
    return status;
}

static int test_binary_bulk(void)
{
    static const unsigned char payload[] = {
        0x00, '\r', '\n', '\r', '\n', 0xff
    };
    static const unsigned char expected[] = {
        '$', '6', '\r', '\n',
        0x00, '\r', '\n', '\r', '\n', 0xff,
        '\r', '\n'
    };
    int status = -1;

    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              payload,
              sizeof(payload),
              sizeof(expected)) == 0);
    CHECK(expect_encoding(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              payload,
              sizeof(payload),
              expected,
              sizeof(expected)) == 0);

    status = 0;

cleanup:
    return status;
}

static int expect_invalid_simple(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length)
{
    unsigned char destination[32];
    unsigned char original[32];
    enum minikv_resp2_encode_error error =
        MINIKV_RESP2_ENCODE_ERROR_NONE;
    size_t size = SIZE_MAX;
    size_t written = SIZE_MAX;

    memset(destination, 0x5a, sizeof(destination));
    memcpy(original, destination, sizeof(destination));

    if (minikv_resp2_encoded_size(
            type,
            payload,
            payload_length,
            &size,
            &error) != -1 ||
        size != 0U ||
        error != MINIKV_RESP2_ENCODE_ERROR_INVALID_PAYLOAD) {
        return -1;
    }

    error = MINIKV_RESP2_ENCODE_ERROR_NONE;

    if (minikv_resp2_encode(
            type,
            payload,
            payload_length,
            destination,
            sizeof(destination),
            &written,
            &error) != -1 ||
        written != 0U ||
        error != MINIKV_RESP2_ENCODE_ERROR_INVALID_PAYLOAD ||
        memcmp(destination, original, sizeof(destination)) != 0) {
        return -1;
    }

    return 0;
}

static int test_simple_payload_validation(void)
{
    static const unsigned char cr[] = {'\r'};
    static const unsigned char lf[] = {'\n'};
    static const unsigned char crlf[] = {'\r', '\n'};
    static const unsigned char middle_cr[] = {'a', '\r', 'b'};
    static const unsigned char middle_lf[] = {'a', '\n', 'b'};
    static const struct {
        const unsigned char *payload;
        size_t length;
    } cases[] = {
        {cr, sizeof(cr)},
        {lf, sizeof(lf)},
        {crlf, sizeof(crlf)},
        {middle_cr, sizeof(middle_cr)},
        {middle_lf, sizeof(middle_lf)}
    };
    static const enum minikv_resp2_encode_type types[] = {
        MINIKV_RESP2_ENCODE_SIMPLE_STRING,
        MINIKV_RESP2_ENCODE_SIMPLE_ERROR
    };
    size_t type_index;
    size_t case_index;
    int status = -1;

    for (type_index = 0U;
         type_index < sizeof(types) / sizeof(types[0]);
         type_index++) {
        for (case_index = 0U;
             case_index < sizeof(cases) / sizeof(cases[0]);
             case_index++) {
            CHECK(expect_invalid_simple(
                      types[type_index],
                      cases[case_index].payload,
                      cases[case_index].length) == 0);
        }
    }

    status = 0;

cleanup:
    return status;
}

static int test_buffer_atomicity(void)
{
    static const unsigned char payload[] = "abc";
    static const enum minikv_resp2_encode_type types[] = {
        MINIKV_RESP2_ENCODE_SIMPLE_STRING,
        MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
        MINIKV_RESP2_ENCODE_BULK_STRING
    };
    unsigned char destination[32];
    unsigned char original[32];
    size_t index;
    int status = -1;

    for (index = 0U;
         index < sizeof(types) / sizeof(types[0]);
         index++) {
        enum minikv_resp2_encode_error error;
        size_t required = 0U;
        size_t written = SIZE_MAX;

        CHECK(minikv_resp2_encoded_size(
                  types[index],
                  payload,
                  sizeof(payload) - 1U,
                  &required,
                  &error) == 0);
        CHECK(required > 0U);

        memset(destination, 0x3c, sizeof(destination));
        memcpy(original, destination, sizeof(destination));
        CHECK(minikv_resp2_encode(
                  types[index],
                  payload,
                  sizeof(payload) - 1U,
                  destination,
                  required - 1U,
                  &written,
                  &error) == -1);
        CHECK(written == 0U);
        CHECK(error == MINIKV_RESP2_ENCODE_ERROR_BUFFER_TOO_SMALL);
        CHECK(memcmp(destination, original, sizeof(destination)) == 0);

        written = SIZE_MAX;
        CHECK(minikv_resp2_encode(
                  types[index],
                  payload,
                  sizeof(payload) - 1U,
                  destination,
                  0U,
                  &written,
                  &error) == -1);
        CHECK(written == 0U);
        CHECK(error == MINIKV_RESP2_ENCODE_ERROR_BUFFER_TOO_SMALL);
        CHECK(memcmp(destination, original, sizeof(destination)) == 0);

        written = 0U;
        CHECK(minikv_resp2_encode(
                  types[index],
                  payload,
                  sizeof(payload) - 1U,
                  destination + 1U,
                  required,
                  &written,
                  &error) == 0);
        CHECK(written == required);
        CHECK(destination[0] == 0x3c);
        CHECK(destination[required + 1U] == 0x3c);
    }

    status = 0;

cleanup:
    return status;
}

static int test_invalid_arguments(void)
{
    unsigned char destination[16];
    enum minikv_resp2_encode_error error =
        MINIKV_RESP2_ENCODE_ERROR_NONE;
    size_t size = SIZE_MAX;
    size_t written = SIZE_MAX;
    int status = -1;

    CHECK(expect_size(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              3U) == 0);
    CHECK(minikv_resp2_encoded_size(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              1U,
              &size,
              &error) == -1);
    CHECK(size == 0U);
    CHECK(error == MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT);

    size = SIZE_MAX;
    error = MINIKV_RESP2_ENCODE_ERROR_NONE;
    CHECK(minikv_resp2_encoded_size(
              (enum minikv_resp2_encode_type) 99,
              NULL,
              0U,
              &size,
              &error) == -1);
    CHECK(size == 0U);
    CHECK(error == MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT);

    CHECK(minikv_resp2_encoded_size(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              NULL,
              &error) == -1);
    CHECK(minikv_resp2_encoded_size(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              &size,
              NULL) == -1);

    CHECK(minikv_resp2_encode(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              NULL,
              0U,
              &written,
              &error) == -1);
    CHECK(written == 0U);
    CHECK(error == MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT);
    CHECK(minikv_resp2_encode(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              destination,
              sizeof(destination),
              NULL,
              &error) == -1);
    CHECK(minikv_resp2_encode(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              NULL,
              0U,
              destination,
              sizeof(destination),
              &written,
              NULL) == -1);

    written = SIZE_MAX;
    error = MINIKV_RESP2_ENCODE_ERROR_NONE;
    CHECK(minikv_resp2_encode(
              (enum minikv_resp2_encode_type) 99,
              NULL,
              0U,
              destination,
              sizeof(destination),
              &written,
              &error) == -1);
    CHECK(written == 0U);
    CHECK(error == MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT);

    status = 0;

cleanup:
    return status;
}

static int expect_overflow(
    enum minikv_resp2_encode_type type,
    size_t payload_length)
{
    unsigned char destination[16];
    unsigned char original[16];
    enum minikv_resp2_encode_error error =
        MINIKV_RESP2_ENCODE_ERROR_NONE;
    const unsigned char *fake_payload =
        (const unsigned char *) (uintptr_t) 1U;
    size_t size = SIZE_MAX;
    size_t written = SIZE_MAX;

    memset(destination, 0x69, sizeof(destination));
    memcpy(original, destination, sizeof(destination));

    if (minikv_resp2_encoded_size(
            type,
            fake_payload,
            payload_length,
            &size,
            &error) != -1 ||
        size != 0U ||
        error != MINIKV_RESP2_ENCODE_ERROR_OVERFLOW) {
        return -1;
    }

    error = MINIKV_RESP2_ENCODE_ERROR_NONE;

    if (minikv_resp2_encode(
            type,
            fake_payload,
            payload_length,
            destination,
            sizeof(destination),
            &written,
            &error) != -1 ||
        written != 0U ||
        error != MINIKV_RESP2_ENCODE_ERROR_OVERFLOW ||
        memcmp(destination, original, sizeof(destination)) != 0) {
        return -1;
    }

    return 0;
}

static int test_overflow(void)
{
    int status = -1;

    CHECK(expect_overflow(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              SIZE_MAX) == 0);
    CHECK(expect_overflow(
              MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
              SIZE_MAX) == 0);
    CHECK(expect_overflow(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              SIZE_MAX) == 0);
    CHECK(expect_overflow(
              MINIKV_RESP2_ENCODE_SIMPLE_STRING,
              SIZE_MAX - 1U) == 0);
    CHECK(expect_overflow(
              MINIKV_RESP2_ENCODE_BULK_STRING,
              SIZE_MAX - 1U) == 0);

    status = 0;

cleanup:
    return status;
}

int main(void)
{
    static const struct test_case cases[] = {
        {"sizes and text output", test_sizes_and_text_output},
        {"binary bulk", test_binary_bulk},
        {"simple payload validation", test_simple_payload_validation},
        {"buffer atomicity", test_buffer_atomicity},
        {"invalid arguments", test_invalid_arguments},
        {"overflow", test_overflow}
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
