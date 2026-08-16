#include "ptp_tx_registry.h"

/* C++ per the rules in Src/cpp_support.cpp: no allocation, no exceptions, extern "C" at the
   boundary. The storage is the caller's struct, so this adds nothing to any pool. */

namespace {

PtpTxRegistryEntry *find(PtpTxRegistry *reg, const void *key) {
    for (unsigned i = 0; i < PTP_TX_REGISTRY_CAPACITY; i++) {
        if (reg->entries[i].key == key) {
            return &reg->entries[i];
        }
    }
    return nullptr;
}

}  // namespace

extern "C" {

void ptp_tx_registry_reset(PtpTxRegistry *reg) {
    for (unsigned i = 0; i < PTP_TX_REGISTRY_CAPACITY; i++) {
        reg->entries[i].key = nullptr;
        reg->entries[i].uid = 0;
        reg->entries[i].age = 0;
    }
    reg->overflows = 0;
    reg->registrations = 0;
    reg->stale = 0;
}

bool ptp_tx_registry_add(PtpTxRegistry *reg, const void *key, uint32_t uid) {
    if (key == nullptr) {
        return false;
    }

    reg->registrations++;

    /* Evict anything that cannot still be in flight.
     *
     * Not a correctness mechanism -- the three rules in the header are -- but a bound on how
     * long a violation of them stays invisible, and the counter that says one happened. See
     * PTP_TX_REGISTRY_MAX_AGE. */
    for (unsigned i = 0; i < PTP_TX_REGISTRY_CAPACITY; i++) {
        PtpTxRegistryEntry *e = &reg->entries[i];
        if ((e->key != nullptr) && ((reg->registrations - e->age) > PTP_TX_REGISTRY_MAX_AGE)) {
            e->key = nullptr;
            reg->stale++;
        }
    }

    /* Overwrite an existing entry for the same address rather than adding a second. Two live
       entries for one key could be taken in either order, which is precisely the mispairing
       this table exists to prevent. */
    PtpTxRegistryEntry *slot = find(reg, key);
    if (slot == nullptr) {
        slot = find(reg, nullptr);
    }
    if (slot == nullptr) {
        reg->overflows++;
        return false;
    }

    slot->key = key;
    slot->uid = uid;
    slot->age = reg->registrations;
    return true;
}

void ptp_tx_registry_cancel(PtpTxRegistry *reg, const void *key) {
    if (key == nullptr) {
        return;
    }
    PtpTxRegistryEntry *slot = find(reg, key);
    if (slot != nullptr) {
        slot->key = nullptr;
    }
}

bool ptp_tx_registry_take(PtpTxRegistry *reg, const void *key, uint32_t *uid) {
    if (key == nullptr) {
        return false;
    }
    PtpTxRegistryEntry *slot = find(reg, key);
    if (slot == nullptr) {
        return false;
    }

    *uid = slot->uid;
    slot->key = nullptr;  /* freed here, before the caller does anything with the uid */
    return true;
}

unsigned ptp_tx_registry_count(const PtpTxRegistry *reg) {
    unsigned n = 0;
    for (unsigned i = 0; i < PTP_TX_REGISTRY_CAPACITY; i++) {
        if (reg->entries[i].key != nullptr) {
            n++;
        }
    }
    return n;
}

}  // extern "C"
