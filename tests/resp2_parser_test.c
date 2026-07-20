#include "resp2_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct expected_value {
    enum minikv_resp2_type type;
    const unsigned char *bytes;
    size_t bytes_length;
    int64_t integer;
    const struct expected_value *elements;
    size_t element_count;
};

struct parser_case {
    const unsigned char *input;
    size_t input_length;
    struct expected_value expected;
};

struct input_case {
    const unsigned char *input;
    size_t length;
};

struct test_case {
    const char *name;
    int (*run)(void);
};

#define TEXT_INPUT(text)                                                  \
    {(const unsigned char *) (text), sizeof(text) - 1U}

static int report_failure(
    const char *test_name,
    int line,
    const char *expression)
{
    fprintf(stderr,
            "FAILED: %s at line %d: %s\n",
            test_name,
            line,
            expression);
    return -1;
}

#define TEST_CHECK(condition)                                             \
    do {                                                                  \
        if (!(condition)) {                                               \
            (void) report_failure(__func__, __LINE__, #condition);        \
            status = -1;                                                  \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

static int compare_value(
    const struct minikv_resp2_value *actual,
    const struct expected_value *expected)
{
    size_t index;

    if (actual == NULL || expected == NULL ||
        actual->type != expected->type) {
        return -1;
    }

    switch (expected->type) {
    case MINIKV_RESP2_SIMPLE_STRING:
    case MINIKV_RESP2_SIMPLE_ERROR:
    case MINIKV_RESP2_BULK_STRING:
        if (actual->as.bytes.length != expected->bytes_length) {
            return -1;
        }

        if (expected->bytes_length > 0U &&
            (actual->as.bytes.data == NULL ||
             memcmp(actual->as.bytes.data,
                    expected->bytes,
                    expected->bytes_length) != 0)) {
            return -1;
        }

        return 0;

    case MINIKV_RESP2_INTEGER:
        return actual->as.integer == expected->integer ? 0 : -1;

    case MINIKV_RESP2_ARRAY:
        if (actual->as.array.length != expected->element_count) {
            return -1;
        }

        for (index = 0U; index < expected->element_count; index++) {
            if (compare_value(
                    actual->as.array.elements[index],
                    &expected->elements[index]) < 0) {
                return -1;
            }
        }

        return 0;

    case MINIKV_RESP2_NULL_BULK_STRING:
    case MINIKV_RESP2_NULL_ARRAY:
        return 0;
    }

    return -1;
}

static int create_parser(
    struct minikv_resp2_parser **out_parser,
    const struct minikv_resp2_limits *limits)
{
    *out_parser = NULL;

    if (minikv_resp2_parser_create(out_parser, limits) < 0 ||
        *out_parser == NULL ||
        minikv_resp2_parser_error(*out_parser) !=
            MINIKV_RESP2_ERROR_NONE) {
        return -1;
    }

    return 0;
}

static int parse_and_compare(
    const unsigned char *input,
    size_t input_length,
    const struct expected_value *expected,
    const struct minikv_resp2_limits *limits)
{
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_value *value = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    if (create_parser(&parser, limits) < 0) {
        goto cleanup;
    }

    result = minikv_resp2_parser_feed(
        parser,
        input,
        input_length,
        &consumed,
        &value);

    if (result != MINIKV_RESP2_VALUE_READY ||
        consumed != input_length ||
        minikv_resp2_parser_error(parser) !=
            MINIKV_RESP2_ERROR_NONE ||
        compare_value(value, expected) < 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int expect_sticky_error(
    const unsigned char *input,
    size_t input_length,
    enum minikv_resp2_error expected_error,
    const struct minikv_resp2_limits *limits)
{
    static const unsigned char recovery[] = "+\r\n";
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_value *value = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    if (create_parser(&parser, limits) < 0) {
        goto cleanup;
    }

    result = minikv_resp2_parser_feed(
        parser,
        input,
        input_length,
        &consumed,
        &value);

    if (result != MINIKV_RESP2_PARSE_ERROR ||
        value != NULL ||
        minikv_resp2_parser_error(parser) != expected_error) {
        goto cleanup;
    }

    consumed = 99U;
    value = (struct minikv_resp2_value *) (uintptr_t) 1U;
    result = minikv_resp2_parser_feed(
        parser,
        recovery,
        sizeof(recovery) - 1U,
        &consumed,
        &value);

    if (result != MINIKV_RESP2_PARSE_ERROR ||
        consumed != 0U ||
        value != NULL ||
        minikv_resp2_parser_error(parser) != expected_error) {
        goto cleanup;
    }

    minikv_resp2_parser_reset(parser);

    result = minikv_resp2_parser_feed(
        parser,
        recovery,
        sizeof(recovery) - 1U,
        &consumed,
        &value);

    if (result != MINIKV_RESP2_VALUE_READY ||
        consumed != sizeof(recovery) - 1U ||
        value == NULL ||
        value->type != MINIKV_RESP2_SIMPLE_STRING ||
        value->as.bytes.length != 0U ||
        minikv_resp2_parser_error(parser) !=
            MINIKV_RESP2_ERROR_NONE) {
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int expect_need_more(
    const unsigned char *input,
    size_t input_length)
{
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_value *value = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    if (create_parser(&parser, NULL) < 0) {
        goto cleanup;
    }

    result = minikv_resp2_parser_feed(
        parser,
        input,
        input_length,
        &consumed,
        &value);

    if (result != MINIKV_RESP2_NEED_MORE ||
        consumed != input_length ||
        value != NULL ||
        minikv_resp2_parser_error(parser) !=
            MINIKV_RESP2_ERROR_NONE) {
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int test_scalar_values(void)
{
    static const struct parser_case cases[] = {
        {
            (const unsigned char *) "+\r\n",
            sizeof("+\r\n") - 1U,
            {
                MINIKV_RESP2_SIMPLE_STRING,
                (const unsigned char *) "",
                0U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "+OK\r\n",
            sizeof("+OK\r\n") - 1U,
            {
                MINIKV_RESP2_SIMPLE_STRING,
                (const unsigned char *) "OK",
                2U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "+hello world\r\n",
            sizeof("+hello world\r\n") - 1U,
            {
                MINIKV_RESP2_SIMPLE_STRING,
                (const unsigned char *) "hello world",
                11U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "-\r\n",
            sizeof("-\r\n") - 1U,
            {
                MINIKV_RESP2_SIMPLE_ERROR,
                (const unsigned char *) "",
                0U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "-ERR\r\n",
            sizeof("-ERR\r\n") - 1U,
            {
                MINIKV_RESP2_SIMPLE_ERROR,
                (const unsigned char *) "ERR",
                3U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "-ERR example\r\n",
            sizeof("-ERR example\r\n") - 1U,
            {
                MINIKV_RESP2_SIMPLE_ERROR,
                (const unsigned char *) "ERR example",
                11U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) ":0\r\n",
            sizeof(":0\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, 0, NULL, 0U}
        },
        {
            (const unsigned char *) ":+0\r\n",
            sizeof(":+0\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, 0, NULL, 0U}
        },
        {
            (const unsigned char *) ":-0\r\n",
            sizeof(":-0\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, 0, NULL, 0U}
        },
        {
            (const unsigned char *) ":1\r\n",
            sizeof(":1\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U}
        },
        {
            (const unsigned char *) ":-1\r\n",
            sizeof(":-1\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, -1, NULL, 0U}
        },
        {
            (const unsigned char *) ":000123\r\n",
            sizeof(":000123\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, 123, NULL, 0U}
        },
        {
            (const unsigned char *) ":+000123\r\n",
            sizeof(":+000123\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, 123, NULL, 0U}
        },
        {
            (const unsigned char *) ":-000123\r\n",
            sizeof(":-000123\r\n") - 1U,
            {MINIKV_RESP2_INTEGER, NULL, 0U, -123, NULL, 0U}
        },
        {
            (const unsigned char *) ":9223372036854775807\r\n",
            sizeof(":9223372036854775807\r\n") - 1U,
            {
                MINIKV_RESP2_INTEGER,
                NULL,
                0U,
                INT64_MAX,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) ":-9223372036854775808\r\n",
            sizeof(":-9223372036854775808\r\n") - 1U,
            {
                MINIKV_RESP2_INTEGER,
                NULL,
                0U,
                INT64_MIN,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "$0\r\n\r\n",
            sizeof("$0\r\n\r\n") - 1U,
            {
                MINIKV_RESP2_BULK_STRING,
                (const unsigned char *) "",
                0U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "$1\r\na\r\n",
            sizeof("$1\r\na\r\n") - 1U,
            {
                MINIKV_RESP2_BULK_STRING,
                (const unsigned char *) "a",
                1U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "$5\r\nhello\r\n",
            sizeof("$5\r\nhello\r\n") - 1U,
            {
                MINIKV_RESP2_BULK_STRING,
                (const unsigned char *) "hello",
                5U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "$05\r\nhello\r\n",
            sizeof("$05\r\nhello\r\n") - 1U,
            {
                MINIKV_RESP2_BULK_STRING,
                (const unsigned char *) "hello",
                5U,
                0,
                NULL,
                0U
            }
        },
        {
            (const unsigned char *) "$-1\r\n",
            sizeof("$-1\r\n") - 1U,
            {
                MINIKV_RESP2_NULL_BULK_STRING,
                NULL,
                0U,
                0,
                NULL,
                0U
            }
        }
    };
    size_t index;
    int status = 0;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         index++) {
        if (parse_and_compare(
                cases[index].input,
                cases[index].input_length,
                &cases[index].expected,
                NULL) < 0) {
            status = report_failure(
                __func__,
                __LINE__,
                "scalar case");
            break;
        }
    }

    return status;
}

static int test_array_values(void)
{
    static const struct expected_value one_integer_elements[] = {
        {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U}
    };
    static const struct expected_value get_elements[] = {
        {
            MINIKV_RESP2_BULK_STRING,
            (const unsigned char *) "GET",
            3U,
            0,
            NULL,
            0U
        },
        {
            MINIKV_RESP2_BULK_STRING,
            (const unsigned char *) "key",
            3U,
            0,
            NULL,
            0U
        }
    };
    static const struct expected_value mixed_elements[] = {
        {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U},
        {
            MINIKV_RESP2_SIMPLE_STRING,
            (const unsigned char *) "OK",
            2U,
            0,
            NULL,
            0U
        },
        {
            MINIKV_RESP2_NULL_BULK_STRING,
            NULL,
            0U,
            0,
            NULL,
            0U
        }
    };
    static const struct expected_value special_elements[] = {
        {MINIKV_RESP2_NULL_ARRAY, NULL, 0U, 0, NULL, 0U},
        {MINIKV_RESP2_ARRAY, NULL, 0U, 0, NULL, 0U},
        {
            MINIKV_RESP2_BULK_STRING,
            (const unsigned char *) "",
            0U,
            0,
            NULL,
            0U
        }
    };
    static const struct expected_value nested_first[] = {
        {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U},
        {MINIKV_RESP2_INTEGER, NULL, 0U, 2, NULL, 0U}
    };
    static const struct expected_value nested_deepest[] = {
        {
            MINIKV_RESP2_SIMPLE_STRING,
            (const unsigned char *) "OK",
            2U,
            0,
            NULL,
            0U
        }
    };
    static const struct expected_value nested_second[] = {
        {
            MINIKV_RESP2_ARRAY,
            NULL,
            0U,
            0,
            nested_deepest,
            1U
        }
    };
    static const struct expected_value nested_root[] = {
        {
            MINIKV_RESP2_ARRAY,
            NULL,
            0U,
            0,
            nested_first,
            2U
        },
        {
            MINIKV_RESP2_ARRAY,
            NULL,
            0U,
            0,
            nested_second,
            1U
        }
    };
    static const struct parser_case cases[] = {
        {
            (const unsigned char *) "*0\r\n",
            sizeof("*0\r\n") - 1U,
            {MINIKV_RESP2_ARRAY, NULL, 0U, 0, NULL, 0U}
        },
        {
            (const unsigned char *) "*-1\r\n",
            sizeof("*-1\r\n") - 1U,
            {MINIKV_RESP2_NULL_ARRAY, NULL, 0U, 0, NULL, 0U}
        },
        {
            (const unsigned char *) "*1\r\n:1\r\n",
            sizeof("*1\r\n:1\r\n") - 1U,
            {
                MINIKV_RESP2_ARRAY,
                NULL,
                0U,
                0,
                one_integer_elements,
                1U
            }
        },
        {
            (const unsigned char *)
                "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n",
            sizeof("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n") - 1U,
            {
                MINIKV_RESP2_ARRAY,
                NULL,
                0U,
                0,
                get_elements,
                2U
            }
        },
        {
            (const unsigned char *)
                "*3\r\n:1\r\n+OK\r\n$-1\r\n",
            sizeof("*3\r\n:1\r\n+OK\r\n$-1\r\n") - 1U,
            {
                MINIKV_RESP2_ARRAY,
                NULL,
                0U,
                0,
                mixed_elements,
                3U
            }
        },
        {
            (const unsigned char *)
                "*3\r\n*-1\r\n*0\r\n$0\r\n\r\n",
            sizeof("*3\r\n*-1\r\n*0\r\n$0\r\n\r\n") - 1U,
            {
                MINIKV_RESP2_ARRAY,
                NULL,
                0U,
                0,
                special_elements,
                3U
            }
        },
        {
            (const unsigned char *)
                "*2\r\n*2\r\n:1\r\n:2\r\n"
                "*1\r\n*1\r\n+OK\r\n",
            sizeof(
                "*2\r\n*2\r\n:1\r\n:2\r\n"
                "*1\r\n*1\r\n+OK\r\n") - 1U,
            {
                MINIKV_RESP2_ARRAY,
                NULL,
                0U,
                0,
                nested_root,
                2U
            }
        }
    };
    size_t index;
    int status = 0;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         index++) {
        if (parse_and_compare(
                cases[index].input,
                cases[index].input_length,
                &cases[index].expected,
                NULL) < 0) {
            status = report_failure(
                __func__,
                __LINE__,
                "array case");
            break;
        }
    }

    return status;
}

static int test_binary_bulk_values(void)
{
    static const unsigned char payload_nul[] = {0x00};
    static const unsigned char input_nul[] = {
        '$', '1', '\r', '\n', 0x00, '\r', '\n'
    };
    static const unsigned char payload_a_nul_b[] = {
        'A', 0x00, 'B'
    };
    static const unsigned char input_a_nul_b[] = {
        '$', '3', '\r', '\n', 'A', 0x00, 'B', '\r', '\n'
    };
    static const unsigned char payload_cr[] = {'\r'};
    static const unsigned char input_cr[] = {
        '$', '1', '\r', '\n', '\r', '\r', '\n'
    };
    static const unsigned char payload_lf[] = {'\n'};
    static const unsigned char input_lf[] = {
        '$', '1', '\r', '\n', '\n', '\r', '\n'
    };
    static const unsigned char payload_crlf[] = {'\r', '\n'};
    static const unsigned char input_crlf[] = {
        '$', '2', '\r', '\n', '\r', '\n', '\r', '\n'
    };
    static const unsigned char payload_double_crlf[] = {
        '\r', '\n', '\r', '\n'
    };
    static const unsigned char input_double_crlf[] = {
        '$', '4', '\r', '\n',
        '\r', '\n', '\r', '\n',
        '\r', '\n'
    };
    static const unsigned char payload_end_cr[] = {'A', '\r'};
    static const unsigned char input_end_cr[] = {
        '$', '2', '\r', '\n', 'A', '\r', '\r', '\n'
    };
    static const unsigned char payload_end_lf[] = {'A', '\n'};
    static const unsigned char input_end_lf[] = {
        '$', '2', '\r', '\n', 'A', '\n', '\r', '\n'
    };
    static const unsigned char payload_combined[] = {
        'A', 0x00, '\r', '\n', 'B'
    };
    static const unsigned char input_combined[] = {
        '$', '5', '\r', '\n',
        'A', 0x00, '\r', '\n', 'B',
        '\r', '\n'
    };
    static const struct parser_case cases[] = {
        {
            input_nul,
            sizeof(input_nul),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_nul,
                sizeof(payload_nul),
                0,
                NULL,
                0U
            }
        },
        {
            input_a_nul_b,
            sizeof(input_a_nul_b),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_a_nul_b,
                sizeof(payload_a_nul_b),
                0,
                NULL,
                0U
            }
        },
        {
            input_cr,
            sizeof(input_cr),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_cr,
                sizeof(payload_cr),
                0,
                NULL,
                0U
            }
        },
        {
            input_lf,
            sizeof(input_lf),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_lf,
                sizeof(payload_lf),
                0,
                NULL,
                0U
            }
        },
        {
            input_crlf,
            sizeof(input_crlf),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_crlf,
                sizeof(payload_crlf),
                0,
                NULL,
                0U
            }
        },
        {
            input_double_crlf,
            sizeof(input_double_crlf),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_double_crlf,
                sizeof(payload_double_crlf),
                0,
                NULL,
                0U
            }
        },
        {
            input_end_cr,
            sizeof(input_end_cr),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_end_cr,
                sizeof(payload_end_cr),
                0,
                NULL,
                0U
            }
        },
        {
            input_end_lf,
            sizeof(input_end_lf),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_end_lf,
                sizeof(payload_end_lf),
                0,
                NULL,
                0U
            }
        },
        {
            input_combined,
            sizeof(input_combined),
            {
                MINIKV_RESP2_BULK_STRING,
                payload_combined,
                sizeof(payload_combined),
                0,
                NULL,
                0U
            }
        }
    };
    size_t index;
    int status = 0;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         index++) {
        if (parse_and_compare(
                cases[index].input,
                cases[index].input_length,
                &cases[index].expected,
                NULL) < 0) {
            status = report_failure(
                __func__,
                __LINE__,
                "binary bulk case");
            break;
        }
    }

    return status;
}

static int check_all_split_points(
    const unsigned char *input,
    size_t input_length)
{
    size_t split = 0U;
    size_t baseline = minikv_resp2_test_live_allocations();

    for (;;) {
        struct minikv_resp2_parser *parser = NULL;
        struct minikv_resp2_value *value = NULL;
        enum minikv_resp2_parse_result result;
        size_t consumed = 0U;
        int iteration_status = -1;

        if (create_parser(&parser, NULL) < 0) {
            goto iteration_cleanup;
        }

        result = minikv_resp2_parser_feed(
            parser,
            input,
            split,
            &consumed,
            &value);

        if (consumed != split) {
            goto iteration_cleanup;
        }

        if (split == input_length) {
            if (result != MINIKV_RESP2_VALUE_READY ||
                value == NULL) {
                goto iteration_cleanup;
            }
        } else {
            if (result != MINIKV_RESP2_NEED_MORE ||
                value != NULL) {
                goto iteration_cleanup;
            }

            result = minikv_resp2_parser_feed(
                parser,
                input + split,
                input_length - split,
                &consumed,
                &value);

            if (result != MINIKV_RESP2_VALUE_READY ||
                consumed != input_length - split ||
                value == NULL) {
                goto iteration_cleanup;
            }
        }

        iteration_status = 0;

iteration_cleanup:
        minikv_resp2_value_destroy(value);
        minikv_resp2_parser_destroy(parser);

        if (iteration_status != 0 ||
            minikv_resp2_test_live_allocations() != baseline) {
            return -1;
        }

        if (split == input_length) {
            break;
        }

        split++;
    }

    return 0;
}

static int check_one_byte_with_zero_feeds(
    const unsigned char *input,
    size_t input_length)
{
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_value *value = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    size_t index;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    if (create_parser(&parser, NULL) < 0) {
        goto cleanup;
    }

    for (index = 0U; index < input_length; index++) {
        result = minikv_resp2_parser_feed(
            parser,
            NULL,
            0U,
            &consumed,
            &value);

        if (result != MINIKV_RESP2_NEED_MORE ||
            consumed != 0U ||
            value != NULL) {
            goto cleanup;
        }

        result = minikv_resp2_parser_feed(
            parser,
            input + index,
            1U,
            &consumed,
            &value);

        if (consumed != 1U) {
            goto cleanup;
        }

        if (index + 1U < input_length) {
            if (result != MINIKV_RESP2_NEED_MORE ||
                value != NULL) {
                goto cleanup;
            }
        } else if (result != MINIKV_RESP2_VALUE_READY ||
                   value == NULL) {
            goto cleanup;
        }
    }

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int check_continuous_values(
    const unsigned char *input,
    size_t input_length,
    const size_t *value_lengths,
    const enum minikv_resp2_type *types,
    size_t value_count)
{
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_value *value = NULL;
    size_t baseline = minikv_resp2_test_live_allocations();
    size_t offset = 0U;
    size_t index;
    int status = -1;

    if (create_parser(&parser, NULL) < 0) {
        goto cleanup;
    }

    for (index = 0U; index < value_count; index++) {
        enum minikv_resp2_parse_result result;
        size_t consumed = 0U;

        result = minikv_resp2_parser_feed(
            parser,
            input + offset,
            input_length - offset,
            &consumed,
            &value);

        if (result != MINIKV_RESP2_VALUE_READY ||
            consumed != value_lengths[index] ||
            value == NULL ||
            value->type != types[index]) {
            goto cleanup;
        }

        offset += consumed;
        minikv_resp2_value_destroy(value);
        value = NULL;
    }

    if (offset != input_length) {
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int test_incremental_and_consumed(void)
{
    static const unsigned char binary[] = {
        '$', '3', '\r', '\n', 'A', 0x00, '\n', '\r', '\n'
    };
    static const struct {
        const unsigned char *input;
        size_t length;
    } split_cases[] = {
        {
            (const unsigned char *) "+OK\r\n",
            sizeof("+OK\r\n") - 1U
        },
        {
            (const unsigned char *) "-ERR\r\n",
            sizeof("-ERR\r\n") - 1U
        },
        {
            (const unsigned char *) ":-123\r\n",
            sizeof(":-123\r\n") - 1U
        },
        {
            (const unsigned char *) "$5\r\nhello\r\n",
            sizeof("$5\r\nhello\r\n") - 1U
        },
        {
            (const unsigned char *) "$-1\r\n",
            sizeof("$-1\r\n") - 1U
        },
        {
            (const unsigned char *) "$0\r\n\r\n",
            sizeof("$0\r\n\r\n") - 1U
        },
        {
            (const unsigned char *) "*0\r\n",
            sizeof("*0\r\n") - 1U
        },
        {
            (const unsigned char *) "*-1\r\n",
            sizeof("*-1\r\n") - 1U
        },
        {
            (const unsigned char *) "*2\r\n:1\r\n+OK\r\n",
            sizeof("*2\r\n:1\r\n+OK\r\n") - 1U
        },
        {
            (const unsigned char *)
                "*1\r\n*1\r\n$3\r\nhey\r\n",
            sizeof("*1\r\n*1\r\n$3\r\nhey\r\n") - 1U
        },
        {binary, sizeof(binary)}
    };
    static const unsigned char continuous[] =
        "+OK\r\n:1\r\n$3\r\nhey\r\n";
    static const size_t continuous_lengths[] = {5U, 4U, 9U};
    static const enum minikv_resp2_type continuous_types[] = {
        MINIKV_RESP2_SIMPLE_STRING,
        MINIKV_RESP2_INTEGER,
        MINIKV_RESP2_BULK_STRING
    };
    static const unsigned char array_first[] =
        "*1\r\n:1\r\n+NEXT\r\n";
    static const size_t array_first_lengths[] = {8U, 7U};
    static const enum minikv_resp2_type array_first_types[] = {
        MINIKV_RESP2_ARRAY,
        MINIKV_RESP2_SIMPLE_STRING
    };
    static const unsigned char bulk_first[] =
        "$3\r\nhey\r\n:2\r\n";
    static const size_t bulk_first_lengths[] = {9U, 4U};
    static const enum minikv_resp2_type bulk_first_types[] = {
        MINIKV_RESP2_BULK_STRING,
        MINIKV_RESP2_INTEGER
    };
    size_t index;
    int status = 0;

    for (index = 0U;
         index < sizeof(split_cases) / sizeof(split_cases[0]);
         index++) {
        if (check_all_split_points(
                split_cases[index].input,
                split_cases[index].length) < 0) {
            status = report_failure(
                __func__,
                __LINE__,
                "all split points");
            break;
        }
    }

    if (status == 0 &&
        check_one_byte_with_zero_feeds(
            split_cases[9].input,
            split_cases[9].length) < 0) {
        status = report_failure(
            __func__,
            __LINE__,
            "one-byte feeds with zero feeds");
    }

    if (status == 0 &&
        check_continuous_values(
            continuous,
            sizeof(continuous) - 1U,
            continuous_lengths,
            continuous_types,
            sizeof(continuous_lengths) /
                sizeof(continuous_lengths[0])) < 0) {
        status = report_failure(
            __func__,
            __LINE__,
            "continuous scalar values");
    }

    if (status == 0 &&
        check_continuous_values(
            array_first,
            sizeof(array_first) - 1U,
            array_first_lengths,
            array_first_types,
            sizeof(array_first_lengths) /
                sizeof(array_first_lengths[0])) < 0) {
        status = report_failure(
            __func__,
            __LINE__,
            "continuous array first");
    }

    if (status == 0 &&
        check_continuous_values(
            bulk_first,
            sizeof(bulk_first) - 1U,
            bulk_first_lengths,
            bulk_first_types,
            sizeof(bulk_first_lengths) /
                sizeof(bulk_first_lengths[0])) < 0) {
        status = report_failure(
            __func__,
            __LINE__,
            "continuous bulk first");
    }

    return status;
}

static int test_unknown_types_and_crlf(void)
{
    static const unsigned char unknown_inputs[][2] = {
        {'?', 'x'},
        {'_', 'x'},
        {'#', 'x'},
        {',', 'x'},
        {'(', 'x'},
        {'=', 'x'},
        {'!', 'x'},
        {'%', 'x'},
        {'~', 'x'},
        {'>', 'x'},
        {'|', 'x'},
        {'.', 'x'}
    };
    static const struct input_case crlf_errors[] = {
        TEXT_INPUT("+OK\n"),
        TEXT_INPUT("+OK\rX"),
        TEXT_INPUT("-ERR\n"),
        TEXT_INPUT(":1\n"),
        TEXT_INPUT(":1\rX"),
        TEXT_INPUT("$1\n"),
        TEXT_INPUT("$1\rX"),
        TEXT_INPUT("*0\n"),
        TEXT_INPUT("*0\rX"),
        TEXT_INPUT("$1\r\na\n"),
        TEXT_INPUT("$1\r\na\rX")
    };
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_value *value = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    size_t index;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    for (index = 0U;
         index < sizeof(unknown_inputs) /
             sizeof(unknown_inputs[0]);
         index++) {
        if (expect_sticky_error(
                unknown_inputs[index],
                sizeof(unknown_inputs[index]),
                MINIKV_RESP2_ERROR_UNKNOWN_TYPE,
                NULL) < 0) {
            (void) report_failure(
                __func__,
                __LINE__,
                "unknown type");
            goto cleanup;
        }
    }

    for (index = 0U;
        index < sizeof(crlf_errors) / sizeof(crlf_errors[0]);
         index++) {
        if (expect_sticky_error(
                crlf_errors[index].input,
                crlf_errors[index].length,
                MINIKV_RESP2_ERROR_INVALID_CRLF,
                NULL) < 0) {
            (void) report_failure(
                __func__,
                __LINE__,
                "invalid CRLF");
            goto cleanup;
        }
    }

    if (create_parser(&parser, NULL) < 0) {
        goto cleanup;
    }

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "+OK\r",
        sizeof("+OK\r") - 1U,
        &consumed,
        &value);

    TEST_CHECK(result == MINIKV_RESP2_NEED_MORE);
    TEST_CHECK(consumed == sizeof("+OK\r") - 1U);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "\n",
        1U,
        &consumed,
        &value);

    TEST_CHECK(result == MINIKV_RESP2_VALUE_READY);
    TEST_CHECK(consumed == 1U);
    TEST_CHECK(value != NULL);
    minikv_resp2_value_destroy(value);
    value = NULL;

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "$1\r\na\r",
        sizeof("$1\r\na\r") - 1U,
        &consumed,
        &value);

    TEST_CHECK(result == MINIKV_RESP2_NEED_MORE);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "\n",
        1U,
        &consumed,
        &value);

    TEST_CHECK(result == MINIKV_RESP2_VALUE_READY);
    TEST_CHECK(value != NULL);
    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int test_integer_errors(void)
{
    static const struct input_case invalid[] = {
        TEXT_INPUT(":\r\n"),
        TEXT_INPUT(":+\r\n"),
        TEXT_INPUT(":-\r\n"),
        TEXT_INPUT(": 1\r\n"),
        TEXT_INPUT(":1 \r\n"),
        TEXT_INPUT(":1a\r\n"),
        TEXT_INPUT(":++1\r\n"),
        TEXT_INPUT(":--1\r\n"),
        TEXT_INPUT(":+-1\r\n"),
        TEXT_INPUT(":-+1\r\n")
    };
    static const struct input_case overflow[] = {
        TEXT_INPUT(":9223372036854775808\r\n"),
        TEXT_INPUT(":-9223372036854775809\r\n"),
        TEXT_INPUT(":+9223372036854775808\r\n"),
        TEXT_INPUT(":999999999999999999999999999999999999\r\n"),
        TEXT_INPUT(":-999999999999999999999999999999999999\r\n")
    };
    size_t index;

    for (index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]);
         index++) {
        if (expect_sticky_error(
                invalid[index].input,
                invalid[index].length,
                MINIKV_RESP2_ERROR_INVALID_INTEGER,
                NULL) < 0) {
            return report_failure(
                __func__,
                __LINE__,
                "invalid integer");
        }
    }

    for (index = 0U;
         index < sizeof(overflow) / sizeof(overflow[0]);
         index++) {
        if (expect_sticky_error(
                overflow[index].input,
                overflow[index].length,
                MINIKV_RESP2_ERROR_INTEGER_OVERFLOW,
                NULL) < 0) {
            return report_failure(
                __func__,
                __LINE__,
                "integer overflow");
        }
    }

    return 0;
}

static int test_length_errors_and_truncation(void)
{
    static const struct input_case invalid_bulk[] = {
        TEXT_INPUT("$\r\n"),
        TEXT_INPUT("$+\r\n"),
        TEXT_INPUT("$+1\r\n"),
        TEXT_INPUT("$-\r\n"),
        TEXT_INPUT("$-0\r\n"),
        TEXT_INPUT("$-2\r\n"),
        TEXT_INPUT("$1a\r\n"),
        TEXT_INPUT("$ 1\r\n"),
        TEXT_INPUT("$1 \r\n"),
        TEXT_INPUT("$--1\r\n")
    };
    static const struct input_case invalid_array[] = {
        TEXT_INPUT("*\r\n"),
        TEXT_INPUT("*+\r\n"),
        TEXT_INPUT("*+1\r\n"),
        TEXT_INPUT("*-\r\n"),
        TEXT_INPUT("*-0\r\n"),
        TEXT_INPUT("*-2\r\n"),
        TEXT_INPUT("*1a\r\n"),
        TEXT_INPUT("* 1\r\n"),
        TEXT_INPUT("*1 \r\n"),
        TEXT_INPUT("*--1\r\n")
    };
    static const struct input_case overflow[] = {
        TEXT_INPUT("$184467440737095516160\r\n"),
        TEXT_INPUT("*184467440737095516160\r\n")
    };
    static const struct input_case truncated[] = {
        TEXT_INPUT("$5\r\n"),
        TEXT_INPUT("$5\r\nhe"),
        TEXT_INPUT("$5\r\nhello"),
        TEXT_INPUT("$5\r\nhello\r"),
        TEXT_INPUT("*1\r\n"),
        TEXT_INPUT("*2\r\n:1\r\n")
    };
    size_t index;

    for (index = 0U;
         index < sizeof(invalid_bulk) / sizeof(invalid_bulk[0]);
         index++) {
        if (expect_sticky_error(
                invalid_bulk[index].input,
                invalid_bulk[index].length,
                MINIKV_RESP2_ERROR_INVALID_LENGTH,
                NULL) < 0) {
            return report_failure(
                __func__,
                __LINE__,
                "invalid bulk length");
        }
    }

    for (index = 0U;
         index < sizeof(invalid_array) / sizeof(invalid_array[0]);
         index++) {
        if (expect_sticky_error(
                invalid_array[index].input,
                invalid_array[index].length,
                MINIKV_RESP2_ERROR_INVALID_LENGTH,
                NULL) < 0) {
            return report_failure(
                __func__,
                __LINE__,
                "invalid array length");
        }
    }

    for (index = 0U;
         index < sizeof(overflow) / sizeof(overflow[0]);
         index++) {
        if (expect_sticky_error(
                overflow[index].input,
                overflow[index].length,
                MINIKV_RESP2_ERROR_LENGTH_OVERFLOW,
                NULL) < 0) {
            return report_failure(
                __func__,
                __LINE__,
                "length overflow");
        }
    }

    for (index = 0U;
         index < sizeof(truncated) / sizeof(truncated[0]);
         index++) {
        if (expect_need_more(
                truncated[index].input,
                truncated[index].length) < 0) {
            return report_failure(
                __func__,
                __LINE__,
                "truncated input");
        }
    }

    return 0;
}

static int test_limits(void)
{
    static const struct expected_value empty_simple = {
        MINIKV_RESP2_SIMPLE_STRING,
        (const unsigned char *) "",
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value empty_error = {
        MINIKV_RESP2_SIMPLE_ERROR,
        (const unsigned char *) "",
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value one_simple = {
        MINIKV_RESP2_SIMPLE_STRING,
        (const unsigned char *) "a",
        1U,
        0,
        NULL,
        0U
    };
    static const struct expected_value integer_zero = {
        MINIKV_RESP2_INTEGER,
        NULL,
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value empty_bulk = {
        MINIKV_RESP2_BULK_STRING,
        (const unsigned char *) "",
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value bulk_two = {
        MINIKV_RESP2_BULK_STRING,
        (const unsigned char *) "ab",
        2U,
        0,
        NULL,
        0U
    };
    static const struct expected_value empty_array = {
        MINIKV_RESP2_ARRAY,
        NULL,
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value null_array = {
        MINIKV_RESP2_NULL_ARRAY,
        NULL,
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value null_bulk = {
        MINIKV_RESP2_NULL_BULK_STRING,
        NULL,
        0U,
        0,
        NULL,
        0U
    };
    static const struct expected_value two_integer_elements[] = {
        {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U},
        {MINIKV_RESP2_INTEGER, NULL, 0U, 2, NULL, 0U}
    };
    static const struct expected_value array_two = {
        MINIKV_RESP2_ARRAY,
        NULL,
        0U,
        0,
        two_integer_elements,
        2U
    };
    static const struct expected_value one_integer_element[] = {
        {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U}
    };
    static const struct expected_value array_one = {
        MINIKV_RESP2_ARRAY,
        NULL,
        0U,
        0,
        one_integer_element,
        1U
    };
    static const struct expected_value nested_child_elements[] = {
        {MINIKV_RESP2_INTEGER, NULL, 0U, 1, NULL, 0U}
    };
    static const struct expected_value nested_root_elements[] = {
        {
            MINIKV_RESP2_ARRAY,
            NULL,
            0U,
            0,
            nested_child_elements,
            1U
        }
    };
    static const struct expected_value nested_two = {
        MINIKV_RESP2_ARRAY,
        NULL,
        0U,
        0,
        nested_root_elements,
        1U
    };
    static const struct expected_value payload_elements[] = {
        {
            MINIKV_RESP2_SIMPLE_STRING,
            (const unsigned char *) "a",
            1U,
            0,
            NULL,
            0U
        },
        {
            MINIKV_RESP2_BULK_STRING,
            (const unsigned char *) "bc",
            2U,
            0,
            NULL,
            0U
        }
    };
    static const struct expected_value payload_array = {
        MINIKV_RESP2_ARRAY,
        NULL,
        0U,
        0,
        payload_elements,
        2U
    };
    static const struct expected_value null_payload_elements[] = {
        {
            MINIKV_RESP2_NULL_BULK_STRING,
            NULL,
            0U,
            0,
            NULL,
            0U
        },
        {
            MINIKV_RESP2_NULL_ARRAY,
            NULL,
            0U,
            0,
            NULL,
            0U
        },
        {
            MINIKV_RESP2_SIMPLE_STRING,
            (const unsigned char *) "",
            0U,
            0,
            NULL,
            0U
        }
    };
    static const struct expected_value null_payload_array = {
        MINIKV_RESP2_ARRAY,
        NULL,
        0U,
        0,
        null_payload_elements,
        3U
    };
    struct minikv_resp2_limits limits;
    struct minikv_resp2_parser *parser =
        (struct minikv_resp2_parser *) (uintptr_t) 1U;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    minikv_resp2_limits_default(&limits);
    limits.max_line_length = 0U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "+\r\n",
        sizeof("+\r\n") - 1U,
        &empty_simple,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "-\r\n",
        sizeof("-\r\n") - 1U,
        &empty_error,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "+a\r\n",
        sizeof("+a\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    limits.max_line_length = 1U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "+a\r\n",
        sizeof("+a\r\n") - 1U,
        &one_simple,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) ":0\r\n",
        sizeof(":0\r\n") - 1U,
        &integer_zero,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "+ab\r\n",
        sizeof("+ab\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "$-1\r\n",
        sizeof("$-1\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    limits.max_line_length = 2U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "$-1\r\n",
        sizeof("$-1\r\n") - 1U,
        &null_bulk,
        &limits) == 0);

    minikv_resp2_limits_default(&limits);
    limits.max_bulk_length = 0U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "$0\r\n\r\n",
        sizeof("$0\r\n\r\n") - 1U,
        &empty_bulk,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "$1\r\na\r\n",
        sizeof("$1\r\na\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);
    limits.max_bulk_length = 2U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "$2\r\nab\r\n",
        sizeof("$2\r\nab\r\n") - 1U,
        &bulk_two,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "$3\r\nabc\r\n",
        sizeof("$3\r\nabc\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    minikv_resp2_limits_default(&limits);
    limits.max_array_length = 0U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*0\r\n",
        sizeof("*0\r\n") - 1U,
        &empty_array,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*1\r\n:1\r\n",
        sizeof("*1\r\n:1\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);
    limits.max_array_length = 2U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*2\r\n:1\r\n:2\r\n",
        sizeof("*2\r\n:1\r\n:2\r\n") - 1U,
        &array_two,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*3\r\n:1\r\n:2\r\n:3\r\n",
        sizeof("*3\r\n:1\r\n:2\r\n:3\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    minikv_resp2_limits_default(&limits);
    limits.max_nesting_depth = 0U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) ":0\r\n",
        sizeof(":0\r\n") - 1U,
        &integer_zero,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*-1\r\n",
        sizeof("*-1\r\n") - 1U,
        MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*0\r\n",
        sizeof("*0\r\n") - 1U,
        MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*1\r\n:1\r\n",
        sizeof("*1\r\n:1\r\n") - 1U,
        MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
        &limits) == 0);

    limits.max_nesting_depth = 1U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*-1\r\n",
        sizeof("*-1\r\n") - 1U,
        &null_array,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*0\r\n",
        sizeof("*0\r\n") - 1U,
        &empty_array,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*1\r\n:1\r\n",
        sizeof("*1\r\n:1\r\n") - 1U,
        &array_one,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*1\r\n*-1\r\n",
        sizeof("*1\r\n*-1\r\n") - 1U,
        MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*1\r\n*0\r\n",
        sizeof("*1\r\n*0\r\n") - 1U,
        MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
        &limits) == 0);

    limits.max_nesting_depth = 2U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*1\r\n*1\r\n:1\r\n",
        sizeof("*1\r\n*1\r\n:1\r\n") - 1U,
        &nested_two,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*1\r\n*1\r\n*0\r\n",
        sizeof("*1\r\n*1\r\n*0\r\n") - 1U,
        MINIKV_RESP2_ERROR_NESTING_TOO_DEEP,
        &limits) == 0);

    minikv_resp2_limits_default(&limits);
    limits.max_value_nodes = 0U;
    TEST_CHECK(minikv_resp2_parser_create(&parser, &limits) < 0);
    TEST_CHECK(parser == NULL);

    limits.max_value_nodes = 1U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) ":0\r\n",
        sizeof(":0\r\n") - 1U,
        &integer_zero,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*0\r\n",
        sizeof("*0\r\n") - 1U,
        &empty_array,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*-1\r\n",
        sizeof("*-1\r\n") - 1U,
        &null_array,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*1\r\n:1\r\n",
        sizeof("*1\r\n:1\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    limits.max_value_nodes = 3U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "*2\r\n:1\r\n:2\r\n",
        sizeof("*2\r\n:1\r\n:2\r\n") - 1U,
        &array_two,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "*3\r\n:1\r\n:2\r\n:3\r\n",
        sizeof("*3\r\n:1\r\n:2\r\n:3\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    minikv_resp2_limits_default(&limits);
    limits.max_total_payload_bytes = 0U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) ":0\r\n",
        sizeof(":0\r\n") - 1U,
        &integer_zero,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "+\r\n",
        sizeof("+\r\n") - 1U,
        &empty_simple,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "-\r\n",
        sizeof("-\r\n") - 1U,
        &empty_error,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *) "$0\r\n\r\n",
        sizeof("$0\r\n\r\n") - 1U,
        &empty_bulk,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "+a\r\n",
        sizeof("+a\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "-a\r\n",
        sizeof("-a\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *) "$1\r\na\r\n",
        sizeof("$1\r\na\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);
    TEST_CHECK(parse_and_compare(
        (const unsigned char *)
            "*3\r\n$-1\r\n*-1\r\n+\r\n",
        sizeof("*3\r\n$-1\r\n*-1\r\n+\r\n") - 1U,
        &null_payload_array,
        &limits) == 0);

    limits.max_total_payload_bytes = 3U;
    TEST_CHECK(parse_and_compare(
        (const unsigned char *)
            "*2\r\n+a\r\n$2\r\nbc\r\n",
        sizeof("*2\r\n+a\r\n$2\r\nbc\r\n") - 1U,
        &payload_array,
        &limits) == 0);
    limits.max_total_payload_bytes = 2U;
    TEST_CHECK(expect_sticky_error(
        (const unsigned char *)
            "*2\r\n+a\r\n$2\r\nbc\r\n",
        sizeof("*2\r\n+a\r\n$2\r\nbc\r\n") - 1U,
        MINIKV_RESP2_ERROR_LIMIT_EXCEEDED,
        &limits) == 0);

    status = 0;

cleanup:
    minikv_resp2_parser_destroy(parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static int test_lifecycle_and_invalid_arguments(void)
{
    static const unsigned char empty_simple[] = "+\r\n";
    struct minikv_resp2_limits limits;
    struct minikv_resp2_parser *parser = NULL;
    struct minikv_resp2_parser *second_parser = NULL;
    struct minikv_resp2_value *value = NULL;
    enum minikv_resp2_parse_result result;
    size_t consumed = 0U;
    size_t baseline = minikv_resp2_test_live_allocations();
    int status = -1;

    minikv_resp2_limits_default(NULL);
    minikv_resp2_parser_reset(NULL);
    minikv_resp2_parser_destroy(NULL);
    minikv_resp2_value_destroy(NULL);

    TEST_CHECK(minikv_resp2_parser_error(NULL) ==
               MINIKV_RESP2_ERROR_INVALID_ARGUMENT);
    TEST_CHECK(minikv_resp2_parser_create(NULL, NULL) < 0);

    minikv_resp2_limits_default(&limits);
    limits.max_value_nodes = 0U;
    parser = (struct minikv_resp2_parser *) (uintptr_t) 1U;
    TEST_CHECK(minikv_resp2_parser_create(&parser, &limits) < 0);
    TEST_CHECK(parser == NULL);

    TEST_CHECK(create_parser(&parser, NULL) == 0);
    minikv_resp2_parser_reset(parser);
    TEST_CHECK(minikv_resp2_parser_error(parser) ==
               MINIKV_RESP2_ERROR_NONE);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "+O",
        sizeof("+O") - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_NEED_MORE);

    value = (struct minikv_resp2_value *) (uintptr_t) 1U;
    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "X",
        1U,
        NULL,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(value == NULL);
    TEST_CHECK(minikv_resp2_parser_error(parser) ==
               MINIKV_RESP2_ERROR_INVALID_ARGUMENT);

    consumed = 99U;
    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "X",
        1U,
        &consumed,
        NULL);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(consumed == 0U);

    consumed = 99U;
    value = (struct minikv_resp2_value *) (uintptr_t) 1U;
    result = minikv_resp2_parser_feed(
        parser,
        NULL,
        1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(consumed == 0U);
    TEST_CHECK(value == NULL);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "K\r\n",
        sizeof("K\r\n") - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_VALUE_READY);
    TEST_CHECK(value != NULL);
    TEST_CHECK(value->type == MINIKV_RESP2_SIMPLE_STRING);
    TEST_CHECK(value->as.bytes.length == 2U);
    TEST_CHECK(memcmp(value->as.bytes.data, "OK", 2U) == 0);
    TEST_CHECK(minikv_resp2_parser_error(parser) ==
               MINIKV_RESP2_ERROR_NONE);
    minikv_resp2_value_destroy(value);
    value = NULL;

    consumed = 99U;
    value = (struct minikv_resp2_value *) (uintptr_t) 1U;
    result = minikv_resp2_parser_feed(
        NULL,
        empty_simple,
        sizeof(empty_simple) - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(consumed == 0U);
    TEST_CHECK(value == NULL);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "?",
        1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(minikv_resp2_parser_error(parser) ==
               MINIKV_RESP2_ERROR_UNKNOWN_TYPE);

    consumed = 99U;
    value = (struct minikv_resp2_value *) (uintptr_t) 1U;
    result = minikv_resp2_parser_feed(
        parser,
        NULL,
        1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(consumed == 0U);
    TEST_CHECK(value == NULL);
    TEST_CHECK(minikv_resp2_parser_error(parser) ==
               MINIKV_RESP2_ERROR_UNKNOWN_TYPE);

    minikv_resp2_parser_reset(parser);
    TEST_CHECK(minikv_resp2_parser_error(parser) ==
               MINIKV_RESP2_ERROR_NONE);

    result = minikv_resp2_parser_feed(
        parser,
        NULL,
        0U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_NEED_MORE);
    TEST_CHECK(consumed == 0U);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "$5\r\nhe",
        sizeof("$5\r\nhe") - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_NEED_MORE);
    minikv_resp2_parser_reset(parser);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "*2\r\n:1\r\n",
        sizeof("*2\r\n:1\r\n") - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_NEED_MORE);
    minikv_resp2_parser_reset(parser);

    result = minikv_resp2_parser_feed(
        parser,
        (const unsigned char *) "+saved\r\n",
        sizeof("+saved\r\n") - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_VALUE_READY);
    TEST_CHECK(value != NULL);

    minikv_resp2_parser_reset(parser);
    minikv_resp2_parser_destroy(parser);
    parser = NULL;

    TEST_CHECK(value->type == MINIKV_RESP2_SIMPLE_STRING);
    TEST_CHECK(value->as.bytes.length == 5U);
    TEST_CHECK(memcmp(value->as.bytes.data, "saved", 5U) == 0);
    minikv_resp2_value_destroy(value);
    value = NULL;

    minikv_resp2_limits_default(&limits);
    limits.max_bulk_length = 0U;
    TEST_CHECK(create_parser(&second_parser, &limits) == 0);
    limits.max_bulk_length = 10U;

    result = minikv_resp2_parser_feed(
        second_parser,
        (const unsigned char *) "$1\r\na\r\n",
        sizeof("$1\r\na\r\n") - 1U,
        &consumed,
        &value);
    TEST_CHECK(result == MINIKV_RESP2_PARSE_ERROR);
    TEST_CHECK(minikv_resp2_parser_error(second_parser) ==
               MINIKV_RESP2_ERROR_LIMIT_EXCEEDED);

    status = 0;

cleanup:
    minikv_resp2_value_destroy(value);
    minikv_resp2_parser_destroy(parser);
    minikv_resp2_parser_destroy(second_parser);

    if (minikv_resp2_test_live_allocations() != baseline) {
        status = -1;
    }

    return status;
}

static size_t build_nested_bulk(
    unsigned char *buffer,
    size_t capacity)
{
    static const unsigned char array_header[] = "*1\r\n";
    static const unsigned char bulk_header[] = "$80\r\n";
    size_t offset = 0U;
    size_t depth;
    size_t index;

    for (depth = 0U; depth < 10U; depth++) {
        if (capacity - offset < sizeof(array_header) - 1U) {
            return 0U;
        }

        memcpy(
            buffer + offset,
            array_header,
            sizeof(array_header) - 1U);
        offset += sizeof(array_header) - 1U;
    }

    if (capacity - offset < sizeof(bulk_header) - 1U + 82U) {
        return 0U;
    }

    memcpy(
        buffer + offset,
        bulk_header,
        sizeof(bulk_header) - 1U);
    offset += sizeof(bulk_header) - 1U;

    for (index = 0U; index < 80U; index++) {
        buffer[offset++] = (unsigned char) ('a' + (index % 26U));
    }

    buffer[offset++] = (unsigned char) '\r';
    buffer[offset++] = (unsigned char) '\n';
    return offset;
}

static int sweep_oom(
    const unsigned char *input,
    size_t input_length)
{
    size_t attempt;
    size_t baseline = minikv_resp2_test_live_allocations();

    for (attempt = 0U; attempt < 256U; attempt++) {
        struct minikv_resp2_parser *parser = NULL;
        struct minikv_resp2_value *value = NULL;
        enum minikv_resp2_parse_result result;
        size_t consumed = 0U;
        bool completed = false;
        int iteration_status = 0;

        minikv_resp2_test_fail_alloc_after(attempt);

        if (minikv_resp2_parser_create(&parser, NULL) < 0) {
            if (parser != NULL) {
                iteration_status = -1;
            }
        } else {
            result = minikv_resp2_parser_feed(
                parser,
                input,
                input_length,
                &consumed,
                &value);

            if (result == MINIKV_RESP2_VALUE_READY) {
                if (consumed != input_length || value == NULL) {
                    iteration_status = -1;
                } else {
                    completed = true;
                }
            } else if (result != MINIKV_RESP2_PARSE_ERROR ||
                       value != NULL ||
                       minikv_resp2_parser_error(parser) !=
                           MINIKV_RESP2_ERROR_OUT_OF_MEMORY) {
                iteration_status = -1;
            }
        }

        minikv_resp2_value_destroy(value);
        minikv_resp2_parser_destroy(parser);
        minikv_resp2_test_disable_alloc_failures();

        if (iteration_status != 0 ||
            minikv_resp2_test_live_allocations() != baseline) {
            return -1;
        }

        if (completed) {
            return 0;
        }
    }

    return -1;
}

static int test_oom_failpoints(void)
{
    static const unsigned char long_simple[] =
        "+abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
        "abcdefghijklmnopqrstuvwxyz\r\n";
    static const unsigned char simple_error[] = "-ERR\r\n";
    static const unsigned char bulk[] = "$5\r\nhello\r\n";
    unsigned char nested[256];
    size_t nested_length;
    int status = -1;

    nested_length = build_nested_bulk(nested, sizeof(nested));

    if (nested_length == 0U ||
        sweep_oom(
            long_simple,
            sizeof(long_simple) - 1U) < 0 ||
        sweep_oom(
            simple_error,
            sizeof(simple_error) - 1U) < 0 ||
        sweep_oom(
            bulk,
            sizeof(bulk) - 1U) < 0 ||
        sweep_oom(nested, nested_length) < 0) {
        (void) report_failure(
            __func__,
            __LINE__,
            "OOM sweep");
        goto cleanup;
    }

    status = 0;

cleanup:
    minikv_resp2_test_disable_alloc_failures();

    if (minikv_resp2_test_live_allocations() != 0U) {
        status = -1;
    }

    return status;
}

static int run_test(const struct test_case *test)
{
    size_t baseline;
    int result;

    minikv_resp2_test_disable_alloc_failures();
    baseline = minikv_resp2_test_live_allocations();
    result = test->run();
    minikv_resp2_test_disable_alloc_failures();

    if (minikv_resp2_test_live_allocations() != baseline) {
        fprintf(stderr,
                "FAILED: %s leaked wrapped allocations\n",
                test->name);
        return -1;
    }

    if (result != 0) {
        fprintf(stderr, "FAILED: %s\n", test->name);
        return -1;
    }

    printf("PASSED: %s\n", test->name);
    return 0;
}

int main(void)
{
    static const struct test_case tests[] = {
        {"normal scalar values", test_scalar_values},
        {"arrays and nesting", test_array_values},
        {"binary-safe bulk values", test_binary_bulk_values},
        {"incremental input and consumed", test_incremental_and_consumed},
        {"unknown types and CRLF", test_unknown_types_and_crlf},
        {"integer errors", test_integer_errors},
        {"length errors and truncation",
         test_length_errors_and_truncation},
        {"resource limits", test_limits},
        {"lifecycle and invalid arguments",
         test_lifecycle_and_invalid_arguments},
        {"OOM failpoints", test_oom_failpoints}
    };
    size_t index;
    int failures = 0;

    for (index = 0U;
         index < sizeof(tests) / sizeof(tests[0]);
         index++) {
        if (run_test(&tests[index]) < 0) {
            failures++;
        }
    }

    if (failures == 0) {
        printf("M2 RESP2 parser tests passed\n");
        return 0;
    }

    fprintf(stderr, "M2 RESP2 parser tests failed\n");
    return 1;
}
