#include "wiegand_format.h"

#include <string.h>

static bool segment_has_odd_parity(const char *bits, size_t start, size_t end)
{
    size_t ones = 0;
    for (size_t i = start; i < end; i++) {
        if (bits[i] == '1') {
            ones++;
        }
    }
    return (ones & 1U) != 0;
}

static bool standard_parity_is_valid(const char *bits, size_t bit_len)
{
    size_t half_len;
    if (bit_len == 26) {
        half_len = 13;
    } else if (bit_len == 34) {
        half_len = 17;
    } else {
        return false;
    }

    bool first_half_odd = segment_has_odd_parity(bits, 0, half_len);
    bool second_half_odd = segment_has_odd_parity(bits, half_len, bit_len);
    return !first_half_odd && second_half_odd;
}

bool wiegand_code_normalize(const char *raw,
                            char *normalized,
                            size_t normalized_size,
                            wiegand_code_polarity_t *polarity_out)
{
    if (polarity_out) {
        *polarity_out = WIEGAND_CODE_POLARITY_UNKNOWN;
    }
    if (!raw || !normalized) {
        return false;
    }

    size_t bit_len = strlen(raw);
    if (bit_len < 24 || bit_len > 58 || normalized_size <= bit_len) {
        return false;
    }
    bool has_zero = false;
    bool has_one = false;
    for (size_t i = 0; i < bit_len; i++) {
        if (raw[i] == '0') {
            has_zero = true;
        } else if (raw[i] == '1') {
            has_one = true;
        } else {
            return false;
        }
    }
    if (!has_zero || !has_one) {
        return false;
    }

    memcpy(normalized, raw, bit_len + 1);
    if (bit_len != 26 && bit_len != 34) {
        return true;
    }

    if (standard_parity_is_valid(raw, bit_len)) {
        if (polarity_out) {
            *polarity_out = WIEGAND_CODE_POLARITY_NORMAL;
        }
        return true;
    }

    for (size_t i = 0; i < bit_len; i++) {
        normalized[i] = raw[i] == '0' ? '1' : '0';
    }
    normalized[bit_len] = '\0';

    if (standard_parity_is_valid(normalized, bit_len)) {
        if (polarity_out) {
            *polarity_out = WIEGAND_CODE_POLARITY_INVERTED;
        }
        return true;
    }

    memcpy(normalized, raw, bit_len + 1);
    return false;
}
