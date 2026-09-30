#ifndef SWITCH_CRITICAL_SECTION_H
#define SWITCH_CRITICAL_SECTION_H

#if !defined(__SWITCH__)
#error "Switch critical section owner requires __SWITCH__"
#endif
#if !defined(KISAK_SP) || defined(KISAK_MP) || defined(KISAK_NO_FASTFILES)
#error "Switch critical section owner requires KISAK_SP without MP or KISAK_NO_FASTFILES"
#endif

typedef void (*SwitchCriticalSectionFailureHook)(void *context);

void Switch_CriticalSectionSetFailureHook(SwitchCriticalSectionFailureHook hook, void *context);

#endif
