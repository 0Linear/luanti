// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "catch.h"
#include "dummygamedef.h"
#include "light.h"
#include "client/content_mapblock.h"
#include "client/mapblock_mesh.h"
#include "client/meshgen/collector.h"
#include "client/node_visuals.h"
#include <cmath>
#include <cstring>
#include <iostream>

/*
	Mesh generation of a 32^3 node region (client_mesh_chunk = 2) of hilly
	terrain with a lake and caves. This is what the mesh generation thread
	spends its time on, which matters a lot on slow CPUs.
*/

namespace {

content_t addNode(NodeDefManager *ndef, const std::string &name, NodeDrawType drawtype,
		u32 texture, bool transparent = false)
{
	ContentFeatures f;
	f.visuals = std::make_unique<NodeVisuals>();
	f.name = name;
	f.drawtype = drawtype;
	if (transparent) {
		f.alpha = ALPHAMODE_BLEND;
		f.light_propagates = true;
		f.param_type = CPT_LIGHT;
		f.liquid_type = LIQUID_SOURCE;
		f.liquid_alternative_source = name;
		f.liquid_alternative_flowing = name;
	} else {
		f.alpha = ALPHAMODE_OPAQUE;
	}
	if (drawtype == NDT_PLANTLIKE) {
		f.param_type = CPT_LIGHT;
		f.light_propagates = true;
		f.walkable = false;
	}
	for (TileDef &tiledef : f.tiledef)
		tiledef.name = name + ".png";
	for (TileSpec &tile : f.visuals->tiles)
		tile.layers[0].texture_id = texture;
	return ndef->set(name, std::move(f));
}

void fillTerrain(MeshMakeData &data, content_t stone, content_t dirt,
		content_t grass, content_t water, content_t plant)
{
	const s16 n = data.m_side_length;
	const s16 water_level = 12;
	for (s16 z = -3; z < n + 3; z++)
	for (s16 x = -3; x < n + 3; x++) {
		s16 h = 14 + (s16)(6 * std::sin(x * 0.21f) + 5 * std::cos(z * 0.17f + x * 0.05f));
		for (s16 y = -3; y < n + 3; y++) {
			MapNode node(CONTENT_AIR);
			if (y < h - 3)
				node = MapNode(stone);
			else if (y < h)
				node = MapNode(dirt);
			else if (y == h)
				node = MapNode(grass);
			else if (y <= water_level)
				node = MapNode(water);
			else if (y == h + 1 && ((x * 31 + z * 17) % 11) == 0)
				node = MapNode(plant);

			// carve some caves
			if (y < h - 2 && std::sin(x * 0.4f) * std::cos(z * 0.3f) *
					std::sin(y * 0.5f) > 0.45f)
				node = MapNode(CONTENT_AIR);

			// sunlight above ground, dark below
			const u8 light = y > h ? LIGHT_SUN : (y > h - 4 ? 6 : 0);
			node.param1 = light | (light << 4);
			if (node.getContent() == stone || node.getContent() == dirt ||
					node.getContent() == grass)
				node.param1 = 0;
			data.m_vmanip.setNode({x, y, z}, node);
		}
	}
}

/// FNV-1a over the generated geometry, to check that optimizations of the
/// mesh generator don't change its output
u64 hashMesh(const MeshCollector &collector)
{
	u64 h = 0xcbf29ce484222325ULL;
	auto add = [&] (const void *p, size_t n) {
		for (size_t i = 0; i < n; i++)
			h = (h ^ ((const u8 *)p)[i]) * 0x100000001b3ULL;
	};
	for (const auto &layer : collector.prebuffers) {
		for (const auto &buf : layer) {
			add(&buf.layer.texture_id, sizeof(buf.layer.texture_id));
			add(&buf.layer.material_flags, sizeof(buf.layer.material_flags));
			for (const auto &v : buf.vertices) {
				add(&v.Pos, sizeof(v.Pos));
				add(&v.Normal, sizeof(v.Normal));
				u32 c = v.Color.color;
				add(&c, sizeof(c));
				add(&v.TCoords, sizeof(v.TCoords));
			}
			add(buf.indices.data(), buf.indices.size() * sizeof(u16));
		}
	}
	return h;
}

}

TEST_CASE("benchmark_meshgen")
{
	{
		u8 table[LIGHT_SUN + 1] = {
			0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
		};
		memcpy(const_cast<u8 *>(light_decode_table), table, sizeof(table));
	}

	DummyGameDef gamedef;
	NodeDefManager *ndef = gamedef.getWritableNodeDefManager();
	content_t stone = addNode(ndef, "stone", NDT_NORMAL, 1);
	content_t dirt = addNode(ndef, "dirt", NDT_NORMAL, 2);
	content_t grass = addNode(ndef, "grass", NDT_NORMAL, 3);
	content_t water = addNode(ndef, "water", NDT_LIQUID, 4, true);
	content_t plant = addNode(ndef, "plant", NDT_PLANTLIKE, 5);
	ndef->resolveCrossrefs();
	ndef->applyFunction([] (ContentFeatures &f) {
		if (!f.visuals)
			f.visuals = std::make_unique<NodeVisuals>();
	});

	for (int mode = 0; mode < 5; mode++) {
		const bool smooth = !(mode & 1) && mode != 4;
		const bool greedy = mode >= 2;
		const u8 lod = mode == 4;
		MeshMakeData data(ndef, 32, MeshGrid{2});
		data.m_greedy_meshing = greedy;
		data.m_lod = lod;
		data.m_generate_minimap = false;
		data.m_smooth_lighting = smooth;
		data.m_enable_water_reflections = false;
		data.m_blockpos = {0, 0, 0};
		fillTerrain(data, stone, dirt, grass, water, plant);

		{
			MeshCollector collector(v3f(16 * BS), v3f());
			MapblockMeshGenerator(&data, &collector).generate();
			size_t vertices = 0;
			for (const auto &layer : collector.prebuffers)
				for (const auto &buf : layer)
					vertices += buf.vertices.size();
			std::cout << "meshgen smooth=" << smooth << " greedy=" << greedy
				<< " lod=" << (int)lod << ": " << vertices
				<< " vertices, hash " << std::hex << hashMesh(collector)
				<< std::dec << std::endl;
		}

		static const char *names[5] = {"meshgen_32_smooth", "meshgen_32_flat",
				"meshgen_32_smooth_greedy", "meshgen_32_flat_greedy",
				"meshgen_32_lod_greedy"};
		BENCHMARK_ADVANCED(names[mode])(
				Catch::Benchmark::Chronometer meter) {
			meter.measure([&] {
				MeshCollector collector(v3f(16 * BS), v3f());
				MapblockMeshGenerator(&data, &collector).generate();
				return collector.prebuffers[0].size();
			});
		};

		if (mode == 0) {
			std::cout << "side connections: " << std::hex
				<< get_side_connections(&data) << std::dec << std::endl;
			BENCHMARK("side_connections_32") {
				return get_side_connections(&data);
			};
		}
	}
}
