"""Exercise the production mutable-hook registry through its native boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeMutableHooks(unittest.TestCase):
    def test_native_return_uses_actual_interpreter_watermark(self):
        compiler = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
        if not compiler:
            self.skipTest('C compiler required')
        bridge = (ROOT / 'runner/src/snes/interp_bridge.c').read_text()
        start = bridge.index('enum { kInterpPreOpcodeHookSlots')
        registry = bridge[start:bridge.index('/* BRK bridge seam.', start)]
        start = bridge.index('        if (s_pre_opcode_hook_count > 0) {')
        end = bridge.index('        /* Opt-in control-flow tripwire:', start)
        boundary = bridge[start:end]
        source = '#include <string.h>\n#include "cpu_state.h"\n#include "snes/interp_bridge.h"\n' + registry
        source += r'''
typedef struct { uint16_t sp,pc; uint8_t k; } ProbeInterp;
static void sync_interp_to_cpu(ProbeInterp *in,CpuState *c) { c->S=in->sp; c->PB=in->k; }
static void sync_cpu_to_interp(CpuState *c,ProbeInterp *in) { in->sp=c->S; }
static void bridge_apu_flush(CpuState *c) { (void)c; }
static int ordinary;
static void consume(CpuState *c,uint32_t pc) {
  (void)pc; c->S+=2;
  if(ordinary) interp_bridge_pre_opcode_redirect(0);
  else interp_bridge_pre_opcode_return(0);
}
static int actual_boundary(CpuState *cpu,uint16_t s_enter,uint32_t yield_pc,int stop_on_rti,uint32_t *out_return_pc) {
  ProbeInterp in={cpu->S,0x8100,0};
  for(unsigned n=0;n<3;++n) {
    uint32_t pc_before=((uint32_t)in.k<<16)|in.pc;
'''
        source += boundary
        source += r'''
    return 2; /* ordinary opcode would execute */
  }
  return 3;
}
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1; } } while(0)
int main(void) {
  CpuState c={0}; uint32_t ret=0xabcdef;
  CHECK(interp_bridge_add_pre_opcode_hook(0x8100,consume));
  c.S=0x100; CHECK(actual_boundary(&c,0x100,0,0,&ret)==1 && c.S==0x102 && ret==0);
  c.S=0xfe; ret=0xabcdef; CHECK(actual_boundary(&c,0x100,0,0,&ret)==2 && c.S==0x100 && ret==0xabcdef);
  ordinary=1; c.S=0x100; CHECK(actual_boundary(&c,0x100,0,0,&ret)==2);
  ordinary=0; c.S=0x100; CHECK(actual_boundary(&c,0x100,0x8121,0,&ret)==2);
  c.S=0x100; CHECK(actual_boundary(&c,0x100,0,1,&ret)==2);
  CHECK(!s_pre_opcode_redirect_is_return);
  puts("native returns: real boundary honors inherited frame, zero PC, nested depth and interrupt ownership");
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='snes-native-return-') as directory:
            path = Path(directory)
            c, exe = path/'test.c', path/('test.exe' if os.name=='nt' else 'test')
            c.write_text(source)
            subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-I'+str(ROOT/'runner/src'),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_actual_registry_mutation_composition_and_redirects(self):
        compiler = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
        if not compiler:
            self.skipTest('C compiler required')
        bridge = (ROOT / 'runner/src/snes/interp_bridge.c').read_text()
        start = bridge.index('enum { kInterpPreOpcodeHookSlots')
        end = bridge.index('/* BRK bridge seam.', start)
        # Compile the actual production registry and functions with CpuState's
        # real header, without bringing the unrelated interpreter into the test.
        source = '#include <string.h>\n#include "cpu_state.h"\n#include "snes/interp_bridge.h"\n'
        source += bridge[start:end]
        source += r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static unsigned calls;
static int nested_ok;
static void first(CpuState *c,uint32_t pc) { (void)pc; c->A++; c->ram[0]++; calls++; }
static void second(CpuState *c,uint32_t pc) { (void)pc; c->X+=2; calls++; }
static void replace(CpuState *c,uint32_t pc) { (void)pc; c->A+=10; calls++; }
static void redirect(CpuState *c,uint32_t pc) { (void)c;(void)pc; calls++; interp_bridge_pre_opcode_redirect(0x808100); }
static void zero_redirect(CpuState *c,uint32_t pc) { (void)c;(void)pc; interp_bridge_pre_opcode_redirect(0); }
static void nested(CpuState *c,uint32_t pc) {
  (void)pc; uint32_t target=0; interp_bridge_pre_opcode_redirect(0x808200);
  nested_ok=interp_bridge_run_native_pre_opcode_hooks(c,0x809000,&target,NULL,NULL) && target==0x808100;
}
int main(void) {
  CpuState cpu={0}; uint8_t ram[0x20000]={0}; cpu.ram=ram;
  uint32_t target=0;
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,first));
  CHECK(interp_bridge_add_pre_opcode_hook(0x808000,first)); /* idempotent mirror */
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,second));
  CHECK(!interp_bridge_run_native_pre_opcode_hooks(&cpu,0x808000,&target,NULL,NULL));
  CHECK(calls==2 && cpu.A==1 && cpu.X==2 && ram[0]==1);
  bool invoked=true;
  CHECK(!interp_bridge_run_native_pre_opcode_hooks(&cpu,0x018000,&target,&invoked,NULL));
  CHECK(!invoked);
  CHECK(calls==2); /* PC16 filter collisions never change full-key identity */
  interp_bridge_set_pre_opcode_hook(0x008000,replace);
  CHECK(!interp_bridge_run_native_pre_opcode_hooks(&cpu,0x008000,&target,NULL,NULL));
  CHECK(calls==4 && cpu.A==11 && cpu.X==4);
  interp_bridge_set_pre_opcode_hook(0,NULL);
  CHECK(!interp_bridge_run_native_pre_opcode_hooks(&cpu,0x808000,&target,NULL,NULL) && calls==4);
  CHECK(!interp_bridge_add_pre_opcode_hook(0x008000,NULL));
  for(unsigned i=0;i<1024;++i) CHECK(interp_bridge_add_pre_opcode_hook(0x008000+i,first));
  CHECK(!interp_bridge_add_pre_opcode_hook(0x009000,first));
  CHECK(!interp_bridge_run_native_pre_opcode_hooks(&cpu,0x80812c,&target,NULL,NULL) && calls==5);
  interp_bridge_set_pre_opcode_hook(0,NULL);
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,redirect));
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,second));
  CHECK(interp_bridge_run_native_pre_opcode_hooks(&cpu,0x808000,&target,NULL,NULL) && target==0x808100);
  CHECK(calls==6); /* redirect stops later callbacks */
  interp_bridge_set_pre_opcode_hook(0,NULL);
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,nested));
  CHECK(interp_bridge_add_pre_opcode_hook(0x009000,redirect));
  CHECK(interp_bridge_run_native_pre_opcode_hooks(&cpu,0x808000,&target,NULL,NULL));
  CHECK(nested_ok && target==0x808200 && calls==7);
  interp_bridge_set_pre_opcode_hook(0,NULL);
  CHECK(!interp_bridge_native_hooks_armed(0x808000));
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,zero_redirect));
  target=0xabcdef; invoked=false;
  CHECK(interp_bridge_run_native_pre_opcode_hooks(&cpu,0x808000,&target,&invoked,NULL));
  CHECK(invoked && target==0);
  interp_bridge_set_pre_opcode_hook(0,NULL); calls=0; cpu.A=cpu.X=ram[0]=0;
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,first));
  CHECK(interp_bridge_add_pre_opcode_hook(0x008000,second));
  CHECK(!interp_bridge_run_native_pre_opcode_hooks(&cpu,0x008000,&target,&invoked,first));
  CHECK(invoked && calls==1 && cpu.A==0 && cpu.X==2 && ram[0]==0);
  puts("native hooks: mutation, composition, replacement, capacity, identity and nested redirects passed");
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='snes-native-hooks-') as directory:
            path = Path(directory)
            c = path / 'test.c'
            exe = path / ('test.exe' if os.name == 'nt' else 'test')
            c.write_text(source)
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-I' + str(ROOT / 'runner/src'), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
