/*
 * kmod-info.cpp - hand-rolled replacement for Xcode's CreateKModInfo.perl output.
 *
 * Without a _kmod_info symbol the kernel rejects the kext with
 * "kexts must have a _kmod_info symbol", which is what the first link of this
 * rebuild produced: the section list and every other symbol matched the released
 * binary, but `nm` showed only the two mangled _start/_stop symbols and no
 * _kmod_info data symbol, where the released binary has one at a known address.
 * The Wi-Fi driver in this repo solved the same problem the same way
 * (driver/AirportMtk/kmod-info.c), so this mirrors it.
 *
 * Written in C++ rather than C because zutil.c defines _start/_stop *outside*
 * its extern "C" block, so they carry C++ mangling (__Z5_stopP9kmod_infoPv /
 * __Z6_startP9kmod_infoPv, confirmed with nm). A plain C translation unit would
 * declare them with C linkage and fail to link.
 */

#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>

/* Defined in zutil.c, C++ linkage - see the note above. */
int _start(struct kmod_info *, void *);
int _stop(struct kmod_info *, void *);

kmod_info_t kmod_info = {
    /* next            */ 0,
    /* info_version    */ KMOD_INFO_VERSION,
    /* id              */ (uint32_t)-1,
    /* name            */ "com.laobamac.SimpleMtkBtFw",
    /* version         */ "1.0.0",
    /* reference_count */ -1,
    /* reference_list  */ 0,
    /* address         */ 0,
    /* size            */ 0,
    /* hdr_size        */ 0,
    /* start           */ (kmod_start_func_t *)_start,
    /* stop            */ (kmod_stop_func_t *)_stop,
};
