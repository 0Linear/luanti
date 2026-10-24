// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2023 Vitaliy Lobachevskiy

#include "test.h"

#include "gamedef.h"
#include "inventory.h" // ItemStack
#include "dummygamedef.h"
#include "client/content_mapblock.h"
#include "client/mapblock_mesh.h"
#include "client/meshgen/collector.h"
#include "client/node_visuals.h"
#include <memory>
#include "mesh_compare.h"

namespace {

class MockGameDef : public DummyGameDef {
public:
	IWritableItemDefManager *item_mgr() noexcept {
		return static_cast<IWritableItemDefManager *>(m_itemdef);
	}

	NodeDefManager *node_mgr() noexcept {
		return const_cast<NodeDefManager *>(m_nodedef);
	}

	content_t registerNode(const ItemDefinition &itemdef, ContentFeatures &&nodedef) {
		item_mgr()->registerItem(itemdef);

		NodeDefManager *mgr = node_mgr();
		content_t id = mgr->set(nodedef.name, std::move(nodedef));

		return id;
	}

	void finalize() {
		node_mgr()->resolveCrossrefs();

		// Need to fill node visuals for predefined nodes
		node_mgr()->applyFunction([] (ContentFeatures &f) {
			if (!f.visuals)
				f.visuals = std::make_unique<NodeVisuals>();
		});
	}

	MeshMakeData makeSingleNodeMMD(bool smooth_lighting = true)
	{
		MeshMakeData data{ndef(), 1, MeshGrid{1}};
		data.m_generate_minimap = false;
		data.m_smooth_lighting = smooth_lighting;
		data.m_enable_water_reflections = false;
		data.m_blockpos = {0, 0, 0};
		// MapblockMeshGenerator needs a margin of at least 3
		for (s16 x = -3; x <= 3; x++)
		for (s16 y = -3; y <= 3; y++)
		for (s16 z = -3; z <= 3; z++)
			data.m_vmanip.setNode({x, y, z}, {CONTENT_AIR, 0, 0});
		return data;
	}

	content_t addSimpleNode(std::string name, u32 texture)
	{
		ItemDefinition itemdef;
		itemdef.type = ITEM_NODE;
		itemdef.name = "test:" + name;
		itemdef.description = name;

		ContentFeatures f;
		f.visuals = std::make_unique<NodeVisuals>();
		f.name = itemdef.name;
		f.drawtype = NDT_NORMAL;
		f.alpha = ALPHAMODE_OPAQUE;
		for (TileDef &tiledef : f.tiledef)
			tiledef.name = name + ".png";
		for (TileSpec &tile : f.visuals->tiles)
			tile.layers[0].texture_id = texture;

		return registerNode(itemdef, std::move(f));
	}

	content_t addLiquidSource(std::string name, u32 texture)
	{
		ItemDefinition itemdef;
		itemdef.type = ITEM_NODE;
		itemdef.name = "test:" + name + "_source";
		itemdef.description = name;

		ContentFeatures f;
		f.visuals = std::make_unique<NodeVisuals>();
		f.name = itemdef.name;
		f.drawtype = NDT_LIQUID;
		f.alpha = ALPHAMODE_BLEND;
		f.light_propagates = true;
		f.param_type = CPT_LIGHT;
		f.liquid_type = LIQUID_SOURCE;
		f.liquid_viscosity = 4;
		f.groups["liquids"] = 3;
		f.liquid_alternative_source = "test:" + name + "_source";
		f.liquid_alternative_flowing = "test:" + name + "_flowing";
		for (TileDef &tiledef : f.tiledef)
			tiledef.name = name + ".png";
		for (TileSpec &tile : f.visuals->tiles)
			tile.layers[0].texture_id = texture;

		return registerNode(itemdef, std::move(f));
	}

