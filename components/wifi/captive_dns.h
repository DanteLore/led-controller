#pragma once

#include <stdbool.h>

// Tiny DNS server for the setup hotspot: answers every name with the board's
// own address, so whatever a phone tries to load lands on the setup page.
// Starts disabled; while disabled, queries are ignored.
void captive_dns_start(const char *ip);
void captive_dns_enable(bool on);
