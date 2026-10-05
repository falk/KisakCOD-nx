#pragma once

// Startup command-line splitting for Com_ParseCommandLine: every '+' or '\n'
// starts a new console line (the separator becomes a NUL). Pure string logic
// so it runs on the host.

// Console lines Com_ParseCommandLine keeps. Retail kept 32, so a command line
// with more than 31 '+' commands (a long kisak_diag.cfg) ran the rest glued to
// line 32 and lost its final map command.
#define SW_CONSOLE_LINE_CAP 128

// Splits commandLine in place into at most maxLines lines stored in lines[].
// Returns the line count. Separators past the cap are left unsplit (their
// commands stay glued to the last line, as retail did); *dropped receives how
// many there were, so the caller can report the lost commands.
inline int Sw_SplitConsoleLines(char *commandLine, char **lines, int maxLines, int *dropped)
{
    int count = 1;
    lines[0] = commandLine;
    *dropped = 0;
    for (char *p = commandLine; *p; ++p)
    {
        if (*p != '+' && *p != '\n')
            continue;
        if (count == maxLines)
        {
            ++*dropped;
            continue;
        }
        lines[count++] = p + 1;
        *p = 0;
    }
    return count;
}
