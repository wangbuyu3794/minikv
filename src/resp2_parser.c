#include "resp2_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum minikv_resp2_parser_state {
    MINIKV_RESP2_STATE_EXPECT_TYPE,
    MINIKV_RESP2_STATE_READ_LINE,
    MINIKV_RESP2_STATE_READ_BULK_PAYLOAD,
    MINIKV_RESP2_STATE_READ_BULK_TRAILING_CR,
    MINIKV_RESP2_STATE_READ_BULK_TRAILING_LF,
    MINIKV_RESP2_STATE_ERROR
};

enum minikv_resp2_line_kind {
    MINIKV_RESP2_LINE_SIMPLE_STRING,
    MINIKV_RESP2_LINE_SIMPLE_ERROR,
    MINIKV_RESP2_LINE_INTEGER,
    MINIKV_RESP2_LINE_BULK_LENGTH,
    MINIKV_RESP2_LINE_ARRAY_LENGTH
};

enum minikv_resp2_step_result {
    MINIKV_RESP2_STEP_CONTINUE,
    MINIKV_RESP2_STEP_VALUE_READY,
    MINIKV_RESP2_STEP_ERROR
};

struct minikv_resp2_array_frame {
    struct minikv_resp2_value *array;
    size_t next_index;
    size_t depth;
};

struct minikv_resp2_parser {
    struct minikv_resp2_limits limits;
    enum minikv_resp2_parser_state state;
    enum minikv_resp2_line_kind line_kind;
    enum minikv_resp2_error last_error;
    enum minikv_resp2_error sticky_error;

    unsigned char *line_buffer;
    size_t line_length;
    size_t line_capacity;
    bool line_has_cr;

    struct minikv_resp2_value *bulk_value;
    size_t bulk_expected;
    size_t bulk_copied;

    struct minikv_resp2_array_frame *frames;
    size_t frame_count;
    size_t frame_capacity;

    struct minikv_resp2_value *root;
    size_t node_count;
    size_t total_payload_bytes;
};

#ifdef MINIKV_RESP2_TESTING

struct minikv_resp2_allocation_control {
    bool failures_enabled;
    size_t successful_before_failure;
    size_t successful_allocations;
    size_t live_allocations;
};

static struct minikv_resp2_allocation_control allocation_control;

static bool allocation_should_fail(void)
{
    return allocation_control.live_allocations == SIZE_MAX ||
           (allocation_control.failures_enabled &&
            allocation_control.successful_allocations >=
                allocation_control.successful_before_failure);
}

static void allocation_succeeded(void)
{
    if (allocation_control.successful_allocations < SIZE_MAX) {
        allocation_control.successful_allocations++;
    }
}

#endif

static void *resp2_malloc(size_t size)
{
    void *result;

#ifdef MINIKV_RESP2_TESTING
    if (allocation_should_fail()) {
        return NULL;
    }
#endif

    result = malloc(size);

#ifdef MINIKV_RESP2_TESTING
    if (result != NULL) {
        allocation_succeeded();
        allocation_control.live_allocations++;
    }
#endif

    return result;
}

static void *resp2_calloc(size_t count, size_t size)
{
    void *result;

    if (count != 0U && size > SIZE_MAX / count) {
        return NULL;
    }

#ifdef MINIKV_RESP2_TESTING
    if (allocation_should_fail()) {
        return NULL;
    }
#endif

    result = calloc(count, size);

#ifdef MINIKV_RESP2_TESTING
    if (result != NULL) {
        allocation_succeeded();
        allocation_control.live_allocations++;
    }
#endif

    return result;
}

static void *resp2_realloc(void *pointer, size_t size)
{
    void *result;

#ifdef MINIKV_RESP2_TESTING
    bool pointer_was_null = pointer == NULL;

#endif

#ifdef MINIKV_RESP2_TESTING
    if (allocation_should_fail()) {
        return NULL;
    }
#endif

    result = realloc(pointer, size);

#ifdef MINIKV_RESP2_TESTING
    if (result != NULL) {
        allocation_succeeded();

        if (pointer_was_null) {
            allocation_control.live_allocations++;
        }
    }
#endif

    return result;
}

