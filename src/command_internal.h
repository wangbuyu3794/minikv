#ifndef MINIKV_COMMAND_INTERNAL_H_INCLUDED
#define MINIKV_COMMAND_INTERNAL_H_INCLUDED

#include <stddef.h>

#include "resp2_internal.h"

enum minikv_command_dispatch_result {
    MINIKV_COMMAND_DISPATCH_REPLY,
    MINIKV_COMMAND_DISPATCH_PROTOCOL_ERROR,
    MINIKV_COMMAND_DISPATCH_INTERNAL_ERROR
};

struct minikv_command_reply {
    enum minikv_resp2_encode_type type;
    const unsigned char *payload;
    size_t payload_length;
};

enum minikv_command_dispatch_result minikv_command_dispatch(
    const struct minikv_resp2_value *request,
    struct minikv_command_reply *out_reply);

#endif
