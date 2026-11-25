#pragma once

#include <juce_core/juce_core.h>
#include <vector>

/** The spans arrive in beat order. A line goes below every earlier line it overlaps. */
inline std::vector<int> lyricRowsFor(const std::vector<juce::Range<float>>& spans)
{
    std::vector<float> rowEnds;
    std::vector<int> rows;
    rows.reserve(spans.size());

    for (auto span : spans)
    {
        int row = static_cast<int>(rowEnds.size()) - 1;
        while (row >= 0 && rowEnds[static_cast<size_t>(row)] <= span.getStart())
            --row;
        ++row;

        if (row == static_cast<int>(rowEnds.size()))
            rowEnds.push_back(span.getEnd());
        else
            rowEnds[static_cast<size_t>(row)] = span.getEnd();
        rows.push_back(row);
    }
    return rows;
}