static void resp2_free(void *pointer)
{
    if (pointer == NULL) {
        return;
    }

#ifdef MINIKV_RESP2_TESTING
    if (allocation_control.live_allocations > 0U) {
        allocation_control.live_allocations--;
    }
#endif

    free(pointer);
}

static void set_sticky_error(
    struct minikv_resp2_parser *parser,
    enum minikv_resp2_error error)
{
    parser->last_error = error;
    parser->sticky_error = error;
    parser->state = MINIKV_RESP2_STATE_ERROR;
}

static void set_invalid_argument(struct minikv_resp2_parser *parser)
{
    if (parser->sticky_error == MINIKV_RESP2_ERROR_NONE) {
        parser->last_error = MINIKV_RESP2_ERROR_INVALID_ARGUMENT;
    }
}

void minikv_resp2_limits_default(struct minikv_resp2_limits *limits)
{
    if (limits == NULL) {
        return;
    }

    limits->max_line_length = 65536U;
    limits->max_bulk_length = 16777216U;
    limits->max_array_length = 65536U;
    limits->max_nesting_depth = 128U;
    limits->max_value_nodes = 1000000U;
    limits->max_total_payload_bytes = 67108864U;
}

void minikv_resp2_value_destroy(struct minikv_resp2_value *value)
{
    size_t index;

    if (value == NULL) {
        return;
    }

    switch (value->type) {
    case MINIKV_RESP2_SIMPLE_STRING:
    case MINIKV_RESP2_SIMPLE_ERROR:
    case MINIKV_RESP2_BULK_STRING:
        resp2_free(value->as.bytes.data);
        break;

    case MINIKV_RESP2_ARRAY:
        if (value->as.array.elements != NULL) {
            for (index = 0U; index < value->as.array.length; index++) {
                minikv_resp2_value_destroy(
                    value->as.array.elements[index]);
            }
        }

        resp2_free(value->as.array.elements);
        break;

    case MINIKV_RESP2_INTEGER:
    case MINIKV_RESP2_NULL_BULK_STRING:
    case MINIKV_RESP2_NULL_ARRAY:
        break;
    }

    resp2_free(value);
}

static void clear_parser_storage(struct minikv_resp2_parser *parser)
{
    minikv_resp2_value_destroy(parser->root);
    parser->root = NULL;
    parser->bulk_value = NULL;
    parser->bulk_expected = 0U;
    parser->bulk_copied = 0U;

    resp2_free(parser->line_buffer);
    parser->line_buffer = NULL;
    parser->line_length = 0U;
    parser->line_capacity = 0U;
    parser->line_has_cr = false;

    resp2_free(parser->frames);
    parser->frames = NULL;
    parser->frame_count = 0U;
    parser->frame_capacity = 0U;

    parser->node_count = 0U;
    parser->total_payload_bytes = 0U;
}

void minikv_resp2_parser_reset(struct minikv_resp2_parser *parser)
{
    if (parser == NULL) {
        return;
    }

    clear_parser_storage(parser);
    parser->state = MINIKV_RESP2_STATE_EXPECT_TYPE;
    parser->line_kind = MINIKV_RESP2_LINE_SIMPLE_STRING;
    parser->last_error = MINIKV_RESP2_ERROR_NONE;
    parser->sticky_error = MINIKV_RESP2_ERROR_NONE;
}

void minikv_resp2_parser_destroy(struct minikv_resp2_parser *parser)
{
    if (parser == NULL) {
        return;
    }

    clear_parser_storage(parser);
    resp2_free(parser);
}

int minikv_resp2_parser_create(
    struct minikv_resp2_parser **out_parser,
    const struct minikv_resp2_limits *limits)
{
    struct minikv_resp2_limits effective_limits;
    struct minikv_resp2_parser *parser;

    if (out_parser == NULL) {
        return -1;
    }

    *out_parser = NULL;

    if (limits == NULL) {
        minikv_resp2_limits_default(&effective_limits);
    } else {
        effective_limits = *limits;
    }

    if (effective_limits.max_value_nodes == 0U) {
        return -1;
    }

    parser = resp2_calloc(1U, sizeof(*parser));

    if (parser == NULL) {
        return -1;
    }

    parser->limits = effective_limits;
    parser->state = MINIKV_RESP2_STATE_EXPECT_TYPE;
    parser->last_error = MINIKV_RESP2_ERROR_NONE;
    parser->sticky_error = MINIKV_RESP2_ERROR_NONE;
    *out_parser = parser;
    return 0;
}

