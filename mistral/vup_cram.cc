// Raw CRAM access for the G4b reference-clock spine.
//
// The spine bits that make the PLL lock are unattributed: they produce no bmux difference and no
// change in route_all_active_links(), so nothing in libmistral's public API can express them. They
// must be written directly. libmistral keeps `cram` and `di` private, and bitstream.cc cannot use
// the `#define private public` trick because it includes the whole nextpnr header set first -- so
// the access is isolated here, in a translation unit that includes ONLY cyclonev.h.
#define private public
#include "cyclonev.h"
#undef private

extern "C" unsigned vup_cram_sx(void *cvp)
{
    return static_cast<mistral::CycloneV *>(cvp)->di.cram_sx;
}

// Returns 1 if the bit changed, 0 if it already held the requested value.
extern "C" int vup_cram_set(void *cvp, unsigned x, unsigned y, int value)
{
    auto *cv = static_cast<mistral::CycloneV *>(cvp);
    unsigned i = y * cv->di.cram_sx + x;
    bool cur = (cv->cram[i >> 3] >> (i & 7)) & 1;
    if (cur == bool(value))
        return 0;
    if (value)
        cv->cram[i >> 3] |= (unsigned char)(1u << (i & 7));
    else
        cv->cram[i >> 3] &= (unsigned char)(~(1u << (i & 7)));
    return 1;
}
