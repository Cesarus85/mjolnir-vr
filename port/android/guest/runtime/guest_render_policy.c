#include "guest_host.h"
#include "android_render_policy.h"

int android_geometry_fixes = 1;
int android_texture_fixes = 1;
int android_render_chip = _android_chip_unknown;

void android_render_policy_initialize(void)
{
	android_render_chip = host_android_chip_family();
	if (android_render_chip < _android_chip_unknown || android_render_chip > _android_chip_other)
		android_render_chip = _android_chip_unknown;
	/* port (Mjolnir VR): never the geometry workarounds (pooled per-draw
	uploads, static buffers, no geometry cache), which upstream keeps for
	Snapdragon phones: the Quest and the Steam Frame are Snapdragon headsets
	that never showed what they work around, and streaming every draw's
	geometry would cost both eyes' draws. The texture fixes stay. */
	android_geometry_fixes = 0;
	android_texture_fixes = android_render_chip != _android_chip_other;
}