enum minikv_resp2_error minikv_resp2_parser_error(
    const struct minikv_resp2_parser *parser)
{
    if (parser == NULL) {
        return MINIKV_RESP2_ERROR_INVALID_ARGUMENT;
    }

    if (parser->sticky_error != MINIKV_RESP2_ERROR_NONE) {
        return parser->sticky_error;
    }

    return parser->last_error;
}

static int ensure_line_capacity(
    struct minikv_resp2_parser *parser,
    size_t needed)
{
    unsigned char *new_buffer;
    size_t new_capacity;

    if (needed <= parser->line_capacity) {
        return 0;
    }

    new_capacity = parser->line_capacity == 0U
        ? 64U
        : parser->line_capacity;

    if (new_capacity > parser->limits.max_line_length) {
        new_capacity = parser->limits.max_line_length;
    }

    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = needed;
            break;
        }

        new_capacity *= 2U;

        if (new_capacity > parser->limits.max_line_length) {
            new_capacity = parser->limits.max_line_length;
        }
    }

    if (new_capacity < needed) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LENGTH_OVERFLOW);
        return -1;
    }

    new_buffer = resp2_realloc(parser->line_buffer, new_capacity);

    if (new_buffer == NULL) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_OUT_OF_MEMORY);
        return -1;
    }

    parser->line_buffer = new_buffer;
    parser->line_capacity = new_capacity;
    return 0;
}

static int ensure_frame_capacity(
    struct minikv_resp2_parser *parser,
    size_t needed)
{
    struct minikv_resp2_array_frame *new_frames;
    size_t new_capacity;

    if (needed <= parser->frame_capacity) {
        return 0;
    }

    new_capacity = parser->frame_capacity == 0U
        ? 8U
        : parser->frame_capacity;

    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = needed;
            break;
        }

        new_capacity *= 2U;
    }

    if (new_capacity < needed ||
        new_capacity > SIZE_MAX / sizeof(*new_frames)) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LENGTH_OVERFLOW);
        return -1;
    }

    new_frames = resp2_realloc(
        parser->frames,
        new_capacity * sizeof(*new_frames));

    if (new_frames == NULL) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_OUT_OF_MEMORY);
        return -1;
    }

    parser->frames = new_frames;
    parser->frame_capacity = new_capacity;
    return 0;
}

static int reserve_payload(
    struct minikv_resp2_parser *parser,
    size_t length)
{
    size_t total;

    if (parser->total_payload_bytes > SIZE_MAX - length) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LENGTH_OVERFLOW);
        return -1;
    }

    total = parser->total_payload_bytes + length;

    if (total > parser->limits.max_total_payload_bytes) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LIMIT_EXCEEDED);
        return -1;
    }

    parser->total_payload_bytes = total;
    return 0;
}

static struct minikv_resp2_value *create_value(
    struct minikv_resp2_parser *parser,
    enum minikv_resp2_type type)
{
    struct minikv_resp2_value *value;

    if (parser->node_count >= parser->limits.max_value_nodes) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LIMIT_EXCEEDED);
        return NULL;
    }

    value = resp2_calloc(1U, sizeof(*value));

    if (value == NULL) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_OUT_OF_MEMORY);
        return NULL;
    }

    value->type = type;
    parser->node_count++;
    return value;
}

static void destroy_unadopted_value(
    struct minikv_resp2_parser *parser,
    struct minikv_resp2_value *value)
{
    minikv_resp2_value_destroy(value);

    if (parser->node_count > 0U) {
        parser->node_count--;
    }
}

static int adopt_value(
    struct minikv_resp2_parser *parser,
    struct minikv_resp2_value *value)
{
    if (parser->frame_count == 0U) {
        if (parser->root != NULL) {
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_INVALID_STATE);
            return -1;
        }