	content_t addLiquidFlowing(std::string name, u32 texture_top, u32 texture_side)
	{
		ItemDefinition itemdef;
		itemdef.type = ITEM_NODE;
		itemdef.name = "test:" + name + "_flowing";
		itemdef.description = name;

		ContentFeatures f;
		f.visuals = std::make_unique<NodeVisuals>();
		f.name = itemdef.name;
		f.drawtype = NDT_FLOWINGLIQUID;
		f.alpha = ALPHAMODE_BLEND;
		f.light_propagates = true;
		f.param_type = CPT_LIGHT;
		f.liquid_type = LIQUID_FLOWING;
		f.liquid_viscosity = 4;
		f.groups["liquids"] = 3;
		f.liquid_alternative_source = "test:" + name + "_source";
		f.liquid_alternative_flowing = "test:" + name + "_flowing";
		f.tiledef_special[0].name = name + "_top.png";
		f.tiledef_special[1].name = name + "_side.png";
		f.visuals->special_tiles[0].layers[0].texture_id = texture_top;
		f.visuals->special_tiles[1].layers[0].texture_id = texture_side;

		return registerNode(itemdef, std::move(f));
	}
};

void set_light_decode_table()
{
	u8 table[LIGHT_SUN + 1] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
	};
	memcpy(const_cast<u8 *>(light_decode_table), table, sizeof(table));
}

class TestMapblockMeshGenerator : public TestBase {
public:
	TestMapblockMeshGenerator() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestMapblockMeshGenerator"; }

	void runTests(IGameDef *gamedef) override;
	void testSimpleNode();
	void testSurroundedNode();
	void testInterliquidSame();
	void testInterliquidDifferent();
	void testGreedyMeshing();
	void testSideConnections();
};

static TestMapblockMeshGenerator g_test_instance;

void TestMapblockMeshGenerator::runTests(IGameDef *gamedef)
{
	set_light_decode_table();
	TEST(testSimpleNode);
	TEST(testSurroundedNode);
	TEST(testInterliquidSame);
	TEST(testInterliquidDifferent);
	TEST(testGreedyMeshing);
	TEST(testSideConnections);
}

namespace quad {
	constexpr float h = BS / 2.0f;
	const Quad zp{{{{-h, -h, h}, {0, 0, 1}, 0, {1, 1}}, {{h, -h, h}, {0, 0, 1}, 0, {0, 1}}, {{h, h, h}, {0, 0, 1}, 0, {0, 0}}, {{-h, h, h}, {0, 0, 1}, 0, {1, 0}}}};
	const Quad yp{{{{-h, h, -h}, {0, 1, 0}, 0, {0, 1}}, {{-h, h, h}, {0, 1, 0}, 0, {0, 0}}, {{h, h, h}, {0, 1, 0}, 0, {1, 0}}, {{h, h, -h}, {0, 1, 0}, 0, {1, 1}}}};
	const Quad xp{{{{h, -h, -h}, {1, 0, 0}, 0, {0, 1}}, {{h, h, -h}, {1, 0, 0}, 0, {0, 0}}, {{h, h, h}, {1, 0, 0}, 0, {1, 0}}, {{h, -h, h}, {1, 0, 0}, 0, {1, 1}}}};
	const Quad zn{{{{-h, -h, -h}, {0, 0, -1}, 0, {0, 1}}, {{-h, h, -h}, {0, 0, -1}, 0, {0, 0}}, {{h, h, -h}, {0, 0, -1}, 0, {1, 0}}, {{h, -h, -h}, {0, 0, -1}, 0, {1, 1}}}};
	const Quad yn{{{{-h, -h, -h}, {0, -1, 0}, 0, {0, 0}}, {{h, -h, -h}, {0, -1, 0}, 0, {1, 0}}, {{h, -h, h}, {0, -1, 0}, 0, {1, 1}}, {{-h, -h, h}, {0, -1, 0}, 0, {0, 1}}}};
	const Quad xn{{{{-h, -h, -h}, {-1, 0, 0}, 0, {1, 1}}, {{-h, -h, h}, {-1, 0, 0}, 0, {0, 1}}, {{-h, h, h}, {-1, 0, 0}, 0, {0, 0}}, {{-h, h, -h}, {-1, 0, 0}, 0, {1, 0}}}};
}

