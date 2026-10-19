#pragma once

// Web page and JSON API served on the access point (http://192.168.4.1).
//   GET  /        drawing page
//   POST /teach   label=N&pixels=64 comma-separated values 0-16
//   POST /guess   pixels=...
//   GET  /status  versions, peers, per-digit knowledge by room, sync counters

void web_begin();
void web_handle();