        parser->root = value;
        return 0;
    }

    {
        struct minikv_resp2_array_frame *frame =
            &parser->frames[parser->frame_count - 1U];

        if (frame->array == NULL ||
            frame->array->type != MINIKV_RESP2_ARRAY ||
            frame->next_index >= frame->array->as.array.length ||
            frame->array->as.array.elements[frame->next_index] != NULL) {
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_INVALID_STATE);
            return -1;
        }

        frame->array->as.array.elements[frame->next_index] = value;
        frame->next_index++;
    }

    return 0;
}

static enum minikv_resp2_step_result finish_completed_value(
    struct minikv_resp2_parser *parser,
    struct minikv_resp2_value **out_value)
{
    while (parser->frame_count > 0U) {
        struct minikv_resp2_array_frame *frame =
            &parser->frames[parser->frame_count - 1U];

        if (frame->array == NULL ||
            frame->array->type != MINIKV_RESP2_ARRAY ||
            frame->next_index > frame->array->as.array.length) {
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_INVALID_STATE);
            return MINIKV_RESP2_STEP_ERROR;
        }

        if (frame->next_index < frame->array->as.array.length) {
            parser->state = MINIKV_RESP2_STATE_EXPECT_TYPE;
            return MINIKV_RESP2_STEP_CONTINUE;
        }

        parser->frame_count--;
    }

    if (parser->root == NULL) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_INVALID_STATE);
        return MINIKV_RESP2_STEP_ERROR;
    }

    *out_value = parser->root;
    parser->root = NULL;
    parser->bulk_value = NULL;
    parser->bulk_expected = 0U;
    parser->bulk_copied = 0U;
    parser->node_count = 0U;
    parser->total_payload_bytes = 0U;
    parser->state = MINIKV_RESP2_STATE_EXPECT_TYPE;
    return MINIKV_RESP2_STEP_VALUE_READY;
}

static enum minikv_resp2_step_result complete_immediate_value(
    struct minikv_resp2_parser *parser,
    struct minikv_resp2_value *value,
    struct minikv_resp2_value **out_value)
{
    if (adopt_value(parser, value) < 0) {
        destroy_unadopted_value(parser, value);
        return MINIKV_RESP2_STEP_ERROR;
    }

    return finish_completed_value(parser, out_value);
}

static int parse_integer_line(
    const unsigned char *data,
    size_t length,
    int64_t *out_integer,
    enum minikv_resp2_error *out_error)
{
    uint64_t magnitude = 0U;
    uint64_t limit;
    size_t index = 0U;
    bool negative = false;

    if (length == 0U) {
        *out_error = MINIKV_RESP2_ERROR_INVALID_INTEGER;
        return -1;
    }

    if (data[index] == (unsigned char) '+' ||
        data[index] == (unsigned char) '-') {
        negative = data[index] == (unsigned char) '-';
        index++;
    }

    if (index == length) {
        *out_error = MINIKV_RESP2_ERROR_INVALID_INTEGER;
        return -1;
    }

    limit = negative
        ? (uint64_t) INT64_MAX + 1U
        : (uint64_t) INT64_MAX;

    for (; index < length; index++) {
        uint64_t digit;

        if (data[index] < (unsigned char) '0' ||
            data[index] > (unsigned char) '9') {
            *out_error = MINIKV_RESP2_ERROR_INVALID_INTEGER;
            return -1;
        }

        digit = (uint64_t) (data[index] - (unsigned char) '0');

        if (magnitude > (limit - digit) / 10U) {
            *out_error = MINIKV_RESP2_ERROR_INTEGER_OVERFLOW;
            return -1;
        }

        magnitude = magnitude * 10U + digit;
    }

    if (negative) {
        if (magnitude == (uint64_t) INT64_MAX + 1U) {
            *out_integer = INT64_MIN;
        } else {
            *out_integer = -(int64_t) magnitude;
        }
    } else {
        *out_integer = (int64_t) magnitude;
    }

    *out_error = MINIKV_RESP2_ERROR_NONE;
    return 0;
}

