import sys
out=['// generated from the .dksh files built by uam; not hand-edited', '#pragma once', '#include <stdint.h>']
for name in ('pica_vsh','pica_fsh'):
    d=open('/home/claude/work/nxout/%s.dksh'%name,'rb').read()
    out.append('static const uint8_t %s_dksh[] __attribute__((aligned(256))) = {%s};'%(name, ','.join(str(b) for b in d)))
    out.append('static const uint32_t %s_dksh_size = %d;'%(name,len(d)))
open('/home/claude/work/nxout/dk_shaders.h','w').write('\n'.join(out)+'\n')
