#pragma once
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdbool.h>

#include "cJSON.h"
#include "fdt.h"

typedef enum {IDLE, CAPTURING_SEQUENCE} state;

void quitLogger();
void setLoggerState(state s);
stopToolFunc startLogger();
void stopLogger();
void doStartLogger();
void l_waitToAdvance();
bool l_canAdvance();
void l_advance();
void l_advancePending();
void handleLoggerEvent(cJSON * event);
