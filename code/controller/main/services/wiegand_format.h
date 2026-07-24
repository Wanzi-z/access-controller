#ifndef WIEGAND_FORMAT_H
#define WIEGAND_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIEGAND_CODE_POLARITY_UNKNOWN = 0,
    WIEGAND_CODE_POLARITY_NORMAL,
    WIEGAND_CODE_POLARITY_INVERTED,
} wiegand_code_polarity_t;

/*
 * Copy a binary Wiegand frame into canonical DATA0/DATA1 polarity.
 *
 * Standard 26- and 34-bit frames carry an even leading parity half and an
 * odd trailing parity half. Swapping DATA0 and DATA1 complements every bit,
 * including both parity bits, so the valid orientation is unambiguous. Other
 * mixed-bit frame lengths are preserved because their parity layout is
 * reader-specific.
 *
 * This controller accepts the reader's configurable 24-58 bit range. A
 * credential frame must also contain at least one DATA0 and one DATA1 pulse.
 * Homogeneous frames cannot identify a card and usually mean one data line is
 * missing; accepting them would turn pulse count/noise into a credential.
 * Standard-length frames with invalid parity are rejected for the same reason.
 */
bool wiegand_code_normalize(const char *raw,
                            char *normalized,
                            size_t normalized_size,
                            wiegand_code_polarity_t *polarity_out);

#endif
