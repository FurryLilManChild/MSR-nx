// MSR-nx: uam (devkitPro's deko3d shader compiler) as a library for the Switch build: one fragment
// shader from GLSL source to a .dksh file. Built into libuam.a by tools/switch/uam/build.sh.
#include "compiler_iface.h"

extern "C" int msr_uam_compile_frag(const char* glsl, const char* out_path)
{
	DekoCompiler compiler{pipeline_stage_fragment};
	if (!compiler.CompileGlsl(glsl))
		return 0;
	compiler.OutputDksh(out_path);
	return 1;
}
