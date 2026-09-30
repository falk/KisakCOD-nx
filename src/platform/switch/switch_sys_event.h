#ifndef SWITCH_SYS_EVENT_H
#define SWITCH_SYS_EVENT_H

#if !defined(__SWITCH__)
#error "Switch system event owner requires __SWITCH__"
#endif
#if !defined(KISAK_SP) || defined(KISAK_MP) || defined(KISAK_NO_FASTFILES)
#error "Switch system event owner requires KISAK_SP without MP or KISAK_NO_FASTFILES"
#endif

typedef void (*SwitchSysEventFailureHook)(void *context);

void Switch_SysEventSetFailureHook(SwitchSysEventFailureHook hook, void *context);

#endif
