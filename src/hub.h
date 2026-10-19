#pragma once

#include <cstdint>

#include "core.h"

// The hub's whole state. One global: it is far too big for the 8 KB loop stack.
struct HubState {
    // Allocated once in setup() and never freed: static memory can't fit it next to Wi-Fi.
    construct::Node* node;
    construct::Totals totals;
    construct::GaussianNB model;
    construct::SyncEngine sync;
    construct::RawCodec codec;
    bool model_dirty;           // slots changed since the model was last computed
    uint32_t unsaved_mask;      // bit i: slot i changed but is not in flash yet
    uint32_t unsaved_since_ms;
    uint32_t led_off_at_ms;
};

extern HubState hub;

// Recompute totals and the model if any slot changed since last time.
void hub_refresh_model();

// Short LED flash, e.g. after teaching.
void hub_blink();
