#pragma once

//
// framecensus - PURE. The game's own frame interval, sorted by which layer of the overlay
// was switched on when it was measured.
//
// The mod's perf counters time the mod's work, and every one of them came back small while
// the game still lost 1.7 ms a frame. What a counter inside our code cannot see is what the
// game's threads lose AROUND it, and the only instrument that sees that is the interval
// between the game's own presents - which the Present hook is standing in.
//
// So the hook samples that interval into one histogram per phase, the phase rotates on a
// wall clock, and the comparison is between phases of one capture rather than between cells
// of separate runs. Scene drift, the mod's warm-up and the order of the cells then land on
// every phase equally, which is what killed three earlier runs.
//
// Fixed bins, no allocation, no locking: the render thread is the only writer and the loop
// thread reads a possibly half-updated bin, which costs one sample of accuracy in a
// thousand-sample window.
//

#include <cstdint>

namespace fc
{
    // What was switched off while the interval was measured. The differences are the
    // measurement: 0-1 is the composition, 1-2 is the overlay's own frame, 2-3 is the
    // compositor carrying our layer at all, and 3-4 is every thread of ours that runs
    // between frames. What is left under 4 is the mod merely being loaded and hooked.
    enum Phase : int
    {
        Full = 0,      // as shipped
        NoCompose = 1, // drawn, recorded, submitted; never copied into the surface
        NoFrame = 2,   // the hook enters, does its housekeeping and returns
        NoVisual = 3,  // NoFrame, and our visual is detached from the window's tree
        NoBackground = 4, // NoVisual, and the game-thread pump and the loop slicer stand down
        kPhases = 5,
    };

    constexpr int kBins = 800;       // 0.05 ms each: 0 .. 40 ms
    constexpr double kBinMs = 0.05;  // past that the sample lands in `over`

    struct Bucket
    {
        std::uint64_t samples = 0;
        double sum_ms = 0.0;
        std::uint64_t over = 0; // intervals longer than the last bin, still in samples/sum
        std::uint32_t bin[kBins] = {};
    };

    struct Census
    {
        Bucket p[kPhases];
    };

    // One sample into one histogram. A `Bucket` is usable on its own - the frame ceiling's
    // own A/B keeps two of them and no Census - so the binning lives here and `record`
    // below is only the phase lookup in front of it.
    inline void record_ms(Bucket& b, double ms)
    {
        if (!(ms >= 0.0))
        {
            return;
        }
        ++b.samples;
        b.sum_ms += ms;
        const int slot = static_cast<int>(ms / kBinMs);
        if (slot >= kBins)
        {
            ++b.over;
            return;
        }
        ++b.bin[slot];
    }

    inline void record(Census& c, int phase, double ms)
    {
        if (phase < 0 || phase >= kPhases)
        {
            return;
        }
        record_ms(c.p[phase], ms);
    }

    inline double mean_ms(const Bucket& b)
    {
        return b.samples > 0 ? b.sum_ms / static_cast<double>(b.samples) : 0.0;
    }

    // The bin containing the given fraction of the samples, as its midpoint. Samples past
    // the last bin count towards the fraction, so a percentile that lands in them reads as
    // the top of the range rather than wrapping back.
    inline double percentile_ms(const Bucket& b, double frac)
    {
        if (b.samples == 0)
        {
            return 0.0;
        }
        if (frac < 0.0)
        {
            frac = 0.0;
        }
        if (frac > 1.0)
        {
            frac = 1.0;
        }
        const std::uint64_t want = static_cast<std::uint64_t>(frac * static_cast<double>(b.samples));
        std::uint64_t seen = 0;
        for (int i = 0; i < kBins; ++i)
        {
            seen += b.bin[i];
            if (seen > want)
            {
                return (static_cast<double>(i) + 0.5) * kBinMs;
            }
        }
        return static_cast<double>(kBins) * kBinMs;
    }

    inline void reset(Census& c)
    {
        c = Census{};
    }
} // namespace fc
