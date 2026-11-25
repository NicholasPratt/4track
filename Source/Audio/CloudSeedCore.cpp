// Builds CloudSeedCore (MIT, ThirdParty/cloudseed) into 8track, for the Reverb effect.
#include <juce_core/juce_core.h>

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wshadow", "-Wshadow-field-in-constructor", "-Wsign-conversion", "-Wconversion",
                                    "-Wfloat-conversion", "-Wimplicit-float-conversion", "-Wshorten-64-to-32",
                                    "-Wunused-parameter", "-Wfloat-equal", "-Wextra-semi", "-Wzero-as-null-pointer-constant",
                                    "-Wimplicit-int-conversion", "-Wcast-align", "-Wunused-variable", "-Wdeprecated",
                                    "-Wmissing-field-initializers", "-Wold-style-cast", "-Wunused-private-field",
                                    "-Wreorder-ctor", "-Wsign-compare", "-Wunused-function", "-Wmacro-redefined")
#include "../../ThirdParty/cloudseed/CloudSeed.cpp"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
