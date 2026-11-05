#include "InitCodeSplit.hpp"

#include <algorithm>
#include <cctype>

namespace libprojectM {
namespace MilkdropPreset {

namespace {

auto IsNameStart(char c) -> bool
{
    return c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0;
}

auto IsNameChar(char c) -> bool
{
    return IsNameStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

auto Lower(std::string text) -> std::string
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

auto IndependentPrefixLength(const std::string& code, const std::unordered_set<std::string>& forbidden) -> size_t
{
    size_t prefixEnd = 0;
    int depth = 0;
    bool statementTouches = false;
    const size_t size = code.size();
    size_t i = 0;
    while (i < size)
    {
        const char c = code[i];
        if (c == '/' && i + 1 < size && code[i + 1] == '/')
        {
            const auto end = code.find('\n', i + 2);
            i = end == std::string::npos ? size : end + 1;
            continue;
        }
        if (c == '/' && i + 1 < size && code[i + 1] == '*')
        {
            const auto end = code.find("*/", i + 2);
            i = end == std::string::npos ? size : end + 2;
            continue;
        }
        if (c == '$' && i + 3 < size && code[i + 1] == '\'' && code[i + 3] == '\'')
        {
            i += 4;
            continue;
        }
        if (IsNameStart(c))
        {
            const size_t start = i;
            while (i < size && IsNameChar(code[i]))
            {
                i++;
            }
            if (forbidden.count(Lower(code.substr(start, i - start))) != 0)
            {
                statementTouches = true;
            }
            continue;
        }
        if (c == '(' || c == '[')
        {
            depth++;
        }
        else if (c == ')' || c == ']')
        {
            depth--;
        }
        else if (c == ';' && depth == 0)
        {
            if (statementTouches)
            {
                return prefixEnd;
            }
            prefixEnd = i + 1;
        }
        i++;
    }
    // The last statement need not end with a ';'.
    return statementTouches || depth != 0 ? prefixEnd : size;
}

} // namespace MilkdropPreset
} // namespace libprojectM
