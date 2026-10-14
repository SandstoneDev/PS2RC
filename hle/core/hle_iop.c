/* hle_iop.c - IOP-side services reached through SIF RPC (the sceSifCallRpc hook in hle_sce.c): a registry of RPC
 * servers by server id. The servers are the game pack's (its IOP modules: streaming, sound driver ...), registered
 * by hle_pack_iop_init at boot; a call to an id nobody registered is logged and answered with an error.
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* PZ_SCOPE zones */

#ifdef HLE_HAVE_AUDIO_MODEL
int hle_opt_audio_model = 1;                   /* the pack's sound-driver model (host --audio model|silent) */
#else
int hle_opt_audio_model = 0;
#endif

#define HLE_RPC_MAX 16
static struct { uint32_t id; HleRpcFn fn; } rpc_srv[HLE_RPC_MAX];
static int rpc_n;

void hle_rpc_register(uint32_t rpc_id, HleRpcFn fn)
{
    if (rpc_n < HLE_RPC_MAX) {
        rpc_srv[rpc_n].id = rpc_id;
        rpc_srv[rpc_n].fn = fn;
        rpc_n++;
    } else
        LOG("SIF RPC: too many servers, %08x not registered\n", rpc_id);
}

int hle_rpc_registered(uint32_t rpc_id)
{
    for (int i = 0; i < rpc_n; i++)
        if (rpc_srv[i].id == rpc_id) return 1;
    return 0;
}

void hle_iop_init(void)
{
    hle_pack_iop_init();
}

int hle_rpc_call(R5900Ctx *c, uint32_t rpc_id, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize,
                 uint32_t recv, uint32_t rsize)
{
    PZ_SCOPE(rpc);
    for (int i = 0; i < rpc_n; i++)
        if (rpc_srv[i].id == rpc_id) return rpc_srv[i].fn(c, fno, mode, send, ssize, recv, rsize);
    LOG("SIF RPC to unknown server %08x fno %u send %08x/%u recv %08x/%u\n", rpc_id, fno, send, ssize, recv, rsize);
    return -1;
}

void hle_iop_report(void)
{
    hle_pack_iop_report();
}
