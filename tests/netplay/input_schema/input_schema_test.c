/* Exercise the actual facade defaults without enabling its SDL/game host,
 * then use the real protocol to prove schema agreement gates frame zero. */
#include "../../../runner/src/netplay/snes_netplay.c"
#include "recomp_net/recomp_net.h"
#include "recomp_net/startup_profiles.h"
#include <assert.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

static rnet_u64 now=1000;
static rnet_u64 clock_ms(void *ctx) { (void)ctx; return now; }
static void sample(rnet_u32 tick,RNetInputSample *out,void *ctx) {
    (void)ctx; memset(out,0,sizeof(*out));out->tick=tick;out->valid=1;out->size=2;
}
static void publish(rnet_u32 t,const RNetInputSample *in,int n,void *ctx) {
    (void)t;(void)in;(void)n;(void)ctx;
}
static void run_case(int mode) {
    RNetSession *s[2]; char bind[2][48];
    unsigned port=30000u+((unsigned)getpid()%4000u)*4u+(unsigned)mode*2u;
    for(int i=0;i<2;++i) {
        RNetConfig cfg;RNetHostVTable vt={0};
        rnet_config_init_defaults(&cfg);cfg.slot_count=2;cfg.local_slot=(rnet_u8)i;
        cfg.session_id=0x534e4100u+(unsigned)mode;
        vt.sample_local=sample;vt.publish=publish;vt.now_ms=clock_ms;
        s[i]=rnet_session_create(&cfg,&vt);assert(s[i]);
        /* mode1 models a peer with a different qualified 12-bit namespace;
         * mode2 models the actual legacy host, which has no startup offer. */
        if(!(mode==2 && i==1)) {
            RNetProfileCaps caps={0,0,1,0};
            uint32_t id=mode==1 && i==1 ? 0x534e1200u : g_profiles.qualification;
            assert(rnet_session_profiles_begin(s[i],id,&caps)==0);
        }
        snprintf(bind[i],sizeof(bind[i]),"127.0.0.1:%u",port+(unsigned)i);
    }
    for(int i=0;i<2;++i)
        assert(rnet_session_start_lan(s[i],bind[i],i ? bind[0] : "")==0);
    int matched=0,refused=0;
    /* A finite protocol workload, not an elapsed-time process cutoff. */
    for(int step=0;step<(mode==2 ? 512 : 4096);++step) {
        ++now; rnet_session_pump(s[1]);rnet_session_pump(s[0]);
        int a=rnet_session_profiles_status(s[0],NULL,NULL);
        int b=rnet_session_profiles_status(s[1],NULL,NULL);
        if(a!=1) assert(!rnet_session_try_admit(s[0],0));
        if(a==1 && b==1) { matched=1;break; }
        if(a<0 || (mode==1 && b<0)) {
            /* The first receiver may refuse before sending its own offer.
             * Neither side may release frame zero on that partial exchange. */
            assert(a!=1 && b!=1);
            assert(!rnet_session_try_admit(s[1],0));
            refused=1;break;
        }
        rnet_session_wait_recv(s[0],1);
    }
    if(mode==0) assert(matched && !refused);
    if(mode==1) assert(refused && !matched);
    if(mode==2) {
        assert(!matched && !refused);
        assert(rnet_session_profiles_status(s[0],NULL,NULL)==0);
        assert(!rnet_session_try_admit(s[0],0));
    }
    for(int i=0;i<2;++i)rnet_session_destroy(s[i]);
}
int main(void) {
    assert(g_profiles.qualification==SNES_INPUT_SCHEMA_ID && g_profiles.profiles==1);
    snes_netplay_set_profile_caps(0,0,0,1,0,NULL);
    assert(g_profiles.qualification==SNES_INPUT_SCHEMA_ID);
    snes_netplay_set_profile_caps(1234,0,0,1,0,NULL);
    assert(g_profiles.qualification==SnesInputQualification(1234));
    assert(g_profiles.qualification!=1234 && g_profiles.qualification!=0);
    snes_netplay_set_profile_caps(0,0,0,1,0,NULL);
    run_case(0);run_case(1);run_case(2);
    puts("input schema: matching accepted; old namespace refused; legacy missing offer cannot admit frame zero");
    return 0;
}
