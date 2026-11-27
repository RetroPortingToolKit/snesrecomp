/* SPDX-License-Identifier: MIT
 * Local correction reuses the complete SNES/title snapshot and replay contract.
 * Only correction scheduling changes; transport, saves and native execution
 * stay with their existing owners.
 */
static uint64_t local_now(void *ctx) {
    (void)ctx;
    uint64_t now=SDL_GetPerformanceCounter(),frequency=SDL_GetPerformanceFrequency();
    return now/frequency*1000000u+now%frequency*1000000u/frequency;
}
static int local_load(void *ctx,uint32_t tick) {
    int ok=rb_host_snap_load(ctx,tick);
    if(ok)++g_rb.local_loads;
    return ok;
}
static void local_publish(void *ctx,uint32_t tick,const RNetInputSample *rows,int slots,int replay) {
    (void)ctx;
    if(replay)++g_rb.local_replays;
    for(int i=0;i<slots;++i) {
        if(rows[i].size!=SNES_NETPLAY_PAD_BYTES) {g_rb.local_error="invalid SNES input row";return;}
        g_rb.resolved[i]=(uint16_t)(rows[i].bytes[0]|(uint16_t)rows[i].bytes[1]<<8)&RB_BUTTON_MASK;
    }
    if(g_rb.b.publish)g_rb.b.publish(tick,g_rb.resolved,slots);
    if(g_rb.b.apply_sync_bytes)g_rb.b.apply_sync_bytes(rows[0].bytes+2);
}
static void local_neutral(void *ctx,int seat,RNetInputSample *out) {
    (void)ctx;(void)seat;memset(out,0,sizeof(*out));
    out->valid=1;out->size=SNES_NETPLAY_PAD_BYTES;
}
static int local_start(void) {
    RNetLocalConfig config={0};RNetLocalHost host={0};
    config.session=*g_rb.b.session;config.seats=(uint32_t)rb_slot_count();
    config.local_seat=(uint32_t)*g_rb.b.local_slot;config.row_size=SNES_NETPLAY_PAD_BYTES;
    config.delay=rnet_session_committed_delay(config.session);
    config.rollback_depth=g_rb.snap_depth-2u;config.tick_us=16639u;
    host.save=rb_host_snap_save;host.load=local_load;host.drop_after=rb_host_snap_drop_after;
    host.publish=local_publish;host.run_tick=rb_host_run_tick;host.neutral=local_neutral;
    host.resim_begin=rb_host_resim_begin;host.resim_end=rb_host_resim_end;
    host.digest=rb_host_digest_master;host.now_us=local_now;
    g_rb.local=rnet_local_create(&config,&host);
    if(!g_rb.local)return 0;
    fprintf(stderr,"snes_netplay: local-v4 seats=%u delay=%u depth=%u\n",
            config.seats,config.delay,config.rollback_depth);
    return 1;
}
static void local_report(void) {
    size_t size=rnet_local_report(g_rb.local,NULL,0);char *report=size?malloc(size):NULL;
    if(!report || rnet_local_report(g_rb.local,report,size)!=size) {free(report);return;}
    fprintf(stderr,"[snes-local-report] %s\n",report);
    const char *path=getenv("SNES_RB_LOCAL_REPORT");
    if(path && *path) {FILE *file=fopen(path,"wbx");if(file) {fputs(report,file);fputc('\n',file);fclose(file);}}
    free(report);
}