void TestMapblockMeshGenerator::testSimpleNode()
{
	MockGameDef gamedef;
	content_t stone = gamedef.addSimpleNode("stone", 42);
	gamedef.finalize();

	MeshMakeData data = gamedef.makeSingleNodeMMD();
	data.m_vmanip.setNode({0, 0, 0}, {stone, 0, 0});

	MeshCollector col{{}};
	MapblockMeshGenerator mg{&data, &col};
	mg.generate();
	UASSERTEQ(std::size_t, col.prebuffers[0].size(), 1);
	UASSERTEQ(std::size_t, col.prebuffers[1].size(), 0);

	auto &&buf = col.prebuffers[0][0];
	UASSERTEQ(u32, buf.layer.texture_id, 42);
	UASSERT(checkMeshEqual(buf.vertices, buf.indices, {quad::xn, quad::xp, quad::yn, quad::yp, quad::zn, quad::zp}));
}

void TestMapblockMeshGenerator::testSurroundedNode()
{
	MockGameDef gamedef;
	content_t stone = gamedef.addSimpleNode("stone", 42);
	content_t wood = gamedef.addSimpleNode("wood", 13);
	gamedef.finalize();

	MeshMakeData data = gamedef.makeSingleNodeMMD();
	data.m_vmanip.setNode({0, 0, 0}, {stone, 0, 0});
	data.m_vmanip.setNode({1, 0, 0}, {wood, 0, 0});

	MeshCollector col{{}};
	MapblockMeshGenerator mg{&data, &col};
	mg.generate();
	UASSERTEQ(std::size_t, col.prebuffers[0].size(), 1);
	UASSERTEQ(std::size_t, col.prebuffers[1].size(), 0);

	auto &&buf = col.prebuffers[0][0];
	UASSERTEQ(u32, buf.layer.texture_id, 42);
	UASSERT(checkMeshEqual(buf.vertices, buf.indices, {quad::xn, quad::yn, quad::yp, quad::zn, quad::zp}));
}

void TestMapblockMeshGenerator::testInterliquidSame()
{
	MockGameDef gamedef;
	auto water = gamedef.addLiquidSource("water", 42);
	gamedef.finalize();

	MeshMakeData data = gamedef.makeSingleNodeMMD();
	data.m_vmanip.setNode({0, 0, 0}, {water, 0, 0});
	data.m_vmanip.setNode({1, 0, 0}, {water, 0, 0});

	MeshCollector col{{}};
	MapblockMeshGenerator mg{&data, &col};
	mg.generate();
	UASSERTEQ(std::size_t, col.prebuffers[0].size(), 1);
	UASSERTEQ(std::size_t, col.prebuffers[1].size(), 0);

	auto &&buf = col.prebuffers[0][0];
	UASSERTEQ(u32, buf.layer.texture_id, 42);
	UASSERT(checkMeshEqual(buf.vertices, buf.indices, {quad::xn, quad::yn, quad::yp, quad::zn, quad::zp}));
}

void TestMapblockMeshGenerator::testInterliquidDifferent()
{
	MockGameDef gamedef;
	auto water = gamedef.addLiquidSource("water", 42);
	auto lava = gamedef.addLiquidSource("lava", 13);
	gamedef.finalize();

	MeshMakeData data = gamedef.makeSingleNodeMMD();
	data.m_vmanip.setNode({0, 0, 0}, {water, 0, 0});
	data.m_vmanip.setNode({0, 0, 1}, {lava, 0, 0});

	MeshCollector col{{}};
	MapblockMeshGenerator mg{&data, &col};
	mg.generate();
	UASSERTEQ(std::size_t, col.prebuffers[0].size(), 1);
	UASSERTEQ(std::size_t, col.prebuffers[1].size(), 0);

	auto &&buf = col.prebuffers[0][0];
	UASSERTEQ(u32, buf.layer.texture_id, 42);
	UASSERT(checkMeshEqual(buf.vertices, buf.indices, {quad::xn, quad::xp, quad::yn, quad::yp, quad::zn, quad::zp}));
}

}

static MeshMakeData makeAreaMMD(const NodeDefManager *ndef, u16 side, content_t fill)
{
	MeshMakeData data{ndef, side, MeshGrid{1}};
	data.m_generate_minimap = false;
	data.m_smooth_lighting = false;
	data.m_enable_water_reflections = false;
	data.m_blockpos = {0, 0, 0};
	for (s16 x = -3; x < side + 3; x++)
	for (s16 y = -3; y < side + 3; y++)
	for (s16 z = -3; z < side + 3; z++)
		data.m_vmanip.setNode({x, y, z}, {fill, 0, 0});
	return data;
}