static int parse_length_line(
    const unsigned char *data,
    size_t length,
    bool *out_is_null,
    size_t *out_length,
    enum minikv_resp2_error *out_error)
{
    size_t value = 0U;
    size_t index;

    *out_is_null = false;
    *out_length = 0U;

    if (length == 2U &&
        data[0] == (unsigned char) '-' &&
        data[1] == (unsigned char) '1') {
        *out_is_null = true;
        *out_error = MINIKV_RESP2_ERROR_NONE;
        return 0;
    }

    if (length == 0U ||
        data[0] == (unsigned char) '+' ||
        data[0] == (unsigned char) '-') {
        *out_error = MINIKV_RESP2_ERROR_INVALID_LENGTH;
        return -1;
    }

    for (index = 0U; index < length; index++) {
        size_t digit;

        if (data[index] < (unsigned char) '0' ||
            data[index] > (unsigned char) '9') {
            *out_error = MINIKV_RESP2_ERROR_INVALID_LENGTH;
            return -1;
        }

        digit = (size_t) (data[index] - (unsigned char) '0');

        if (value > (SIZE_MAX - digit) / 10U) {
            *out_error = MINIKV_RESP2_ERROR_LENGTH_OVERFLOW;
            return -1;
        }

        value = value * 10U + digit;
    }

    *out_length = value;
    *out_error = MINIKV_RESP2_ERROR_NONE;
    return 0;
}

static enum minikv_resp2_step_result make_bytes_value(
    struct minikv_resp2_parser *parser,
    enum minikv_resp2_type type,
    const unsigned char *data,
    size_t length,
    struct minikv_resp2_value **out_value)
{
    struct minikv_resp2_value *value;

    if (reserve_payload(parser, length) < 0) {
        return MINIKV_RESP2_STEP_ERROR;
    }

    value = create_value(parser, type);

    if (value == NULL) {
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (length > 0U) {
        value->as.bytes.data = resp2_malloc(length);

        if (value->as.bytes.data == NULL) {
            destroy_unadopted_value(parser, value);
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_OUT_OF_MEMORY);
            return MINIKV_RESP2_STEP_ERROR;
        }

        memcpy(value->as.bytes.data, data, length);
    }

    value->as.bytes.length = length;
    return complete_immediate_value(parser, value, out_value);
}

static enum minikv_resp2_step_result make_integer_value(
    struct minikv_resp2_parser *parser,
    const unsigned char *data,
    size_t length,
    struct minikv_resp2_value **out_value)
{
    struct minikv_resp2_value *value;
    enum minikv_resp2_error error;
    int64_t integer;

    if (parse_integer_line(
            data,
            length,
            &integer,
            &error) < 0) {
        set_sticky_error(parser, error);
        return MINIKV_RESP2_STEP_ERROR;
    }

    value = create_value(parser, MINIKV_RESP2_INTEGER);

    if (value == NULL) {
        return MINIKV_RESP2_STEP_ERROR;
    }

    value->as.integer = integer;
    return complete_immediate_value(parser, value, out_value);
}

static enum minikv_resp2_step_result start_bulk_value(
    struct minikv_resp2_parser *parser,
    const unsigned char *data,
    size_t line_length,
    struct minikv_resp2_value **out_value)
{
    struct minikv_resp2_value *value;
    enum minikv_resp2_error error;
    size_t length;
    bool is_null;

    if (parse_length_line(
            data,
            line_length,
            &is_null,
            &length,
            &error) < 0) {
        set_sticky_error(parser, error);
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (is_null) {
        value = create_value(
            parser,
            MINIKV_RESP2_NULL_BULK_STRING);

        if (value == NULL) {
            return MINIKV_RESP2_STEP_ERROR;
        }

        return complete_immediate_value(parser, value, out_value);
    }

    if (length > parser->limits.max_bulk_length) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LIMIT_EXCEEDED);
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (reserve_payload(parser, length) < 0) {
        return MINIKV_RESP2_STEP_ERROR;
    }

    value = create_value(parser, MINIKV_RESP2_BULK_STRING);

