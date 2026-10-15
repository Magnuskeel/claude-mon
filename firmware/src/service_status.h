// Polls the public Anthropic status page (no token) in a background task.
#pragma once
#include "state.h"

// Starts the poller; results are copied into `out` under `lock`.
void startServiceStatusTask(ServiceStatus* out, SemaphoreHandle_t lock);
