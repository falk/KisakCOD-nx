#ifndef SWITCH_EVENT_H
#define SWITCH_EVENT_H

#if !defined(__SWITCH__)
#error "Switch event owner requires __SWITCH__"
#endif
#if !defined(KISAK_SP) || defined(KISAK_MP) || defined(KISAK_NO_FASTFILES)
#error "Switch event owner requires KISAK_SP without MP or KISAK_NO_FASTFILES"
#endif

#ifndef __cdecl
#define __cdecl
#endif

typedef void (*SwitchEventFailureHook)(void *context);

void Switch_EventSetFailureHook(SwitchEventFailureHook hook, void *context);

#endif
