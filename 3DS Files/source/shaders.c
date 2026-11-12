// The shared in-world shader blob (TASKS.md #352); see shaders.h.
#include "shaders.h"
#include "worldsh_shbin.h"

static DVLB_s* s_blob;
static bool s_tried;

DVLE_s* shaders_get(ShaderId id) {
	if (!s_tried) {
		s_tried = true;
		s_blob = DVLB_ParseFile((u32*)worldsh_shbin, worldsh_shbin_size);
		if (s_blob && s_blob->numDVLE != SHADER_COUNT) {
			// The Makefile's SHADER_GROUP and ShaderId disagree: refuse every
			// program rather than hand one the wrong code.
			DVLB_Free(s_blob);
			s_blob = NULL;
		}
	}
	if (!s_blob || id < 0 || id >= SHADER_COUNT)
		return NULL;
	return &s_blob->DVLE[id];
}
