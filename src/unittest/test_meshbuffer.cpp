// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "catch.h"

#include <CMeshBuffer.h>
#include <S3DVertex.h>

TEST_CASE("mesh buffer data release")
{
	auto *buf = new scene::SMeshBuffer();
	video::S3DVertex v[3] = {
		{0, 0, 0, 0, 0, 1, video::SColor(255, 255, 255, 255), 0, 0},
		{1, 0, 0, 0, 0, 1, video::SColor(255, 255, 255, 255), 1, 0},
		{0, 1, 0, 0, 0, 1, video::SColor(255, 255, 255, 255), 0, 1},
	};
	u16 idx[3] = {0, 1, 2};
	buf->append(v, 3, idx, 3);

	SECTION("data is kept by default")
	{
		CHECK(!buf->getVertexBuffer()->isDataReleased());
		CHECK(!buf->Vertices->ReleaseDataAfterUpload);
		CHECK(buf->getVertexCount() == 3);
	}

	SECTION("counts survive releasing the data")
	{
		const u32 prims = buf->getPrimitiveCount();
		const u32 size = buf->getSize();
		buf->Vertices->releaseData();
		buf->Indices->releaseData();

		CHECK(buf->getVertexBuffer()->isDataReleased());
		CHECK(buf->getIndexBuffer()->isDataReleased());
		CHECK(buf->Vertices->Data.capacity() == 0);
		CHECK(buf->Indices->Data.capacity() == 0);
		CHECK(buf->getVertexCount() == 3);
		CHECK(buf->getIndexCount() == 3);
		CHECK(buf->getPrimitiveCount() == prims);
		CHECK(buf->getSize() == size);
	}

	buf->drop();
}