    if (value == NULL) {
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (length > 0U) {
        value->as.bytes.data = resp2_malloc(length);

        if (value->as.bytes.data == NULL) {
            destroy_unadopted_value(parser, value);
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_OUT_OF_MEMORY);
            return MINIKV_RESP2_STEP_ERROR;
        }
    }

    value->as.bytes.length = length;

    if (adopt_value(parser, value) < 0) {
        destroy_unadopted_value(parser, value);
        return MINIKV_RESP2_STEP_ERROR;
    }

    parser->bulk_value = value;
    parser->bulk_expected = length;
    parser->bulk_copied = 0U;
    parser->state = length == 0U
        ? MINIKV_RESP2_STATE_READ_BULK_TRAILING_CR
        : MINIKV_RESP2_STATE_READ_BULK_PAYLOAD;
    return MINIKV_RESP2_STEP_CONTINUE;
}

static enum minikv_resp2_step_result start_array_value(
    struct minikv_resp2_parser *parser,
    const unsigned char *data,
    size_t line_length,
    struct minikv_resp2_value **out_value)
{
    struct minikv_resp2_value *value;
    enum minikv_resp2_error error;
    size_t length;
    size_t depth;
    bool is_null;

    if (parse_length_line(
            data,
            line_length,
            &is_null,
            &length,
            &error) < 0) {
        set_sticky_error(parser, error);
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (parser->frame_count == SIZE_MAX) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_NESTING_TOO_DEEP);
        return MINIKV_RESP2_STEP_ERROR;
    }

    depth = parser->frame_count + 1U;

    if (depth > parser->limits.max_nesting_depth) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_NESTING_TOO_DEEP);
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (is_null) {
        value = create_value(parser, MINIKV_RESP2_NULL_ARRAY);

        if (value == NULL) {
            return MINIKV_RESP2_STEP_ERROR;
        }

        return complete_immediate_value(parser, value, out_value);
    }

    if (length > parser->limits.max_array_length) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LIMIT_EXCEEDED);
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (length > SIZE_MAX / sizeof(*value->as.array.elements)) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_LENGTH_OVERFLOW);
        return MINIKV_RESP2_STEP_ERROR;
    }

    value = create_value(parser, MINIKV_RESP2_ARRAY);

    if (value == NULL) {
        return MINIKV_RESP2_STEP_ERROR;
    }

    value->as.array.length = length;

    if (length > 0U) {
        value->as.array.elements = resp2_calloc(
            length,
            sizeof(*value->as.array.elements));

        if (value->as.array.elements == NULL) {
            destroy_unadopted_value(parser, value);
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_OUT_OF_MEMORY);
            return MINIKV_RESP2_STEP_ERROR;
        }

        if (ensure_frame_capacity(
                parser,
                parser->frame_count + 1U) < 0) {
            destroy_unadopted_value(parser, value);
            return MINIKV_RESP2_STEP_ERROR;
        }
    }

    if (adopt_value(parser, value) < 0) {
        destroy_unadopted_value(parser, value);
        return MINIKV_RESP2_STEP_ERROR;
    }

    if (length == 0U) {
        return finish_completed_value(parser, out_value);
    }

    parser->frames[parser->frame_count].array = value;
    parser->frames[parser->frame_count].next_index = 0U;
    parser->frames[parser->frame_count].depth = depth;
    parser->frame_count++;
    parser->state = MINIKV_RESP2_STATE_EXPECT_TYPE;
    return MINIKV_RESP2_STEP_CONTINUE;
}

static enum minikv_resp2_step_result process_completed_line(
    struct minikv_resp2_parser *parser,
    struct minikv_resp2_value **out_value)
{
    enum minikv_resp2_step_result result;

