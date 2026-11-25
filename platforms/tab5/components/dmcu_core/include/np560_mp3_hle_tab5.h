/*
 * np560_mp3_hle_tab5.h - ROM 01/02 MP3 HLE gate adapter for the Tab5 port.
 *
 * Mirrors QEMU's np560_mp3_hle_service() contract: a deliberately planted
 * 16-byte marker (TRAP#0, RETS-far, "MPLH", version, service index) in the
 * reconstructed resident ROM hole. Without the adapter the interpreter takes
 * TRAP#0, lands on the 01:0000 vector (also 0F00) and self-traps forever.
 */
#ifndef NP560_MP3_HLE_TAB5_H
#define NP560_MP3_HLE_TAB5_H

#include <stdint.h>
#include <stdbool.h>
#include "dmcu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NP560Mp3HleTab5 NP560Mp3HleTab5;

/* Validate the gate markers at `base` and create the service state.
 * Returns NULL when the code ROM does not carry the expected versioned
 * marker (then the caller must leave the TRAP path untouched). */
NP560Mp3HleTab5 *np560_mp3_hle_tab5_create(Np560Bus *bus, uint32_t base);
void np560_mp3_hle_tab5_destroy(NP560Mp3HleTab5 *hle);

/* bus->rom_service hook. `pc` is the current instruction address, which has
 * already been decoded as TRAP#0. Returns true when the gate was serviced;
 * the interpreter then leaves pc at the following RETS-far instruction. */
bool np560_mp3_hle_tab5_service(void *user, DMCUCpu *cpu, uint32_t pc);

/* Diagnostic counter: number of gate services performed. */
uint64_t np560_mp3_hle_tab5_call_count(const NP560Mp3HleTab5 *hle);

#ifdef __cplusplus
}
#endif

#endif /* NP560_MP3_HLE_TAB5_H */
