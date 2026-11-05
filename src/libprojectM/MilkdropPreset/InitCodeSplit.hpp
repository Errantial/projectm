#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>

namespace libprojectM {
namespace MilkdropPreset {

/**
 * @brief The length of the leading top-level statements of expression code that name none of the
 * given names, up to and including the ';' that ends the last of them.
 *
 * Tokenizes the way projectm-eval's scanner does as far as it matters here: "//" and "/* * /"
 * comments are skipped, a $'c' character constant is one token, and a name is
 * [_a-zA-Z][_a-zA-Z0-9]*, compared without case. Names are also picked out of number tokens such
 * as "1e5", which can only add names and so only end the prefix earlier. A ';' outside parentheses
 * and brackets ends a top-level statement.
 *
 * The caller compiles the prefix and the rest separately; if a split were ever wrong, a half would
 * not compile and the caller must then run the code as one block.
 *
 * @param code The expression code.
 * @param forbidden Lower-case variable and function names the statements may not name.
 * @return The length of the prefix in bytes: 0 if the first statement names one of them, the
 *         whole length if no statement does.
 */
auto IndependentPrefixLength(const std::string& code, const std::unordered_set<std::string>& forbidden) -> size_t;

} // namespace MilkdropPreset
} // namespace libprojectM
