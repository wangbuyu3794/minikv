#include "resp2_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int is_valid_encode_type(enum minikv_resp2_encode_type type)
{
    return type == MINIKV_RESP2_ENCODE_SIMPLE_STRING ||
           type == MINIKV_RESP2_ENCODE_SIMPLE_ERROR ||
           type == MINIKV_RESP2_ENCODE_BULK_STRING;
}

static int checked_add(size_t left, size_t right, size_t *out_sum)
{
    if (left > SIZE_MAX - right) {
        return -1;
    }

    *out_sum = left + right;
    return 0;
}

static size_t decimal_digit_count(size_t value)
{
    size_t digits = 1U;

    while (value >= 10U) {
        value /= 10U;
        digits++;
    }

    return digits;
}

static int validate_simple_payload(
    const unsigned char *payload,
    size_t payload_length)
{
    size_t index;

    for (index = 0U; index < payload_length; index++) {
        if (payload[index] == (unsigned char) '\r' ||
            payload[index] == (unsigned char) '\n') {
            return -1;
        }
    }

    return 0;
}

static int calculate_encoded_size(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    size_t *out_size,
    enum minikv_resp2_encode_error *out_error)
{
    size_t required = 1U;

    if (!is_valid_encode_type(type) ||
        (payload == NULL && payload_length > 0U)) {
        *out_error = MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT;
        return -1;
    }

    if (type == MINIKV_RESP2_ENCODE_BULK_STRING) {
        size_t digits = decimal_digit_count(payload_length);

        if (checked_add(required, digits, &required) < 0 ||
            checked_add(required, 2U, &required) < 0 ||
            checked_add(required, payload_length, &required) < 0 ||
            checked_add(required, 2U, &required) < 0) {
            *out_error = MINIKV_RESP2_ENCODE_ERROR_OVERFLOW;
            return -1;
        }
    } else {
        if (checked_add(required, payload_length, &required) < 0 ||
            checked_add(required, 2U, &required) < 0) {
            *out_error = MINIKV_RESP2_ENCODE_ERROR_OVERFLOW;
            return -1;
        }

        if (validate_simple_payload(payload, payload_length) < 0) {
            *out_error = MINIKV_RESP2_ENCODE_ERROR_INVALID_PAYLOAD;
            return -1;
        }
    }

    *out_size = required;
    *out_error = MINIKV_RESP2_ENCODE_ERROR_NONE;
    return 0;
}

static size_t write_decimal_length(
    size_t value,
    unsigned char *destination)
{
    size_t digits = decimal_digit_count(value);
    size_t index = digits;

    do {
        index--;
        destination[index] =
            (unsigned char) ('0' + (unsigned char) (value % 10U));
        value /= 10U;
    } while (index > 0U);

    return digits;
}

int minikv_resp2_encoded_size(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    size_t *out_size,
    enum minikv_resp2_encode_error *out_error)
{
    if (out_size != NULL) {
        *out_size = 0U;
    }

    if (out_error != NULL) {
        *out_error = MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT;
    }

    if (out_size == NULL || out_error == NULL) {
        return -1;
    }

    return calculate_encoded_size(
        type,
        payload,
        payload_length,
        out_size,
        out_error);
}

int minikv_resp2_encode(
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length,
    unsigned char *destination,
    size_t destination_size,
    size_t *out_written,
    enum minikv_resp2_encode_error *out_error)
{
    enum minikv_resp2_encode_error size_error;
    size_t required = 0U;
    size_t cursor = 0U;

    if (out_written != NULL) {
        *out_written = 0U;
    }

    if (out_error != NULL) {
        *out_error = MINIKV_RESP2_ENCODE_ERROR_INVALID_ARGUMENT;
    }

    if (destination == NULL || out_written == NULL || out_error == NULL) {
        return -1;
    }

    if (calculate_encoded_size(
            type,
            payload,
            payload_length,
            &required,
            &size_error) < 0) {
        *out_error = size_error;
        return -1;
    }

    if (destination_size < required) {
        *out_error = MINIKV_RESP2_ENCODE_ERROR_BUFFER_TOO_SMALL;
        return -1;
    }

    if (type == MINIKV_RESP2_ENCODE_SIMPLE_STRING ||
        type == MINIKV_RESP2_ENCODE_SIMPLE_ERROR) {
        destination[cursor++] =
            type == MINIKV_RESP2_ENCODE_SIMPLE_STRING
                ? (unsigned char) '+'
                : (unsigned char) '-';

        if (payload_length > 0U) {
            memcpy(destination + cursor, payload, payload_length);
            cursor += payload_length;
        }
    } else {
        destination[cursor++] = (unsigned char) '$';
        cursor += write_decimal_length(
            payload_length,
            destination + cursor);
        destination[cursor++] = (unsigned char) '\r';
        destination[cursor++] = (unsigned char) '\n';

        if (payload_length > 0U) {
            memcpy(destination + cursor, payload, payload_length);
            cursor += payload_length;
        }
    }

    destination[cursor++] = (unsigned char) '\r';
    destination[cursor++] = (unsigned char) '\n';
    *out_written = cursor;
    *out_error = MINIKV_RESP2_ENCODE_ERROR_NONE;
    return 0;
}