    switch (parser->line_kind) {
    case MINIKV_RESP2_LINE_SIMPLE_STRING:
        result = make_bytes_value(
            parser,
            MINIKV_RESP2_SIMPLE_STRING,
            parser->line_buffer,
            parser->line_length,
            out_value);
        break;

    case MINIKV_RESP2_LINE_SIMPLE_ERROR:
        result = make_bytes_value(
            parser,
            MINIKV_RESP2_SIMPLE_ERROR,
            parser->line_buffer,
            parser->line_length,
            out_value);
        break;

    case MINIKV_RESP2_LINE_INTEGER:
        result = make_integer_value(
            parser,
            parser->line_buffer,
            parser->line_length,
            out_value);
        break;

    case MINIKV_RESP2_LINE_BULK_LENGTH:
        result = start_bulk_value(
            parser,
            parser->line_buffer,
            parser->line_length,
            out_value);
        break;

    case MINIKV_RESP2_LINE_ARRAY_LENGTH:
        result = start_array_value(
            parser,
            parser->line_buffer,
            parser->line_length,
            out_value);
        break;

    default:
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_INVALID_STATE);
        result = MINIKV_RESP2_STEP_ERROR;
        break;
    }

    parser->line_length = 0U;
    parser->line_has_cr = false;
    return result;
}

static int begin_line(
    struct minikv_resp2_parser *parser,
    unsigned char prefix)
{
    switch (prefix) {
    case (unsigned char) '+':
        parser->line_kind = MINIKV_RESP2_LINE_SIMPLE_STRING;
        break;

    case (unsigned char) '-':
        parser->line_kind = MINIKV_RESP2_LINE_SIMPLE_ERROR;
        break;

    case (unsigned char) ':':
        parser->line_kind = MINIKV_RESP2_LINE_INTEGER;
        break;

    case (unsigned char) '$':
        parser->line_kind = MINIKV_RESP2_LINE_BULK_LENGTH;
        break;

    case (unsigned char) '*':
        parser->line_kind = MINIKV_RESP2_LINE_ARRAY_LENGTH;
        break;

    default:
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_UNKNOWN_TYPE);
        return -1;
    }

    parser->line_length = 0U;
    parser->line_has_cr = false;
    parser->state = MINIKV_RESP2_STATE_READ_LINE;
    return 0;
}

enum minikv_resp2_parse_result minikv_resp2_parser_feed(
    struct minikv_resp2_parser *parser,
    const unsigned char *data,
    size_t length,
    size_t *consumed,
    struct minikv_resp2_value **out_value)
{
    size_t index = 0U;

    if (consumed != NULL) {
        *consumed = 0U;
    }

    if (out_value != NULL) {
        *out_value = NULL;
    }

    if (parser == NULL) {
        return MINIKV_RESP2_PARSE_ERROR;
    }

    if (parser->sticky_error != MINIKV_RESP2_ERROR_NONE) {
        return MINIKV_RESP2_PARSE_ERROR;
    }

    if (consumed == NULL || out_value == NULL ||
        (data == NULL && length > 0U)) {
        set_invalid_argument(parser);
        return MINIKV_RESP2_PARSE_ERROR;
    }

    parser->last_error = MINIKV_RESP2_ERROR_NONE;

    if (parser->state == MINIKV_RESP2_STATE_ERROR) {
        set_sticky_error(
            parser,
            MINIKV_RESP2_ERROR_INVALID_STATE);
        return MINIKV_RESP2_PARSE_ERROR;
    }

