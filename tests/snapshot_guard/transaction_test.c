int main(void) {
    machine.cpu=&cpu; machine.apu=&apu; machine.dma=&dma; machine.ppu=&ppu;
    machine.cart=&cart; machine.ram=ram; apu.dsp=&dsp; apu.spc=&spc;
    cart.ram=sram; cart.ramSize=sizeof(sram); g_rtl_game_info=&game;
    cpu.a=0x1234; host_extra=17; ram[70]=8;
    unsigned char *snapshot=malloc(1024*1024); assert(snapshot);
    size_t size=RtlSaveSnapshotToMemory(snapshot,1024*1024); assert(size>0x20000);
    cpu.a=0x5678; cpu.e=true; ram[70]=9; host_extra=42;
    const Cpu before_cpu=cpu; const Snes before_snes=machine;
    static Apu before_apu; static Dsp before_dsp; static Ppu before_ppu;
    before_apu=apu; before_dsp=dsp; before_ppu=ppu;
    const Dma before_dma=dma; const Spc before_spc=spc;
    unsigned char before_ram[sizeof(ram)], before_sram[sizeof(sram)];
    memcpy(before_ram,ram,sizeof(ram)); memcpy(before_sram,sram,sizeof(sram));
    size_t jp_size; const void *jp=joypad_state_blob(&jp_size);
    unsigned char before_jp[256]; assert(jp_size<=sizeof(before_jp)); memcpy(before_jp,jp,jp_size);
    const size_t cuts[]={9,300,size/2,size-sizeof(host_extra)-1,size-1};
    RtlGameInfo guest_check=game; guest_check.snapshot_preflight=NULL;
    unsigned before_captures=save_captures, before_loads=extra_loads;
    snes_saveload_set_version(7);
    uint32_t queried_version;
    assert(RtlSnapshotGuestSize(&queried_version)==size-sizeof(host_extra));
    assert(queried_version==RTL_SAV_VERSION && snes_saveload_get_version()==7);
    assert(!memcmp(&cpu,&before_cpu,sizeof(cpu)));
    for(size_t i=0;i<sizeof(cuts)/sizeof(cuts[0]);i++) {
        /* Guest truncations must exercise the guest walk, not be intercepted
         * by the optional title preflight. Only the last cut is in the chunk. */
        g_rtl_game_info=cuts[i]<size-sizeof(host_extra) ? &guest_check : &game;
        assert(!RtlLoadSnapshotFromMemory(snapshot,cuts[i]));
        assert(!memcmp(&cpu,&before_cpu,sizeof(cpu)));
        assert(!memcmp(&machine,&before_snes,sizeof(machine)));
        assert(!memcmp(&apu,&before_apu,sizeof(apu)) && !memcmp(&dsp,&before_dsp,sizeof(dsp)));
        assert(!memcmp(&ppu,&before_ppu,sizeof(ppu)) && !memcmp(&dma,&before_dma,sizeof(dma)));
        assert(!memcmp(&spc,&before_spc,sizeof(spc)) && !memcmp(ram,before_ram,sizeof(ram)));
        assert(!memcmp(sram,before_sram,sizeof(sram)) && !memcmp(jp,before_jp,jp_size));
        assert(ram[70]==9 && host_extra==42 && !audio_resets && !reconciled && !presentation_resets && !locks);
        assert(snes_saveload_get_version()==7);
        assert(save_captures==before_captures && extra_loads==before_loads);
        FILE *f=fopen("truncated.sav","wb"); assert(f);
        assert(fwrite(snapshot,1,cuts[i],f)==cuts[i]); assert(!fclose(f));
        assert(!RtlLoadSnapshot("truncated.sav"));
        assert(!memcmp(&cpu,&before_cpu,sizeof(cpu)) && host_extra==42 && !audio_resets);
    }
    g_rtl_game_info=&game;
    /* Reject malformed PPU metadata before the live serializer sees it. */
    MemorySli prefix={{&memory_sli_func},NULL,0,8,true,false};
    cpu_saveload(&cpu,&prefix.base); apu_saveload(&apu,&prefix.base); dma_saveload(&dma,&prefix.base);
    snapshot[prefix.position]^=1;
    assert(!RtlLoadSnapshotFromMemory(snapshot,size)); snapshot[prefix.position]^=1;
    assert(!memcmp(&cpu,&before_cpu,sizeof(cpu)) && !audio_resets);
    assert(!RtlLoadSnapshotFromMemory(snapshot,RTL_SAV_MAX_SIZE+1));
    RtlGameInfo rejecting=game; rejecting.snapshot_preflight=title_preflight;
    g_rtl_game_info=&rejecting; assert(!RtlLoadSnapshotFromMemory(snapshot,size));
    assert(!memcmp(&cpu,&before_cpu,sizeof(cpu)) && host_extra==42 && !audio_resets);
    g_rtl_game_info=&game;
    assert(RtlLoadSnapshotFromMemory(snapshot,size));
    assert(cpu.a==0x1234 && ram[70]==8 && host_extra==17);
    assert(reconciled==1 && presentation_resets==1 && audio_resets==1 && !locks);
    /* An unchanged consumer may load an older chunk smaller than its current
     * save output. Its save callback must never run as input validation. */
    RtlGameInfo legacy=game; legacy.state_load_extra=load_variable;
    legacy.snapshot_preflight=NULL;
    g_rtl_game_info=&legacy;
    size_t legacy_core=size-sizeof(host_extra);
    unsigned captures_before=save_captures;
    unsigned loads_before=extra_loads;
    snapshot[legacy_core]=17; host_extra=42;
    assert(RtlLoadSnapshotFromMemory(snapshot,legacy_core+1));
    assert(host_extra==17 && save_captures==captures_before);
    assert(extra_loads==loads_before+1);
    FILE *legacy_file=fopen("legacy.sav","wb"); assert(legacy_file);
    assert(fwrite(snapshot,1,legacy_core+1,legacy_file)==legacy_core+1);
    assert(!fclose(legacy_file)); host_extra=42;
    assert(RtlLoadSnapshot("legacy.sav") && host_extra==17);
    assert(save_captures==captures_before);
    assert(extra_loads==loads_before+2);
    /* The current four-byte extension remains accepted without validators. */
    host_extra=42;
    assert(RtlLoadSnapshotFromMemory(snapshot,size) && host_extra==17);
    assert(extra_loads==loads_before+3 && save_captures==captures_before);
    const Cpu legacy_cpu=cpu;
    assert(!RtlLoadSnapshotFromMemory(snapshot,legacy_core-1));
    assert(save_captures==captures_before && extra_loads==loads_before+3);
    assert(!memcmp(&cpu,&legacy_cpu,sizeof(cpu)));
    /* A consumer without an extension loader historically ignores trailing
     * chunks; do not turn those saves into new compatibility errors. */
    RtlGameInfo no_loader=legacy; no_loader.state_load_extra=NULL;
    g_rtl_game_info=&no_loader; host_extra=42;
    assert(RtlLoadSnapshotFromMemory(snapshot,legacy_core+1) && host_extra==42);
    assert(save_captures==captures_before && extra_loads==loads_before+3);
    /* No generic transaction is promised for an opaque legacy callback. It
     * runs once on the live state; a late short-read retains original effects.
     * A title must opt into pure preflight to reject such a chunk earlier. */
    g_rtl_game_info=&legacy; cpu.a=0x5678;
    unsigned resets_before=audio_resets;
    assert(!RtlLoadSnapshotFromMemory(snapshot,legacy_core+2));
    assert(cpu.a==0x1234 && host_extra==42);
    assert(extra_loads==loads_before+4 && save_captures==captures_before);
    assert(audio_resets==resets_before);
    /* A validator accepts an older one-byte chunk and rejects corrupt chunks
     * without running a mutating title loader or resetting host delivery. */
    RtlGameInfo variable=game; variable.state_load_extra=load_variable;
    variable.snapshot_preflight=validate_title_extra; g_rtl_game_info=&variable;
    size_t core=size-sizeof(host_extra); host_extra=42;
    unsigned old_resets=audio_resets, old_generation=s_state_generation;
    snapshot[core]=99;
    assert(!RtlLoadSnapshotFromMemory(snapshot,size));
    assert(host_extra==42 && audio_resets==old_resets && s_state_generation==old_generation);
    snapshot[core]=17;
    assert(RtlLoadSnapshotFromMemory(snapshot,core+1)); assert(host_extra==17);
    /* Supported versions and guest-only states keep loading. */
    g_rtl_game_info=&legacy;
    for(uint32_t version=RTL_SAV_VERSION_MIN;version<=RTL_SAV_VERSION;version++) {
        uint32_t header[2]={RTL_SAV_MAGIC,version};
        MemorySli out={{&memory_sli_func},snapshot,1024*1024,0,true,false};
        memory_sli_func(&out.base,header,sizeof(header));
        snes_saveload_set_version(version); snes_saveload(&machine,&out.base);
        assert(!out.error); assert(RtlLoadSnapshotFromMemory(snapshot,out.position));
    }
    /* Early v6/v7 used the compact DMA and SNES tail layouts. */
    for(uint32_t version=6;version<=7;version++) {
        uint32_t header[2]={RTL_SAV_MAGIC,version};
        MemorySli out={{&memory_sli_func},snapshot,1024*1024,0,true,false};
        memory_sli_func(&out.base,header,sizeof(header));
        snes_saveload_set_version(version);
        cpu_saveload(&cpu,&out.base); apu_saveload(&apu,&out.base);
        unsigned char old_dma[216]={0}, old_tail[40]={0};
        memcpy(old_dma,dma.channel,208); memcpy(old_dma+208,&dma.dmaTimer,4);
        old_dma[212]=dma.dmaBusy;
        memory_sli_func(&out.base,old_dma,sizeof(old_dma));
        ppu_saveload(&ppu,&out.base); cart_saveload(&cart,&out.base);
        memory_sli_func(&out.base,old_tail,sizeof(old_tail));
        memory_sli_func(&out.base,ram,sizeof(ram)); memory_sli_func(&out.base,&machine.ramAdr,4);
        if(version==7) {
            memory_sli_func(&out.base,&machine.joypadStrobe,sizeof(machine.joypadStrobe));
            memory_sli_func(&out.base,&machine.joypad1Index,sizeof(machine.joypad1Index));
            memory_sli_func(&out.base,&machine.joypad2Index,sizeof(machine.joypad2Index));
            memory_sli_func(&out.base,&machine.joypad1Latched,sizeof(machine.joypad1Latched));
            memory_sli_func(&out.base,&machine.joypad2Latched,sizeof(machine.joypad2Latched));
        }
        assert(RtlLoadSnapshotFromMemory(snapshot,out.position));
        const Cpu saved_cpu=cpu; const Dma saved_dma=dma; const Snes saved_snes=machine;
        assert(!RtlLoadSnapshotFromMemory(snapshot,out.position-1));
        assert(!memcmp(&cpu,&saved_cpu,sizeof(cpu)) && !memcmp(&dma,&saved_dma,sizeof(dma)));
        assert(!memcmp(&machine,&saved_snes,sizeof(machine)));
    }
    /* Guarded file/memory input continues to share integrity validation. */
    RtlGameInfo guarded=game; guarded.snapshot_guard_identity=guard_identity;
    g_rtl_game_info=&guarded;
    size=RtlSaveSnapshotToMemory(snapshot,1024*1024); assert(size);
    snapshot[size-1]^=1; unsigned before_resets=audio_resets;
    assert(!RtlLoadSnapshotFromMemory(snapshot,size)); assert(audio_resets==before_resets);
    snapshot[size-1]^=1; assert(RtlLoadSnapshotFromMemory(snapshot,size));
    FILE *file=fopen("valid.sav","wb"); assert(file);
    assert(fwrite(snapshot,1,size,file)==size && !fclose(file));
    assert(RtlLoadSnapshot("valid.sav"));
    free(snapshot); return 0;
}
