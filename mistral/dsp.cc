/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Cyclone V DSP (Variable-Precision DSP Block) -- minimal 18x18 multiply support (G7).
 *
 *  Ground truth: two Quartus builds of the same design differing only in operand values place the
 *  DSP at different tiles yet emit BYTE-IDENTICAL configuration -- so every setting below is
 *  structural for a combinational unsigned 18x18 multiply, none is design-derived. The Quartus
 *  build uses UNK_IN[64..93] + UNK_IN[120..125] as the 36 operand bits and RESULT[0..35] as the
 *  product (libmistral never named the DSP data inputs, hence UNK_IN).
 *
 *  The A/B-bit -> UNK_IN-index permutation IS silicon-derived -- see the table's comment. The
 *  probe also decoded DATA_INV: the ground truth's odd per-lane masks are inverters Quartus set to
 *  cancel ITS OWN inverted routing polarities (they spell the reference design's init values 12345
 *  and 6789 across 9-bit lanes). Our routing does not invert, so used lanes get 0.
 */
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// the 36 input indices the ground-truth build drives, in provisional order: A[0..17], B[0..17]
// SILICON-DERIVED (dspprobe pair/single scan, 2026-08-09): A[i] = UNK_IN[64+i]; B[0..11] =
// UNK_IN[82..93]; B[12..17] = UNK_IN[125..120] -- the last six lanes are REVERSED. Confirmed by
// driving single bits and reading the raw product: each index's delta is +/- K_other * 2^weight.
static const int dsp_in_idx[36] = {64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75,
                                   76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87,
                                   88, 89, 90, 91, 92, 93, 125, 124, 123, 122, 121, 120};

void Arch::create_dsp(int x, int y)
{
    BelId bel = add_bel(x, y, id_MISTRAL_DSP, id_MISTRAL_MUL18X18);
    auto pos = CycloneV::xy2pos(x, y);
    for (int i = 0; i < 18; i++) {
        if (has_port(CycloneV::DSP, x, y, -1, CycloneV::UNK_IN, dsp_in_idx[i]))
            add_bel_pin(bel, idf("A[%d]", i), PORT_IN,
                        get_port(CycloneV::DSP, x, y, -1, CycloneV::UNK_IN, dsp_in_idx[i]));
        if (has_port(CycloneV::DSP, x, y, -1, CycloneV::UNK_IN, dsp_in_idx[18 + i]))
            add_bel_pin(bel, idf("B[%d]", i), PORT_IN,
                        get_port(CycloneV::DSP, x, y, -1, CycloneV::UNK_IN, dsp_in_idx[18 + i]));
    }
    for (int i = 0; i < 36; i++)
        if (has_port(CycloneV::DSP, x, y, -1, CycloneV::RESULT, i))
            add_bel_pin(bel, idf("Y[%d]", i), PORT_OUT, get_port(CycloneV::DSP, x, y, -1, CycloneV::RESULT, i));
    (void)pos;
}

NEXTPNR_NAMESPACE_END