    while (index < length) {
        enum minikv_resp2_step_result step =
            MINIKV_RESP2_STEP_CONTINUE;

        switch (parser->state) {
        case MINIKV_RESP2_STATE_EXPECT_TYPE:
            if (begin_line(parser, data[index]) < 0) {
                index++;
                step = MINIKV_RESP2_STEP_ERROR;
            } else {
                index++;
            }
            break;

        case MINIKV_RESP2_STATE_READ_LINE:
            if (parser->line_has_cr) {
                unsigned char byte = data[index];

                index++;

                if (byte != (unsigned char) '\n') {
                    set_sticky_error(
                        parser,
                        MINIKV_RESP2_ERROR_INVALID_CRLF);
                    step = MINIKV_RESP2_STEP_ERROR;
                } else {
                    step = process_completed_line(
                        parser,
                        out_value);
                }
            } else if (data[index] == (unsigned char) '\r') {
                parser->line_has_cr = true;
                index++;
            } else if (data[index] == (unsigned char) '\n') {
                index++;
                set_sticky_error(
                    parser,
                    MINIKV_RESP2_ERROR_INVALID_CRLF);
                step = MINIKV_RESP2_STEP_ERROR;
            } else if (parser->line_length >=
                       parser->limits.max_line_length) {
                index++;
                set_sticky_error(
                    parser,
                    MINIKV_RESP2_ERROR_LIMIT_EXCEEDED);
                step = MINIKV_RESP2_STEP_ERROR;
            } else {
                size_t needed = parser->line_length + 1U;

                if (ensure_line_capacity(parser, needed) < 0) {
                    step = MINIKV_RESP2_STEP_ERROR;
                } else {
                    parser->line_buffer[parser->line_length] =
                        data[index];
                    parser->line_length++;
                    index++;
                }
            }
            break;

        case MINIKV_RESP2_STATE_READ_BULK_PAYLOAD:
            if (parser->bulk_value == NULL ||
                parser->bulk_value->type !=
                    MINIKV_RESP2_BULK_STRING ||
                parser->bulk_copied > parser->bulk_expected) {
                set_sticky_error(
                    parser,
                    MINIKV_RESP2_ERROR_INVALID_STATE);
                step = MINIKV_RESP2_STEP_ERROR;
            } else {
                size_t remaining =
                    parser->bulk_expected - parser->bulk_copied;
                size_t available = length - index;
                size_t chunk = remaining < available
                    ? remaining
                    : available;

                if (chunk > 0U) {
                    memcpy(
                        parser->bulk_value->as.bytes.data +
                            parser->bulk_copied,
                        data + index,
                        chunk);
                    parser->bulk_copied += chunk;
                    index += chunk;
                }

                if (parser->bulk_copied ==
                    parser->bulk_expected) {
                    parser->state =
                        MINIKV_RESP2_STATE_READ_BULK_TRAILING_CR;
                }
            }
            break;

        case MINIKV_RESP2_STATE_READ_BULK_TRAILING_CR:
            if (data[index] != (unsigned char) '\r') {
                index++;
                set_sticky_error(
                    parser,
                    MINIKV_RESP2_ERROR_INVALID_CRLF);
                step = MINIKV_RESP2_STEP_ERROR;
            } else {
                index++;
                parser->state =
                    MINIKV_RESP2_STATE_READ_BULK_TRAILING_LF;
            }
            break;

        case MINIKV_RESP2_STATE_READ_BULK_TRAILING_LF:
            if (data[index] != (unsigned char) '\n') {
                index++;
                set_sticky_error(
                    parser,
                    MINIKV_RESP2_ERROR_INVALID_CRLF);
                step = MINIKV_RESP2_STEP_ERROR;
            } else {
                index++;
                parser->bulk_value = NULL;
                parser->bulk_expected = 0U;
                parser->bulk_copied = 0U;
                step = finish_completed_value(
                    parser,
                    out_value);
            }
            break;

        case MINIKV_RESP2_STATE_ERROR:
        default:
            set_sticky_error(
                parser,
                MINIKV_RESP2_ERROR_INVALID_STATE);
            step = MINIKV_RESP2_STEP_ERROR;
            break;
        }

        if (step == MINIKV_RESP2_STEP_ERROR) {
            *consumed = index;
            *out_value = NULL;
            return MINIKV_RESP2_PARSE_ERROR;
        }

        if (step == MINIKV_RESP2_STEP_VALUE_READY) {
            *consumed = index;
            return MINIKV_RESP2_VALUE_READY;
        }
    }

    *consumed = length;
    return MINIKV_RESP2_NEED_MORE;
}

#ifdef MINIKV_RESP2_TESTING

void minikv_resp2_test_fail_alloc_after(
    size_t successful_allocations)
{
    allocation_control.failures_enabled = true;
    allocation_control.successful_before_failure =
        successful_allocations;
    allocation_control.successful_allocations = 0U;
}

void minikv_resp2_test_disable_alloc_failures(void)
{
    allocation_control.failures_enabled = false;
    allocation_control.successful_before_failure = 0U;
    allocation_control.successful_allocations = 0U;
}

size_t minikv_resp2_test_live_allocations(void)
{
    return allocation_control.live_allocations;
}

#endif
