# "This must not compile" checks (./test's negative_checks() and friends):
# every Switch-only production TU must reject KISAK_MP, KISAK_NO_FASTFILES,
# and a missing KISAK_SP/__SWITCH__ guard, and switch_signed_char_probe.cpp
# must accept -fsigned-char while rejecting -funsigned-char (the q_shared.h
# guard behind the negative-usercmd movement-byte bug). See
# negative_compile_check.py for why these run as scripted ctest tests
# rather than add_executable() targets.

kisak_negative_test(negative-base
    CC gcc STD c11 SOURCE ${H}/switch_skeleton.c
    INCLUDES ${R}
    EXTRA_ARGS
        --fail-variant -DKISAK_SP -DKISAK_MP --end
        --fail-variant -DKISAK_SP -DKISAK_NO_FASTFILES --end
        --fail-variant --end)

kisak_negative_test(negative-sys-event
    CC g++ STD c++11 SOURCE ${S}/platform/switch/switch_sys_event.cpp
    INCLUDES ${R}
    EXTRA_ARGS
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_MP --end
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_NO_FASTFILES --end
        --fail-variant -D__SWITCH__ --end)

kisak_negative_test(negative-critical-section
    CC g++ STD c++11 SOURCE ${S}/platform/switch/switch_critical_section.cpp
    INCLUDES ${R}
    EXTRA_ARGS
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_MP --end
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_NO_FASTFILES --end
        --fail-variant -D__SWITCH__ --end
        --fail-variant -DKISAK_SP --end)

kisak_negative_test(negative-event
    CC g++ STD c++11 SOURCE ${S}/platform/switch/switch_event.cpp
    INCLUDES ${R}
    EXTRA_ARGS
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_MP --end
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_NO_FASTFILES --end
        --fail-variant -D__SWITCH__ --end
        --fail-variant -DKISAK_SP --end)

kisak_negative_test(negative-thread
    CC g++ STD c++11 SOURCE ${S}/platform/switch/switch_thread.cpp
    INCLUDES ${R}
    EXTRA_ARGS
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_MP --end
        --fail-variant -D__SWITCH__ -DKISAK_SP -DKISAK_NO_FASTFILES --end
        --fail-variant -D__SWITCH__ --end
        --fail-variant -DKISAK_SP --end)

kisak_negative_test(negative-signed-char
    CC g++ STD c++20 SOURCE ${H}/switch_signed_char_probe.cpp
    INCLUDES ${S} ${R}/deps ${R}
    EXTRA_ARGS
        --common-flag=-DKISAK_SP --common-flag=-D__SWITCH__
        --common-flag=-D__cdecl= --common-flag=-D__stdcall=
        --pass-variant -fsigned-char --end
        --fail-variant -funsigned-char --end)
