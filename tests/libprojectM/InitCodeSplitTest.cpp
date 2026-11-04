#include <MilkdropPreset/InitCodeSplit.hpp>

#include <projectm-eval.h>

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

using libprojectM::MilkdropPreset::IndependentPrefixLength;

namespace {

const std::unordered_set<std::string> forbidden{"rand", "time", "bass", "zoom", "frame"};

auto Prefix(const std::string& code) -> std::string
{
    return code.substr(0, IndependentPrefixLength(code, forbidden));
}

} // namespace

TEST(InitCodeSplit, StopsAtTheFirstStatementNamingAForbiddenName)
{
    EXPECT_EQ(Prefix("i = 0; loop(10, gmegabuf(i) = 0; i = i + 1); t0 = time; x = 1;"),
              "i = 0; loop(10, gmegabuf(i) = 0; i = i + 1);");
    EXPECT_EQ(Prefix("t0 = time; i = 0;"), "");
    EXPECT_EQ(Prefix("a = rand(5); b = 1;"), "");
    EXPECT_EQ(Prefix("a = 1; zoom = 2; b = 3;"), "a = 1;");
}

TEST(InitCodeSplit, NamesAreWholeTokensWithoutCase)
{
    EXPECT_EQ(Prefix("TIME = 1;"), "");
    EXPECT_EQ(Prefix("Bass_x = 1; mytime = 2; time2 = 3;"), "Bass_x = 1; mytime = 2; time2 = 3;");
    EXPECT_EQ(Prefix("a = 1; b = 2 + Zoom;"), "a = 1;");
}

TEST(InitCodeSplit, OnlyTopLevelSemicolonsEndAStatement)
{
    EXPECT_EQ(Prefix("x = (a = 1; time); y = 2;"), "");
    EXPECT_EQ(Prefix("loop(3, a = a + 1; b = b + 2); c = megabuf[time];"), "loop(3, a = a + 1; b = b + 2);");
    EXPECT_EQ(Prefix("x = (1;"), "");
}

TEST(InitCodeSplit, CommentsAndCharacterConstantsAreSkipped)
{
    EXPECT_EQ(Prefix("a = 1; // time;\nb = 2;"), "a = 1; // time;\nb = 2;");
    EXPECT_EQ(Prefix("/* time; rand( */ a = 1; b = 2;"), "/* time; rand( */ a = 1; b = 2;");
    EXPECT_EQ(Prefix("a = $';'; b = time;"), "a = $';';");
}

TEST(InitCodeSplit, TheLastStatementNeedNotEndWithASemicolon)
{
    EXPECT_EQ(Prefix("a = 1; b = 2"), "a = 1; b = 2");
    EXPECT_EQ(Prefix("a = 1; b = time"), "a = 1;");
    EXPECT_EQ(Prefix(""), "");
}

TEST(InitCodeSplit, NumbersOnlyEverEndThePrefixEarlier)
{
    // "1e5" yields the name "e5", which is not forbidden; a name hidden in a number never makes
    // a statement look independent when it is not.
    EXPECT_EQ(Prefix("a = 1e5; b = .5;"), "a = 1e5; b = .5;");
    const std::unordered_set<std::string> withE5{"e5"};
    EXPECT_EQ(IndependentPrefixLength("a = 1e5; b = 2;", withE5), 0u);
}

namespace {

struct Run {
    std::vector<PRJM_EVAL_F> values;
    std::vector<PRJM_EVAL_F> memory;
    PRJM_EVAL_F registers[100]{};
};

/**
 * Runs code in a fresh context, either as one block or split at the prefix, and reads back the
 * named variables, the first gmegabuf entries and the registers.
 */
auto Execute(const std::string& code, bool split, const std::vector<std::string>& names) -> Run
{
    Run run;
    auto memory = projectm_eval_memory_buffer_create();
    auto* context = projectm_eval_context_create(memory, &run.registers);
    std::vector<std::string> parts;
    if (split)
    {
        const auto length = IndependentPrefixLength(code, forbidden);
        parts = {code.substr(0, length), code.substr(length)};
    }
    else
    {
        parts = {code};
    }
    for (const auto& part : parts)
    {
        if (part.find_first_not_of(" \t\r\n;") == std::string::npos)
        {
            continue;
        }
        auto* compiled = projectm_eval_code_compile(context, part.c_str());
        EXPECT_NE(compiled, nullptr) << part;
        projectm_eval_code_execute(compiled);
        projectm_eval_code_destroy(compiled);
    }
    for (const auto& name : names)
    {
        run.values.push_back(*projectm_eval_context_register_variable(context, name.c_str()));
    }
    // Read gmegabuf back through code, which is the only public way in.
    for (int index = 0; index < 64; index++)
    {
        const auto read = "gmegabuf(" + std::to_string(index) + ")";
        auto* compiled = projectm_eval_code_compile(context, read.c_str());
        run.memory.push_back(projectm_eval_code_execute(compiled));
        projectm_eval_code_destroy(compiled);
    }
    projectm_eval_context_destroy(context);
    projectm_eval_memory_buffer_destroy(memory);
    return run;
}

} // namespace

TEST(InitCodeSplit, RunningThePrefixThenTheRestEqualsRunningTheWhole)
{
    const std::vector<std::string> codes{
        "i = 0; loop(64, gmegabuf(i) = i * 3; i = i + 1); n = 50; reg01 = n * 2; q30 = 24; t0 = time; x = t0 + n;",
        "// clear\ni = 0; loop(32, gmegabuf(i) = 0; i = i + 1);\ncount = 7; /* ; */ v = count / 2; q1 = v; zoom = 1.5; y = zoom * v",
        "a = $';'; b = a + 1; c = sin(b) + sqr(a); d = if(above(c, 1), c, -c); e = rand(10) * 0; f = d + e;",
        "megabuf(5) = 3; m = megabuf(5); exec2(p = 1, p = p + 1); w = 0; while(w = w + 1; below(w, 10)); z = frame;",
    };
    const std::vector<std::string> names{"i", "n", "t0", "x", "count", "v", "q1", "q30", "y", "a", "b", "c", "d",
                                         "f", "m", "p", "w", "z", "reg01"};
    for (const auto& code : codes)
    {
        const auto whole = Execute(code, false, names);
        const auto split = Execute(code, true, names);
        EXPECT_GT(IndependentPrefixLength(code, forbidden), 0u) << code;
        EXPECT_EQ(whole.values, split.values) << code;
        EXPECT_EQ(whole.memory, split.memory) << code;
        for (int r = 0; r < 100; r++)
        {
            EXPECT_EQ(whole.registers[r], split.registers[r]) << code;
        }
    }
}
