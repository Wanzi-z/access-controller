#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "wiegand_format.h"

static void complement(const char *input, char *output, size_t output_size)
{
    size_t len = strlen(input);
    assert(output_size > len);
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] == '0' ? '1' : '0';
    }
    output[len] = '\0';
}

static void make_wiegand26(unsigned payload, char output[27])
{
    char data[25];
    unsigned first_ones = 0;
    unsigned second_ones = 0;
    for (int i = 23; i >= 0; i--) {
        char bit = (payload & (1U << i)) ? '1' : '0';
        data[23 - i] = bit;
        if (bit == '1') {
            if (i >= 12) first_ones++;
            else second_ones++;
        }
    }
    data[24] = '\0';
    output[0] = (first_ones & 1U) ? '1' : '0';
    memcpy(output + 1, data, 24);
    output[25] = (second_ones & 1U) ? '0' : '1';
    output[26] = '\0';
}

static void assert_normalizes_to(const char *raw,
                                 const char *expected,
                                 wiegand_code_polarity_t expected_polarity)
{
    char actual[80];
    wiegand_code_polarity_t polarity = WIEGAND_CODE_POLARITY_UNKNOWN;
    assert(wiegand_code_normalize(raw, actual, sizeof(actual), &polarity));
    assert(strcmp(actual, expected) == 0);
    assert(polarity == expected_polarity);
}

int main(void)
{
    const char *live_normal = "00001111001100000111010111";
    const char *live_inverted = "11110000110011111000101000";
    assert_normalizes_to(live_normal, live_normal, WIEGAND_CODE_POLARITY_NORMAL);
    assert_normalizes_to(live_inverted, live_normal, WIEGAND_CODE_POLARITY_INVERTED);

    const char *normal34 = "1000100100011010001010110011110001";
    const char *inverted34 = "0111011011100101110101001100001110";
    assert_normalizes_to(normal34, normal34, WIEGAND_CODE_POLARITY_NORMAL);
    assert_normalizes_to(inverted34, normal34, WIEGAND_CODE_POLARITY_INVERTED);

    /* Property: every generated valid 26-bit frame and its complement agree. */
    for (unsigned payload = 0; payload < (1U << 24); payload += 7919U) {
        char normal[27];
        char inverted[27];
        make_wiegand26(payload, normal);
        complement(normal, inverted, sizeof(inverted));
        assert_normalizes_to(normal, normal, WIEGAND_CODE_POLARITY_NORMAL);
        assert_normalizes_to(inverted, normal, WIEGAND_CODE_POLARITY_INVERTED);
    }

    /* Unknown mixed-bit formats are preserved; guessing could merge badges. */
    const char *unknown37 = "1010101010101010101010101010101010101";
    assert_normalizes_to(unknown37, unknown37, WIEGAND_CODE_POLARITY_UNKNOWN);

    char output[80];
    /* Exact malformed frames observed on the live controller must be rejected. */
    assert(!wiegand_code_normalize("0000000000000000000000000000000", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("00000000000000000000000000000000", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("000000000000000000000000000000000000000000000", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("0000000000000000000000000000000000000000000000", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("0000000000000000000000000000000000000000000000000", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("11111111111111111111111111", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("01", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("01010101010101010101010101010101010101010101010101010101010", output, sizeof(output), NULL));

    /* Standard-length frames need valid parity in one polarity. */
    assert(!wiegand_code_normalize("01000000000001000000000000", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize("10102", output, sizeof(output), NULL));
    assert(!wiegand_code_normalize(live_normal, output, 26, NULL));

    puts("wiegand-format: all tests passed");
    return 0;
}
