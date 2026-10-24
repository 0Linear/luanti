// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "catch.h"
#include "noise.h"
#include <cmath>
#include <iostream>

/*
	Noise maps of the size that map generation uses (one 80^3 chunk),
	with the plain and the separable interpolation.
*/

TEST_CASE("benchmark_noise")
{
	// Like mgv7's mountain and terrain noises
	NoiseParams np3d(-0.6f, 1.0f, v3f(250, 350, 250), 5333, 5, 0.63f, 2.0f);
	NoiseParams np2d(4.0f, 70.0f, v3f(600, 600, 600), 82341, 5, 0.6f, 2.0f);

	for (bool separable : {false, true}) {
		Noise n3(&np3d, 1234, 80, 82, 80);
		Noise n2(&np2d, 1234, 80, 80);
		n3.separable = separable;
		n2.separable = separable;

		BENCHMARK(separable ? "noise3d_80_separable" : "noise3d_80_plain") {
			return n3.noiseMap3D(-80, -40, 160)[0];
		};
		BENCHMARK(separable ? "noise2d_80_separable" : "noise2d_80_plain") {
			return n2.noiseMap2D(-80, 160)[0];
		};
	}

	// Both must give the same maps, up to rounding
	Noise a(&np3d, 1234, 80, 82, 80), b(&np3d, 1234, 80, 82, 80);
	a.separable = false;
	b.separable = true;
	const float *ma = a.noiseMap3D(-80, -40, 160);
	const float *mb = b.noiseMap3D(-80, -40, 160);
	float max_diff = 0;
	for (u32 i = 0; i < 80 * 82 * 80; i++)
		max_diff = std::max(max_diff, std::fabs(ma[i] - mb[i]));
	std::cout << "noise3d max difference: " << max_diff << std::endl;
	CHECK(max_diff < 1e-4f);

	Noise c(&np2d, 1234, 80, 80), d(&np2d, 1234, 80, 80);
	c.separable = false;
	d.separable = true;
	ma = c.noiseMap2D(-80, 160);
	mb = d.noiseMap2D(-80, 160);
	max_diff = 0;
	for (u32 i = 0; i < 80 * 80; i++)
		max_diff = std::max(max_diff, std::fabs(ma[i] - mb[i]));
	std::cout << "noise2d max difference: " << max_diff << std::endl;
	CHECK(max_diff < 1e-3f);
}
