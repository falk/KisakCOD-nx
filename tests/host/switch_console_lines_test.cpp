// Host test for src/port/switch_console_lines.h, the splitter
// Com_ParseCommandLine runs on the startup command line (kisak_diag.cfg plus
// nxlink arguments): every '+' command must become its own console line with
// the map command last, and a line past the cap must be reported, not lost
// silently.

#include "src/port/switch_console_lines.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        ++g_checks;                                                         \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// A kisak_diag.cfg-shaped line: `sets` +set commands, then +spmap cargoship.
static std::string CfgLine(int sets)
{
    std::string s;
    for (int i = 0; i < sets; ++i)
        s += "+set dvar" + std::to_string(i) + " " + std::to_string(i) + " ";
    return s + "+spmap cargoship";
}

struct Split
{
    std::vector<std::string> lines;
    int dropped;
};

static Split Run(const std::string &line, int cap)
{
    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    std::vector<char *> out(cap);
    Split r;
    const int n = Sw_SplitConsoleLines(buf.data(), out.data(), cap, &r.dropped);
    for (int i = 0; i < n; ++i)
        r.lines.emplace_back(out[i]);
    return r;
}

// Retail Com_ParseCommandLine with its fixed 32-line array, kept as the
// negative control.
static std::vector<std::string> RetailSplit(const std::string &line)
{
    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    char *lines[32];
    char *p = buf.data();
    int count = 1;
    lines[0] = p;
    for (; *p; ++p)
    {
        if (*p == '+' || *p == '\n')
        {
            if (count == 32)
                break;
            lines[count++] = p + 1;
            *p = 0;
        }
    }
    return std::vector<std::string>(lines, lines + count);
}

// Every command on its own line, the map command last and nothing dropped.
static void ExpectClean(int sets)
{
    const Split r = Run(CfgLine(sets), SW_CONSOLE_LINE_CAP);
    CHECK(r.dropped == 0);
    CHECK((int)r.lines.size() == sets + 2); // the empty text before the first '+', the sets, the map
    CHECK(r.lines.back() == "spmap cargoship");
    bool each = true;
    for (int i = 0; i < sets; ++i)
        each = each && r.lines[i + 1] == "set dvar" + std::to_string(i) + " " + std::to_string(i) + " ";
    CHECK(each);
}

int main()
{
    CHECK(SW_CONSOLE_LINE_CAP == 128);

    // 31 '+' commands (30 sets and the map) filled retail's 32 lines exactly.
    ExpectClean(30);
    // 100 '+' commands: past retail, well inside the cap.
    ExpectClean(99);
    // The largest line that fits: SW_CONSOLE_LINE_CAP - 1 '+' commands.
    ExpectClean(SW_CONSOLE_LINE_CAP - 2);

    // A newline separates like '+'.
    {
        const Split r = Run("+set a 1\nset b 2+spmap x", SW_CONSOLE_LINE_CAP);
        CHECK(r.dropped == 0 && r.lines.size() == 4 && r.lines[2] == "set b 2" && r.lines[3] == "spmap x");
    }

    // 200 '+' commands overflow the cap and are counted: 200 + 1 lines wanted,
    // SW_CONSOLE_LINE_CAP kept, the rest reported, the map not on its own line.
    {
        const Split r = Run(CfgLine(199), SW_CONSOLE_LINE_CAP);
        CHECK((int)r.lines.size() == SW_CONSOLE_LINE_CAP);
        CHECK(r.dropped == 201 - SW_CONSOLE_LINE_CAP);
        CHECK(r.lines.back() != "spmap cargoship");
        CHECK(r.lines.back().find("+spmap cargoship") != std::string::npos);
    }
    // One past the cap reports exactly one.
    {
        const Split r = Run(CfgLine(SW_CONSOLE_LINE_CAP - 1), SW_CONSOLE_LINE_CAP);
        CHECK(r.dropped == 1 && r.lines.back() == "set dvar126 126 +spmap cargoship");
    }

    // Negative control: the retail 32-line splitter on a 32-command line
    // (31 sets and the map) leaves the map glued to the last set, and the
    // same line splits cleanly with the new cap.
    {
        const std::string line = CfgLine(31);
        const std::vector<std::string> retail = RetailSplit(line);
        CHECK(retail.size() == 32);
        CHECK(retail.back() == "set dvar30 30 +spmap cargoship");
        const Split r = Run(line, SW_CONSOLE_LINE_CAP);
        CHECK(r.dropped == 0 && r.lines.back() == "spmap cargoship");
        // The new splitter at retail's cap reproduces retail exactly and says so.
        const Split capped = Run(line, 32);
        CHECK(capped.lines == retail && capped.dropped == 1);
        // 31 commands still fit retail.
        CHECK(RetailSplit(CfgLine(30)).back() == "spmap cargoship");
    }

    if (g_failures)
    {
        fprintf(stderr, "FAIL: switch_console_lines_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS: switch_console_lines_test: %d checks (31/100/127 commands split, 200 overflow reported, retail 32-line control)\n",
           g_checks);
    return 0;
}
