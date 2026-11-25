"""Compile the production coroutine call-control seam without a game build."""
from pathlib import Path
import os,re,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]

def function(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'

class HostContext(unittest.TestCase):
    def test_actual_nested_cpu_and_bridge_context_preserves_live_timeline(self):
        cc=os.environ.get('CC')or shutil.which('gcc')or shutil.which('cc')
        if not cc:self.skipTest('C compiler required')
        cpu=(ROOT/'runner/src/common_cpu_infra.c').read_text()
        bridge=(ROOT/'runner/src/snes/interp_bridge.c').read_text()
        source='#include <stdlib.h>\n#include <string.h>\n#include <setjmp.h>\n#include "common_cpu_infra.h"\n#include "cpu_state.h"\n#include "snes/interp_bridge.h"\n'
        source+=cpu[cpu.index('const char *g_last_recomp_func'):cpu.index('#define UNRESOLVED_ABANDON_MAX')]
        for name in ('g_wlog_aot_slot','g_recomp_yield_popped_top'):
            source+=re.search(r'^(?:static )?int '+name+r'[^\n]*;',cpu,re.M)[0]+'\n'
        source+=function(cpu,'void cpu_with_host_context(')
        names=('s_interp_bridge_depth','s_lle_sched_depth','s_interp_bounce_owner_depth',
          's_interp_bounce_recomp_base','s_interp_owner_exit_s','s_interp_owner_exit_valid',
          's_interp_owner_is_scheduler','s_sched_yield_pc','s_sched_yield_flag_addr',
          's_sched_yield_flag_value','s_lle_unwind_active','s_lle_unwind_pc24',
          's_lle_unwind_owner_depth','s_lle_unwind_is_deadline','s_lle_next_unwind_is_deadline',
          'g_interp_wlog_pc24','s_pre_opcode_redirect_valid','s_pre_opcode_redirect_pc24',
          's_pre_opcode_redirect_is_return')
        for name in names:
            source+=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\b[^\n;]*;',bridge,re.M)[0]+'\n'
        start=bridge.index('typedef struct AotResumeRec')
        source+=bridge[start:bridge.index('void interp_bridge_with_host_context(',start)]
        source+='int g_interp_apu_driving; static unsigned flushes; static uint64_t pending;\n'
        source+='static void bridge_apu_flush(CpuState *cpu) { (void)cpu;flushes++;pending=0; }\n'
        source+=function(bridge,'bool interp_bridge_guest_scope_idle(')
        source+=function(bridge,'void interp_bridge_guest_scope_unwind(')
        source+=function(bridge,'void interp_bridge_with_host_context(')
        source+='static int aot_resume_on(void) { return 1; }\n'
        source+='int cpu_dispatch_pc_matches(uint32 a,uint32 b) { return a==b; }\n'
        source+=function(bridge,'static void aot_resume_note_handback(')
        source+=function(bridge,'static int aot_resume_take_handback(')
        source+=r'''
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static void clean_host(void *arg) {
  CpuState *c=arg;
  CHECK(interp_bridge_guest_scope_idle() && g_recomp_stack_top==0 && !cpu_interrupt_context_active());
  CHECK(!g_tailcall_context_valid && !g_aot_resume_pc && c->host_return_valid==0);
  CHECK(!g_interp_apu_driving && !s_pre_opcode_redirect_valid && pending==0);
  CHECK(!aot_resume_take_handback(0x808888) && !aot_resume_take_handback(0x809000));
  c->ram[0]++;c->master_cycles+=100;
}
static void second_task(void *arg) {
  CpuState *c=arg;clean_host(c);
  g_recomp_stack_top=1;g_recomp_stack[0]="second";g_cpu_entry_s[0]=0x2bd;
  g_cpu_entry_s_valid[0]=1;g_cpu_entry_hrv[0]=2;g_cpu_entry_tailcall[0]=0;
  s_interp_bridge_depth=1;s_interp_bounce_owner_depth=1;s_interp_bounce_recomp_base=0;
  s_interp_owner_exit_s=0x2bd;s_interp_owner_exit_valid=1;
  s_aot_resume_n=1;s_aot_resume[0].pc=0x809000;
  aot_resume_note_handback(0x809000,0x809000);
  g_last_recomp_func="second";c->host_return_valid=2;pending=7;
  interp_bridge_with_host_context(c,clean_host,c);
  CHECK(g_recomp_stack_top==1 && !strcmp(g_recomp_stack[0],"second"));
  CHECK(s_interp_bridge_depth==1 && s_interp_bounce_owner_depth==1 && s_interp_owner_exit_s==0x2bd);
  CHECK(c->host_return_valid==2 && s_aot_resume[0].pc==0x809000 && s_aot_resume_n==1);
  CHECK(aot_resume_take_handback(0x809000) && !aot_resume_take_handback(0x809000));
  interp_bridge_guest_scope_unwind();cpu_guest_scope_unwind();g_aot_resume_pc=0;
  c->host_return_valid=0;pending=99;
}
static jmp_buf completed;
static void complete_and_run_next_task(void *arg) {
  second_task(arg);
  longjmp(completed,1); /* The scheduler deletes the completed fiber. */
}
int main(void) {
  static CpuState c;static uint8_t ram[1];c.ram=ram;c.master_cycles=10;c.host_return_valid=3;
  g_recomp_stack_top=2;g_recomp_stack[0]="outer";g_recomp_stack[1]="tail";
  g_cpu_entry_s[0]=0x13d;g_cpu_entry_s[1]=0x139;g_cpu_entry_s_valid[0]=g_cpu_entry_s_valid[1]=1;
  g_cpu_entry_return_frame[0]=3;g_cpu_entry_hrv[0]=3;g_cpu_entry_tailcall[1]=1;
  g_tailcall_context_valid=1;g_tailcall_entry_s=0x13d;g_tailcall_hrv=3;
  g_interrupt_context_depth=1;g_aot_resume_pc=0x808123;g_aot_resume_entry_s=0x13d;g_aot_resume_hrv=3;
  g_recomp_yield_popped_top=1;g_last_recomp_func="outer";g_wlog_aot_slot=1;
  s_interp_bridge_depth=2;s_interp_bounce_owner_depth=2;s_interp_bounce_recomp_base=1;
  s_interp_owner_exit_s=0x13d;s_interp_owner_exit_valid=1;g_interp_apu_driving=1;
  s_aot_resume_n=1;s_aot_resume[0].pc=0x808888;
  aot_resume_note_handback(0x808888,0x808888);
  s_pre_opcode_redirect_valid=1;s_pre_opcode_redirect_pc24=0;s_pre_opcode_redirect_is_return=1;
  g_interp_wlog_pc24=0x808100;pending=10;
  interp_bridge_with_host_context(&c,second_task,&c);
  CHECK(g_recomp_stack_top==2 && !strcmp(g_recomp_stack[0],"outer") && !strcmp(g_recomp_stack[1],"tail"));
  CHECK(g_cpu_entry_s[0]==0x13d && g_cpu_entry_s[1]==0x139 && g_cpu_entry_return_frame[0]==3);
  CHECK(g_cpu_entry_tailcall[1] && g_cpu_entry_hrv[0]==3 && g_cpu_entry_s_valid[1]);
  CHECK(g_tailcall_context_valid && g_tailcall_entry_s==0x13d && g_tailcall_hrv==3);
  CHECK(cpu_interrupt_context_active() && g_aot_resume_pc==0x808123 && g_aot_resume_entry_s==0x13d && g_aot_resume_hrv==3);
  CHECK(g_recomp_yield_popped_top==1 && g_wlog_aot_slot==1 && !strcmp(g_last_recomp_func,"outer"));
  CHECK(s_interp_bridge_depth==2 && s_interp_bounce_owner_depth==2 && s_interp_bounce_recomp_base==1);
  CHECK(s_interp_owner_exit_s==0x13d && s_interp_owner_exit_valid && g_interp_apu_driving==1);
  CHECK(s_aot_resume_n==1 && s_aot_resume[0].pc==0x808888 && g_interp_wlog_pc24==0x808100);
  CHECK(aot_resume_take_handback(0x808888) && !aot_resume_take_handback(0x808888));
  CHECK(s_pre_opcode_redirect_valid && s_pre_opcode_redirect_pc24==0 && s_pre_opcode_redirect_is_return);
  CHECK(ram[0]==2 && c.master_cycles==210 && pending==99 && flushes==2 && c.host_return_valid==3);
  aot_resume_note_handback(0x808888,0x808888);
  if(!setjmp(completed)) {
    interp_bridge_with_host_context(&c,complete_and_run_next_task,&c);
    CHECK(0); /* A dead task must never regain its old guest caller. */
  }
  CHECK(interp_bridge_guest_scope_idle() && !g_recomp_stack_top && !cpu_interrupt_context_active());
  CHECK(!g_tailcall_context_valid && !g_aot_resume_pc && !c.host_return_valid);
  CHECK(!s_aot_resume_n && !aot_resume_take_handback(0x808888));
  CHECK(ram[0]==4 && c.master_cycles==410 && pending==99 && flushes==4);
  puts("host call context: separate nested task ownership, exact ancestor/return metadata, shared RAM/clocks/APU progress");
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='snes-host-context-')as directory:
            p=Path(directory);c=p/'test.c';exe=p/('test.exe'if os.name=='nt'else'test')
            c.write_text(source)
            subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I'+str(ROOT/'runner/src'),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()
