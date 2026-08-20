/**
 * @file asVolumetricCompositeF.glsl
 * @brief AyaneStorm optional volumetric lighting - upsample and additive composite.
 * @author chanayane@firestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D emissiveRect; // half-res volumetric scatter target

void main()
{
    // Plain bilinear upsample. A depth-aware (bilateral) variant was tried
    // to reduce color bleed across opaque-geometry silhouettes, but the
    // deferred depth buffer has no notion of alpha-blended surfaces (hair,
    // foliage): getDepth() there returns whatever is behind them, which made
    // the bilateral weighting glow/over-brighten hair against a bright
    // background instead. Reverted rather than continuing to tune weight
    // constants against a structurally blind comparison - see the plan doc's
    // "Regression found in-game" entry for the full history.
    frag_color = vec4(texture(emissiveRect, vary_fragcoord).rgb, 0.0);
}
