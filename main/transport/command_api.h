#pragma once
#include <stddef.h>
typedef void (*command_notify_fn_t)(void);
void command_api_set_notify(command_notify_fn_t fn);
int command_api_execute_line(const char *line, char *response, size_t response_len);
int command_api_execute_json(const char *json, char *response, size_t response_len);
int command_api_get_state_json(char *response, size_t response_len);
