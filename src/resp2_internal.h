#ifndef MINIKV_RESP2_INTERNAL_H_INCLUDED
#define MINIKV_RESP2_INTERNAL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

enum minikv_resp2_type {
    MINIKV_RESP2_SIMPLE_STRING,
    MINIKV_RESP2_SIMPLE_ERROR,
    MINIKV_RESP2_INTEGER,
    MINIKV_RESP2_BULK_STRING,
    MINIKV_RESP2_NULL_BULK_STRING,
    MINIKV_RESP2_ARRAY,
    MINIKV_RESP2_NULL_ARRAY
};

struct minikv_resp2_bytes {
    unsigned char *data;
    size_t length;
};

struct minikv_resp2_value;

struct minikv_resp2_array {
    struct minikv_resp2_value **elements;
    size_t length;
};

struct minikv_resp2_value {
    enum minikv_resp2_type type;

    union {
        struct minikv_resp2_bytes bytes;
        int64_t integer;
        struct minikv_resp2_array array;
    } as;
};

struct minikv_resp2_limits {
    size_t max_line_length;
    size_t max_bulk_length;
    size_t max_array_length;
    size_t max_nesting_depth;
    size_t max_value_nodes;
    size_t max_total_payload_bytes;
};

struct minikv_resp2_parser;

enum minikv_resp2_parse_result {
    MINIKV_RESP2_NEED_MORE,
    MINIKV_RESP2_VALUE_READY,
    MINIKV_RESP2_PARSE_ERROR
};

enum minikv_resp2_error {
    MINIKV_RESP2_ERROR_NONE,
    MINIKV_RESP2_ERROR_INVALID_ARGUMENT,
    MINIKV_RESP2_ERROR_UNKNOWN_TYPE,
    MINIKV_RESP2_ERROR_INVALID_CRLF,
    MINIKV_RESP2_ERROR_INVALID_INTEGER,
    MINIKV_RESP2_ERROR_INTEGER_OVERFLOW,
    MINIKV_RESP2_ERROR_INVALID_LENGTH,
    MINIKV_RESP2_ERROR_LENGTH_OVERFLOW,
    MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
    MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
    MINIKV_RESP2_ERROR_OUT_OF_MEMORY,
    MINIKV_RESP2_ERROR_INVALID_STATE
};

enum minikv_resp2_encode_type {
    MINIKV_RESP2_ENCODE_SIMPLE_STRING,
    MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
    MINIKV_RESP2_ENCODE_BULK_STRING
};

enum minikv_resp2_encode_error {
    MINIKV_RESP2_ENCODE_ERROR_NONE,
    MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT,
    MINIKV_RESP2_ENCODE_ERROR_INVALID_PAYLOAD,
    MINIKV_RESP2_ENCODE_ERROR_OVERFLOW,
    MINIKV_RESP2_ENCODE_ERROR_BUFFER_TOO_SMALL
};

void minikv_resp2_limits_default(
    struct minikv_resp2_limits *limits);

int minikv_resp2_parser_create(
    struct minikv_resp2_parser **out_parser,
    const struct minikv_resp2_limits *limits);

enum minikv_resp2_parse_result minikv_resp2_parser_feed(
    struct minikv_resp2_parser *parser,
    const unsigned char *data,
    size_t length,
    size_t *consumed,
    struct minikv_resp2_value **out_value);

enum minikv_resp2_error minikv_resp2_parser_error(
    const struct minikv_resp2_parser *parser);

void minikv_resp2_parser_reset(
    struct minikv_resp2_parser *parser);

void minikv_resp2_parser_destroy(
    struct minikv_resp2_parser *parser);

void minikv_resp2_value_destroy(
    struct minikv_resp2_value *value);

int minikv_resp2_encoded_size(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    size_t *out_size,
    enum minikv_resp2_encode_error *out_error);

int minikv_resp2_encode(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    unsigned char *destination,
    size_t destination_size,
    size_t *out_written,
    enum minikv_resp2_encode_error *out_error);

#ifdef MINIKV_RESP2_TESTING

void minikv_resp2_test_fail_alloc_after(
    size_t successful_allocations);

void minikv_resp2_test_disable_alloc_failures(void);

size_t minikv_resp2_test_live_allocations(void);

#endif

#endif