void TestMapblockMeshGenerator::testGreedyMeshing()
{
	MockGameDef gamedef;
	content_t stone = gamedef.addSimpleNode("stone", 42);
	gamedef.finalize();

	// A 4x1x3 slab of stone
	auto generate = [&] (bool greedy) {
		MeshMakeData data = makeAreaMMD(gamedef.ndef(), 4, CONTENT_AIR);
		data.m_greedy_meshing = greedy;
		for (s16 x = 0; x < 4; x++)
		for (s16 z = 0; z < 3; z++)
			data.m_vmanip.setNode({x, 0, z}, {stone, 0, 0});
		MeshCollector col{{}};
		MapblockMeshGenerator(&data, &col).generate();
		UASSERTEQ(std::size_t, col.prebuffers[0].size(), 1);
		return col.prebuffers[0][0];
	};
	auto plain = generate(false);
	auto merged = generate(true);
	UASSERTEQ(std::size_t, plain.vertices.size(), 4 * (12 + 12 + 4 + 4 + 3 + 3));
	// One quad per side
	UASSERTEQ(std::size_t, merged.vertices.size(), 4 * 6);
	UASSERTEQ(std::size_t, merged.indices.size(), 6 * 6);

	// The merged mesh covers the same box and repeats the texture once per node
	aabb3f box_plain(plain.vertices[0].Pos), box_merged(merged.vertices[0].Pos);
	for (auto &v : plain.vertices)
		box_plain.addInternalPoint(v.Pos);
	for (auto &v : merged.vertices)
		box_merged.addInternalPoint(v.Pos);
	UASSERT(box_plain.MinEdge == box_merged.MinEdge);
	UASSERT(box_plain.MaxEdge == box_merged.MaxEdge);
	for (u32 q = 0; q < 6; q++) {
		const video::S3DVertex *v = &merged.vertices[4 * q];
		v3f size = v[2].Pos - v[0].Pos;
		f32 area = std::fabs(size.X * size.Y) + std::fabs(size.X * size.Z) +
				std::fabs(size.Y * size.Z);
		v2f uv_size = v[2].TCoords - v[0].TCoords;
		UASSERT(std::fabs(std::fabs(uv_size.X * uv_size.Y) * BS * BS - area) < 0.01f);
	}
}

void TestMapblockMeshGenerator::testSideConnections()
{
	MockGameDef gamedef;
	content_t stone = gamedef.addSimpleNode("stone", 42);
	gamedef.finalize();
	const NodeDefManager *ndef = gamedef.ndef();

	{
		MeshMakeData data = makeAreaMMD(ndef, 16, CONTENT_AIR);
		UASSERTEQ(u16, get_side_connections(&data), SIDE_CONNECTIONS_ALL);
	}
	{
		MeshMakeData data = makeAreaMMD(ndef, 16, stone);
		UASSERTEQ(u16, get_side_connections(&data), 0);
	}
	{
		// A straight tunnel along X
		MeshMakeData data = makeAreaMMD(ndef, 16, stone);
		for (s16 x = 0; x < 16; x++)
			data.m_vmanip.setNode({x, 8, 8}, {CONTENT_AIR, 0, 0});
		UASSERTEQ(u16, get_side_connections(&data), side_connection_bit(0, 1));
	}
	{
		// A bent tunnel from -X to +Y, plus a closed cave
		MeshMakeData data = makeAreaMMD(ndef, 16, stone);
		for (s16 x = 0; x <= 5; x++)
			data.m_vmanip.setNode({x, 3, 7}, {CONTENT_AIR, 0, 0});
		for (s16 y = 3; y < 16; y++)
			data.m_vmanip.setNode({5, y, 7}, {CONTENT_AIR, 0, 0});
		for (s16 z = 2; z < 14; z++)
			data.m_vmanip.setNode({10, 10, z}, {CONTENT_AIR, 0, 0});
		UASSERTEQ(u16, get_side_connections(&data), side_connection_bit(0, 3));
	}
	{
		// An open pocket at a single side connects nothing
		MeshMakeData data = makeAreaMMD(ndef, 16, stone);
		data.m_vmanip.setNode({0, 4, 4}, {CONTENT_AIR, 0, 0});
		UASSERTEQ(u16, get_side_connections(&data), 0);
	}
}
